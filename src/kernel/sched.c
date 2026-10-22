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
static bool running_somewhere(const struct task *t) {
    for (uint32_t cpu = 0; cpu < SMP_MAX_CPUS; cpu++) {
        if (cpu_current[cpu] == t) {
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

static struct task *runq_pop(void) {
    struct task *t = run_head;
    if (t != NULL) {
        run_head = t->run_next;
        run_tail = run_head == NULL ? NULL : run_tail;
    }
    return t;
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
    arm_switch(&prev->saved_sp, next->saved_sp);
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

void sched_yield(void) {
    struct task *next;
    if (!is_idle(current) && current->state == TASK_RUNNABLE) {
        runq_push(current);
    }
    next = runq_pop();
    switch_to(next != NULL ? next : my_idle());
}

int sched_sleep(const void *chan, uint32_t key, uint64_t deadline_ns, bool interruptible) {
    struct task *t = current;
    if (is_idle(t)) {
        panic("sched: the idle task cannot sleep");
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

void sched_kill_task(struct task *t) {
    if (t == current || is_idle(t)) {
        panic("sched: cannot kill the running or idle task");
    }
    if (t->state == TASK_RUNNABLE) {
        runq_remove(t);
    }
    free_task(t);
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
