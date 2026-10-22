#include "kernel/sched.h"

#include "core/console.h"
#include "core/errno.h"
#include "kernel/irq.h"
#include "kernel/klock.h"
#include "kernel/mem.h"
#include "kernel/panic.h"
#include "kernel/proc.h"

#define SLICE_TICKS 10u          /* 10 ms at TIME_HZ = 1000 */
#define SWITCH_FRAME_WORDS 10u   /* r4-r12, lr: arm_switch's frame */
#define SWITCH_FRAME_LR 9u

void arm_switch(uint32_t *save_sp, uint32_t new_sp);
void arm_thread_start(void);

#include "arch/arm/smp.h"

static struct task tasks[SCHED_MAX_TASKS];

/* Which task each core is running. The first SMP_MAX_CPUS entries of tasks[]
 * are the cores' idle contexts — the context a core is on when it has
 * nothing to run — so a core always has a `current` to save state into. */
static struct task *cpu_current[SMP_MAX_CPUS];

#define current (*current_slot())

static struct task **current_slot(void) {
    uint32_t cpu = smp_cpu_id();
    return &cpu_current[cpu < SMP_MAX_CPUS ? cpu : 0];
}

/* The task each core is in the middle of leaving, and whether it should go
 * back on the run queue once it has been left.
 *
 * A task stops being a core's `current` at the top of switch_to, but the
 * core is still using it until `arm_switch` has saved its registers — and it
 * is still standing on its kernel stack. For that window the task is owned
 * by neither the queue nor a core, which is exactly long enough for another
 * core to pop it (two cores, one stack) or to reap it (a freed stack under a
 * running core: the crash this pair of variables exists to prevent). Both
 * are answered the same way: a task recorded here is not available to
 * anybody, and the far side of the switch releases it. */
static struct task *leaving[SMP_MAX_CPUS];
static bool requeue_on_leave[SMP_MAX_CPUS];

/* The first SMP_MAX_CPUS tasks are the cores' idle contexts, one each.
 *
 * They are never on the run queue and never scheduled by another core: two
 * cores running one idle context would be two cores on one stack. Every
 * loop over tasks[] therefore starts past them, and every "is this the idle
 * task" test asks about the range rather than about tasks[0] — which is
 * what the single-core version did, and what made a secondary push its own
 * idle context onto the shared queue. */
#define FIRST_REAL_TASK SMP_MAX_CPUS

static bool is_idle(const struct task *t) {
    return t >= &tasks[0] && t < &tasks[FIRST_REAL_TASK];
}

/* This core's idle context: where it goes when there is nothing to run. */
static struct task *my_idle(void) {
    return &tasks[smp_cpu_id() % SMP_MAX_CPUS];
}

/* True when any core is running this task, which is what makes it unsafe
 * to free. */
/* True while any core is running this task **or has not finished leaving
 * it**, which is what makes it unsafe to free. The second half is not
 * pedantry: freeing a task releases the kernel stack a core is still
 * standing on inside arm_switch, and the fault lands in whatever syscall
 * runs next. */
static bool running_somewhere(const struct task *t) {
    for (uint32_t cpu = 0; cpu < SMP_MAX_CPUS; cpu++) {
        if (cpu_current[cpu] == t || leaving[cpu] == t) {
            return true;
        }
    }
    return false;
}
static struct task *run_head, *run_tail;
static uint32_t next_tid = 1;
static bool need_resched;

static void set_tpidruro(uint32_t v) {
    __asm__ volatile("mcr p15, 0, %0, c13, c0, 3" : : "r"(v));
}

static void runq_push(struct task *t) {
    t->run_next = NULL;
    if (run_tail != NULL) {
        run_tail->run_next = t;
    } else {
        run_head = t;
    }
    run_tail = t;
}

/* Recomputes the tail after a removal from anywhere in the queue. */
static void runq_retail(void) {
    struct task *t = run_head;
    run_tail = NULL;
    while (t != NULL) {
        run_tail = t;
        t = t->run_next;
    }
}

/* The first task on the queue that no core still holds.
 *
 * A task can be on the queue and still owned by a core: `sched_wake` pushes
 * a sleeper as soon as its reason to sleep is gone, and the core it went to
 * sleep on may not have finished switching away from it yet. Handing it to a
 * second core then puts two cores on one kernel stack, and the corruption
 * surfaces as a fault in whatever unrelated syscall runs next — which is how
 * this was found, as a NULL process in nanosleep. */
