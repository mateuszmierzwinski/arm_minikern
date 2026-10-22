#include "kernel/proc.h"

#include "arch/arm/mmu.h"
#include "arch/arm/smp.h"
#include "core/console.h"
#include "core/errno.h"
#include "kernel/futex.h"
#include "kernel/mem.h"
#include "kernel/panic.h"
#include "kernel/time.h"
#include "kernel/vfs.h"

#define PROC_MAX 32

static struct proc_exit_hook exit_hook;

/* Every live process, so that a parent can find its children. A fixed table
 * rather than a list: the bound is the point, since a fork bomb is a
 * denial-of-service test this kernel is expected to pass. */
static struct proc *table[PROC_MAX];

static int table_add(struct proc *p) {
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        if (table[i] == NULL) {
            table[i] = p;
            return KERR_OK;
        }
    }
    return KERR_AGAIN;
}

static void table_remove(const struct proc *p) {
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        if (table[i] == p) {
            table[i] = NULL;
        }
    }
}

void proc_set_exit_hook(const struct proc_exit_hook *hook) {
    exit_hook = *hook;
}

struct proc *proc_create(const char *name) {
    struct proc *p = kzalloc(sizeof *p);
    if (p == NULL) {
        return NULL;
    }
    p->mm = mm_create();
    if (p->mm == NULL || table_add(p) != KERR_OK) {
        mm_put(p->mm);
        kfree(p);
        return NULL;
    }
    for (uint32_t i = 0; i + 1 < sizeof p->name && name[i] != '\0'; i++) {
        p->name[i] = name[i];
    }
    return p;
}

struct proc *proc_current(void) {
    return sched_current()->proc;
}

void proc_attach_thread(struct proc *p, struct task *t) {
    if (p->nthreads++ == 0) {
        p->pid = t->tid;
    }
}

/* What each core has in TTBR0 right now.
 *
 * The switch used to be decided by comparing the outgoing task's address
 * space with the incoming one, which is a guess about the hardware rather
 * than a fact about it — and a wrong one as soon as anything else writes
 * TTBR0. A process exiting does exactly that (`mmu_clear_user`), so a core
 * that had just run a vfork child was left holding the empty table while
 * still believing it held the shared one: the next thread of the parent to
 * run there found none of its own memory mapped and took a translation
 * fault on its own code. Recording what was installed, per core, removes
 * the guess. */
static const struct mm *installed[SMP_MAX_CPUS];

static const struct mm **installed_slot(void) {
    uint32_t cpu = smp_cpu_id();
    return &installed[cpu < SMP_MAX_CPUS ? cpu : 0];
}

void proc_switch_mm(const struct proc *from, const struct proc *to) {
    const struct mm **have = installed_slot();
    (void)from;
    if (to != NULL && *have != to->mm) {
        mmu_switch_user(to->mm->pt.l1_phys);
        *have = to->mm;
    }
}

/* Takes this core off any process's address space, after which the next
 * switch installs one for certain. */
static void drop_user_mm(void) {
    mmu_clear_user();
    *installed_slot() = NULL;
}

/* A process becomes a zombie rather than disappearing when a parent still
 * exists to ask how it ended. Everything expensive is already gone by then:
 * what is kept is the struct and the exit status. */
static bool parent_may_wait(const struct proc *p) {
    if (p->ppid == 0) {
        return false;
    }
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        if (table[i] != NULL && table[i]->pid == p->ppid && !table[i]->zombie) {
            return true;
        }
    }
    return false;
}

/* The last thread is gone: files, memory, then whoever is told. */
static void proc_finish(struct proc *p) {
    for (uint32_t fd = 0; fd < PROC_MAX_FDS; fd++) {
        if (p->fds[fd] != NULL) {
            file_put(p->fds[fd]);
            p->fds[fd] = NULL;
        }
    }
    drop_user_mm();
    if (!p->mm_borrowed) {
        mm_put(p->mm);
    }
    p->mm = NULL;
    proc_vfork_done(p);
    /* The hook belongs to the **first** process, the one the board started:
     * it reports how the machine's job ended and may power it off. A child's
     * exit is not that, and firing it for one turned the first `exec.Command`
     * into a shutdown. A process with no parent is init, and nothing else. */
    if (exit_hook.exited != NULL && p->ppid == 0) {
        exit_hook.exited(exit_hook.self, p);
    }
    if (parent_may_wait(p)) {
        p->zombie = true;
        sched_wake(&table, 0, UINT32_MAX); /* whoever is in wait4 */
        return;
    }
    table_remove(p);
    kfree(p);
}

struct proc *proc_vfork(struct proc *parent, const char *name) {
    struct proc *p = kzalloc(sizeof *p);
    if (p == NULL || table_add(p) != KERR_OK) {
        kfree(p);
        return NULL;
    }
    p->mm = parent->mm;
    p->mm_borrowed = true;
    p->vfork_pending = true;
    p->ppid = parent->pid;
    for (uint32_t fd = 0; fd < PROC_MAX_FDS; fd++) {
        p->fds[fd] = parent->fds[fd];
        if (p->fds[fd] != NULL) {
            file_get(p->fds[fd]);
        }
    }
    p->cloexec = parent->cloexec;
    for (uint32_t i = 0; i < NSIG; i++) {
        p->actions[i] = parent->actions[i];
    }
    for (uint32_t i = 0; i + 1 < sizeof p->name && name[i] != '\0'; i++) {
        p->name[i] = name[i];
    }
    return p;
}

