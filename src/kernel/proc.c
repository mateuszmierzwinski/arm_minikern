#include "kernel/proc.h"

#include "arch/arm/mmu.h"
#include "core/console.h"
#include "kernel/futex.h"
#include "kernel/mem.h"
#include "kernel/panic.h"
#include "kernel/vfs.h"

static struct proc_exit_hook exit_hook;

void proc_set_exit_hook(const struct proc_exit_hook *hook) {
    exit_hook = *hook;
}

struct proc *proc_create(const char *name) {
    struct proc *p = kzalloc(sizeof *p);
    if (p == NULL) {
        return NULL;
    }
    p->mm = mm_create();
    if (p->mm == NULL) {
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

void proc_switch_mm(const struct proc *from, const struct proc *to) {
    if (to != NULL && (from == NULL || from->mm != to->mm)) {
        mmu_switch_user(to->mm->pt.l1_phys);
    }
}

/* The last thread is gone: files, memory, then whoever is told. */
static void proc_finish(struct proc *p) {
    for (uint32_t fd = 0; fd < PROC_MAX_FDS; fd++) {
        if (p->fds[fd] != NULL) {
            file_put(p->fds[fd]);
            p->fds[fd] = NULL;
        }
    }
    mmu_clear_user();
    mm_put(p->mm);
    p->mm = NULL;
    if (exit_hook.exited != NULL) {
        exit_hook.exited(exit_hook.self, p);
    }
    kfree(p);
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
    if (--p->nthreads == 0) {
        if (!p->exiting) {
            p->exit_status = (code & 0xff) << 8;
        }
        proc_finish(p);
    }
    t->proc = NULL;
    sched_exit_current();
}

/* sched_for_each callback: ends every other thread of the process. */
static void kill_sibling(void *ctx, struct task *t) {
    struct proc *p = ctx;
    if (t->proc == p && t != sched_current()) {
        p->nthreads--;
        sched_kill_task(t);
    }
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