static struct task *runq_pop(void) {
    struct task **link = &run_head;
    while (*link != NULL) {
        struct task *t = *link;
        if (!running_somewhere(t)) {
            *link = t->run_next;
            runq_retail();
            return t;
        }
        link = &t->run_next;
    }
    return NULL;
}

static void runq_remove(struct task *t) {
    struct task **link = &run_head;
    run_tail = NULL;
    while (*link != NULL) {
        if (*link == t) {
            *link = t->run_next;
            continue;
        }
        run_tail = *link;
        link = &(*link)->run_next;
    }
}

/* Runs as the resumed task, on the core that resumed it: the task that core
 * left is now saved and off its stack, so it is safe for anyone to run —
 * or, if it has ended, to free.
 *
 * Every path that resumes a task must run this, which is why it is exported:
 * a thread started for the first time comes up in `arm_thread_start` and goes
 * straight out to user mode, never through the tail of switch_to. Missing it
 * there leaves the displaced task on no queue and no core — a lost wakeup
 * that strands the whole machine once the other cores run out of work, and
 * one that appears in maybe one boot in six. */
void sched_finish_switch(void) {
    uint32_t cpu = smp_cpu_id() % SMP_MAX_CPUS;
    struct task *left = leaving[cpu];
    bool back = requeue_on_leave[cpu];
    leaving[cpu] = NULL;
    requeue_on_leave[cpu] = false;
    if (left != NULL && back) {
        runq_push(left);
    }
}

static void free_task(struct task *t) {
    page_free(t->kstack, KSTACK_PAGES);
    *t = (struct task){.state = TASK_UNUSED};
}

/* Frees exited tasks; never the one running (its stack is in use). */
static void reap_zombies(void) {
    for (uint32_t i = FIRST_REAL_TASK; i < SCHED_MAX_TASKS; i++) {
        if (tasks[i].state == TASK_ZOMBIE && !running_somewhere(&tasks[i])) {
            free_task(&tasks[i]);
        }
    }
}

static void switch_to(struct task *next) {
    struct task *prev = current;
    if (next == prev) {
        return;
    }
    if (prev->proc != NULL && prev->state != TASK_ZOMBIE) {
        arm_vfp_save(&prev->vfp);
    }
    proc_switch_mm(prev->state == TASK_ZOMBIE ? NULL : prev->proc, next->proc);
    if (next->proc != NULL) {
        arm_vfp_restore(&next->vfp);
        set_tpidruro(next->tls);
    }
    current = next;
    next->slice_left = SLICE_TICKS;
    /* Hand prev to the far side of the switch, which is the only place that
     * knows its state has been saved and its stack left. */
    leaving[smp_cpu_id() % SMP_MAX_CPUS] = prev;
    arm_switch(&prev->saved_sp, next->saved_sp);
    sched_finish_switch();
    reap_zombies();
}

void sched_init(void) {
    for (uint32_t cpu = 0; cpu < SMP_MAX_CPUS; cpu++) {
        tasks[cpu] = (struct task){.tid = 0, .state = TASK_RUNNABLE};
        cpu_current[cpu] = &tasks[cpu];
    }
}

/* What a core calls once, before it starts scheduling: it adopts its own
 * idle context. The primary did this in sched_init; a secondary does it
 * when it is let into the kernel (arch/arm/smp.h). */
void sched_init_cpu(void) {
    current = &tasks[smp_cpu_id() % SMP_MAX_CPUS];
}

static struct task *free_slot(void) {
    for (uint32_t i = FIRST_REAL_TASK; i < SCHED_MAX_TASKS; i++) {
        if (tasks[i].state == TASK_UNUSED) {
            return &tasks[i];
        }
    }
    return NULL;
}

struct trapframe *sched_trapframe(struct task *t) {
    return (struct trapframe *)(t->kstack + KSTACK_PAGES * 4096u) - 1;
}

struct task *sched_new_task(struct proc *p) {
    struct task *t = free_slot();
    uint32_t *frame;
    if (t == NULL || (t->kstack = page_alloc(KSTACK_PAGES, 1)) == NULL) {
        return NULL;
    }
    t->tid = next_tid++;
    t->proc = p;
    t->state = TASK_SLEEPING;  /* not runnable until sched_start */
    frame = (uint32_t *)sched_trapframe(t) - SWITCH_FRAME_WORDS;
    frame[SWITCH_FRAME_LR] = (uint32_t)(uintptr_t)arm_thread_start;
    t->saved_sp = (uint32_t)(uintptr_t)frame;
    return t;
}