/* The flag, and not the borrowed address space, is what says a parent is
 * still waiting: by the time an execve gets here it has already taken an
 * address space of its own. The flag also closes a race that only four cores
 * can show — the child can reach this point on another core before the
 * parent has reached its sleep, and a wake with nobody waiting is lost. The
 * parent therefore tests the flag rather than trusting the wake. */
void proc_vfork_done(struct proc *child) {
    child->vfork_pending = false;
    sched_wake(&child->ppid, 0, UINT32_MAX);
}

void proc_adopt_mm(struct proc *p, struct mm *fresh) {
    if (!p->mm_borrowed) {
        mm_put(p->mm);
    }
    p->mm = fresh;
    p->mm_borrowed = false;
    mmu_switch_user(fresh->pt.l1_phys);
    *installed_slot() = fresh;
}

void proc_rename(struct proc *p, const char *name) {
    uint32_t i = 0;
    for (; i + 1 < sizeof p->name && name[i] != '\0'; i++) {
        p->name[i] = name[i];
    }
    for (; i < sizeof p->name; i++) {
        p->name[i] = '\0';
    }
}

void proc_close_on_exec(struct proc *p) {
    for (uint32_t fd = 0; fd < PROC_MAX_FDS; fd++) {
        if (p->fds[fd] != NULL && (p->cloexec & (1ull << fd)) != 0) {
            file_put(p->fds[fd]);
            p->fds[fd] = NULL;
        }
    }
    p->cloexec = 0;
}

/* The caller's child that has ended, if any; counts the living ones so that
 * wait4 can tell "none yet" from "none at all". */
static struct proc *find_dead_child(uint32_t ppid, uint32_t pid, uint32_t *living) {
    struct proc *dead = NULL;
    *living = 0;
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        struct proc *c = table[i];
        if (c == NULL || c->ppid != ppid || (pid != (uint32_t)-1 && c->pid != pid)) {
            continue;
        }
        if (c->zombie) {
            dead = dead != NULL ? dead : c;
        } else {
            (*living)++;
        }
    }
    return dead;
}

int proc_wait(uint32_t pid, bool nohang, uint32_t *status) {
    struct proc *self = proc_current();
    for (;;) {
        uint32_t living;
        struct proc *dead = find_dead_child(self->pid, pid, &living);
        if (dead != NULL) {
            int reaped = (int)dead->pid;
            *status = (uint32_t)dead->exit_status;
            table_remove(dead);
            kfree(dead);
            return reaped;
        }
        if (living == 0) {
            return KERR_CHILD;
        }
        if (nohang) {
            return KERR_AGAIN;
        }
        int rc = sched_sleep(&table, 0, 0, true);
        if (rc != KERR_OK) {
            return rc;
        }
    }
}

/* CLONE_CHILD_CLEARTID: zero the word and wake one futex waiter on it,
 * which is how a thread library learns a thread is gone. */
static void clear_child_tid(struct task *t) {
    uint32_t zero = 0;
    if (t->clear_child_tid != 0 && mm_copy_out(t->proc->mm, t->clear_child_tid, &zero, 4) == 0) {
        futex_wake(t->proc->mm, t->clear_child_tid, 1);
    }
}

void proc_exit_thread(int code) {
    struct task *t = sched_current();
    struct proc *p = t->proc;
    clear_child_tid(t);
    p->nthreads--;
    /* Whoever is waiting for this process to be down to one thread
     * (proc_become_single_threaded) is woken by every departure. */
    sched_wake(&p->nthreads, 0, UINT32_MAX);
    if (p->nthreads == 0) {
        if (!p->exiting) {
            p->exit_status = (code & 0xff) << 8;
        }
        proc_finish(p);
    }
    t->proc = NULL;
    sched_exit_current();
}

/* sched_for_each callback: ends every other thread of the process. */
/* Ends one sibling of the exiting thread.
 *
 * Whoever really ends a thread decrements the count. A sibling that is
 * running on another core cannot be stopped synchronously: it is marked and
 * ends itself at its next entry into the kernel, and *it* decrements the
 * count then. Counting such a thread as gone immediately let the last
 * thread run proc_finish while those threads were still alive — the process
 * became a zombie, its parent reaped it, and a thread was left running with
 * a pointer to a freed process. Counting it nowhere is the opposite bug: the
 * count never reaches zero and the process never ends at all. */
static void kill_sibling(void *ctx, struct task *t) {
    struct proc *p = ctx;
    if (t->proc == p && t != sched_current() && sched_kill_task(t)) {
        p->nthreads--;
    }
}

#define SINGLE_THREAD_WAIT_MS 2000u

int proc_become_single_threaded(struct proc *p) {
    uint64_t deadline = time_monotonic_ns() + (uint64_t)SINGLE_THREAD_WAIT_MS * 1000000ull;
    sched_for_each(kill_sibling, p);
    while (p->nthreads > 1) {
        if (sched_sleep(&p->nthreads, 0, deadline, false) == KERR_TIMEDOUT) {
            return KERR_AGAIN;
        }
    }
    return KERR_OK;
}

void proc_exit_group(int status) {
    struct proc *p = proc_current();
    if (!p->exiting) {
        p->exiting = true;
        p->exit_status = status;
    }
    sched_for_each(kill_sibling, p);
    proc_exit_thread(0);
}

void proc_dump(void) {
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        const struct proc *p = table[i];
        if (p == NULL) {
            continue;
        }
        kprintf("  pid %-3u ppid %-3u threads %u%s%s%s%s mm %p name %s\n", p->pid, p->ppid, p->nthreads,
                p->exiting ? " exiting" : "", p->zombie ? " zombie" : "",
                p->vfork_pending ? " vfork-pending" : "", p->mm_borrowed ? " borrowed-mm" : "",
                (const void *)p->mm, p->name);
    }
}