void sched_start(struct task *t) {
    t->state = TASK_RUNNABLE;
    runq_push(t);
}

struct task *sched_current(void) {
    return current;
}

/* Gives up the core.
 *
 * A task that is still runnable is put back on the queue by finish_switch,
 * on the far side of the switch — never before it, because until arm_switch
 * has saved its state another core that popped it would run it on the same
 * stack.
 *
 * With nothing else to run, a runnable task simply carries on; one that has
 * gone to sleep or died cannot, and goes to this core's idle context. */
void sched_yield(void) {
    struct task *next = runq_pop();
    bool runnable = !is_idle(current) && current->state == TASK_RUNNABLE;
    if (next == NULL) {
        if (runnable || is_idle(current)) {
            return;
        }
        next = my_idle();
    }
    requeue_on_leave[smp_cpu_id() % SMP_MAX_CPUS] = runnable;
    switch_to(next);
}

int sched_sleep(const void *chan, uint32_t key, uint64_t deadline_ns, bool interruptible) {
    struct task *t = current;
    if (is_idle(t)) {
        panic("sched: the idle task cannot sleep");
    }
    if (t->killed) {
        return KERR_INTR; /* it is on its way out: it must not start waiting */
    }
    if (interruptible && (t->sig_pending & ~t->sig_blocked) != 0) {
        return KERR_INTR;
    }
    t->state = TASK_SLEEPING;
    t->wait_chan = chan;
    t->wait_key = key;
    t->wake_at_ns = deadline_ns;
    t->interruptible = interruptible;
    t->wait_result = 0;
    sched_yield();
    return t->wait_result;
}

static void wake(struct task *t, int result) {
    t->wait_result = result;
    t->wait_chan = NULL;
    t->state = TASK_RUNNABLE;
    runq_push(t);
}

uint32_t sched_wake(const void *chan, uint32_t key, uint32_t max) {
    uint32_t n = 0;
    for (uint32_t i = FIRST_REAL_TASK; i < SCHED_MAX_TASKS && n < max; i++) {
        struct task *t = &tasks[i];
        if (t->state == TASK_SLEEPING && t->wait_chan == chan && t->wait_key == key && chan != NULL) {
            wake(t, 0);
            n++;
        }
    }
    return n;
}

void sched_interrupt(struct task *t) {
    if (t->state == TASK_SLEEPING && t->interruptible && t->wait_chan != NULL) {
        wake(t, KERR_INTR);
    }
}

static const char *state_name(enum task_state st) {
    switch (st) {
    case TASK_UNUSED:
        return "unused";
    case TASK_RUNNABLE:
        return "runnable";
    case TASK_SLEEPING:
        return "sleeping";
    default:
        return "zombie";
    }
}

/* Every live task, what it is waiting for, and which core has it.
 *
 * `chan` is the address a sleeper is waiting on, which is what identifies a
 * lost wakeup: the code that was supposed to wake it is the code that owns
 * that address. */
bool sched_has_work(void) {
    return run_head != NULL || !is_idle(current);
}

void sched_dump(void) {
    kprintf("sched: run queue %s\n", run_head == NULL ? "empty" : "not empty");
    for (uint32_t i = 0; i < SCHED_MAX_TASKS; i++) {
        const struct task *t = &tasks[i];
        int on = -1;
        if (t->state == TASK_UNUSED) {
            continue;
        }
        for (uint32_t cpu = 0; cpu < SMP_MAX_CPUS; cpu++) {
            on = cpu_current[cpu] == t ? (int)cpu : on;
        }
        kprintf("  tid %-3u %-9s pid %-3u chan %p key %u deadline %s cpu %d%s\n", t->tid, state_name(t->state),
                t->proc != NULL ? t->proc->pid : 0, t->wait_chan, t->wait_key,
                t->wake_at_ns != 0 ? "yes" : "no", on, is_idle(t) ? " (idle)" : "");
    }
}

/* How long every core may be idle with a sleeper on the books before the
 * kernel decides it is wedged. 0 turns the check off. */
static uint32_t hang_ticks;
static uint32_t idle_ticks;

void sched_set_hangcheck(uint32_t seconds) {
    hang_ticks = seconds * 1000u; /* the tick is 1 ms */
}

/* A kernel where every core is idle and something is asleep that nothing
 * will wake is wedged. Silence is the worst way to report that — it looks
 * like a slow test — so it is reported with the task table and a panic,
 * which names the channel whose waker never came. */
static void check_for_a_wedged_kernel(void) {
    bool sleepers = false;
    if (hang_ticks == 0 || run_head != NULL || !is_idle(current)) {
        idle_ticks = 0;
        return;
    }
    for (uint32_t i = FIRST_REAL_TASK; i < SCHED_MAX_TASKS; i++) {
        sleepers = sleepers || tasks[i].state == TASK_SLEEPING;
    }
    if (!sleepers || ++idle_ticks < hang_ticks) {
        return;
    }
    sched_dump();
    proc_dump();
    panic("sched: every core idle for %u s with a sleeper nothing will wake", hang_ticks / 1000u);
}

void sched_tick(uint64_t now_ns) {
    for (uint32_t i = FIRST_REAL_TASK; i < SCHED_MAX_TASKS; i++) {
        struct task *t = &tasks[i];
        if (t->state == TASK_SLEEPING && t->wake_at_ns != 0 && now_ns >= t->wake_at_ns && t->wait_chan != NULL) {
            wake(t, KERR_TIMEDOUT);
        }
    }
    if (!is_idle(current) && current->slice_left > 0 && --current->slice_left == 0) {
        need_resched = true;
    }
    check_for_a_wedged_kernel();
}

void sched_preempt_point(void) {
    if (need_resched) {
        need_resched = false;
        sched_yield();
    }
}

void sched_exit_current(void) {
    current->state = TASK_ZOMBIE;
    sched_yield();
    panic("sched: a zombie was scheduled");
}

bool sched_kill_task(struct task *t) {
    if (t == current || is_idle(t)) {
        panic("sched: cannot kill the running or idle task");
    }
    if (t->state == TASK_RUNNABLE) {
        runq_remove(t);
    }
    /* "Not the task I am running" meant "not running at all" on one core.
     * On four it does not: another core may have this task as its current,
     * or be part way through switching away from it, and freeing it there
     * releases the kernel stack that core is standing on and zeroes the
     * struct it calls `current`. The fault then lands in an unrelated
     * syscall, as a process that is suddenly NULL.
     *
     * Such a task is marked dead instead and freed by reap_zombies once no
     * core holds it. It stops at its next entry into the kernel, which the
     * tick guarantees within a millisecond (sched_exit_if_killed). */
    /* The mark is a flag of its own and **not** the state field: a task
     * running on another core goes on to make its next syscall, and a
     * sched_sleep would overwrite a TASK_ZOMBIE state with TASK_SLEEPING —
     * after which the thread waited for ever as an ordinary sleeper and its
     * process, already exiting, could never end. */
    if (running_somewhere(t)) {
        t->killed = true;
        return false;
    }
    free_task(t);
    return true;
}

bool sched_killed(void) {
    return current->killed;
}

/* An idle core. It holds the kernel lock while it looks at the run queue
 * and gives it up around the wait, which is the one place anything does
 * (klock.h rule 3): there is nothing to protect while asleep, and holding
 * it there would stop every other core from entering the kernel.
 *
 * Every core ends up here — the primary at the end of boot, a secondary as
 * soon as it is let in — so this is also where a core waits for work. */
void sched_idle_loop(void) {
    klock_acquire();
    for (;;) {
        reap_zombies();
        if (run_head != NULL) {
            sched_yield();
            continue;
        }
        irq_wait();  /* gives the lock up while it waits (klock.h rule 5) */
    }
}

void sched_for_each(void (*fn)(void *ctx, struct task *t), void *ctx) {
    for (uint32_t i = FIRST_REAL_TASK; i < SCHED_MAX_TASKS; i++) {
        if (tasks[i].state != TASK_UNUSED && tasks[i].state != TASK_ZOMBIE) {
            fn(ctx, &tasks[i]);
        }
    }
}

struct task *sched_find(uint32_t tid) {
    for (uint32_t i = FIRST_REAL_TASK; i < SCHED_MAX_TASKS; i++) {
        if (tasks[i].tid == tid && tasks[i].state != TASK_UNUSED && tasks[i].state != TASK_ZOMBIE) {
            return &tasks[i];
        }
    }
    return NULL;
}
