/* sched — threads and the scheduler.
 *
 * A task is one thread: a kernel stack, the saved switch state, its VFP
 * registers and TLS value, and the process it belongs to. The scheduler is
 * round-robin and preemptive on the tick (a 10 ms slice), single core.
 * The kernel itself never preempts: it runs with IRQs masked, and a task
 * leaves the CPU only by sleeping, exiting, or on its way back to user mode.
 *
 * Sleeping is on a channel (an object address plus a key), which is what
 * futexes (mm, uaddr), timed sleeps and device waits all need. A sleep can
 * end by a wake, a deadline, or a signal (sched_interrupt). */
#ifndef MINIKERN_KERNEL_SCHED_H
#define MINIKERN_KERNEL_SCHED_H

#include <stdbool.h>
#include <stdint.h>

#include "arch/arm/cpu.h"
#include "arch/arm/trap.h"

#define SCHED_MAX_TASKS 64u
#define KSTACK_PAGES 2u  /* 8 KiB kernel stack per thread */

struct proc;

enum task_state {
    TASK_UNUSED = 0,
    TASK_RUNNABLE,
    TASK_SLEEPING,
    TASK_ZOMBIE,  /* exited; its stack is freed by the next schedule() */
};

struct task {
    uint32_t tid;
    enum task_state state;
    uint32_t saved_sp;       /* arm_switch's save slot */
    uint8_t *kstack;         /* KSTACK_PAGES pages; the trapframe sits at the top */
    struct proc *proc;       /* NULL for the idle task */
    struct vfp_state vfp;
    uint32_t tls;            /* TPIDRURO, set by set_tls */
    const void *wait_chan;
    uint32_t wait_key;
    uint64_t wake_at_ns;     /* 0: no deadline */
    int wait_result;         /* 0 woken, KERR_TIMEDOUT, KERR_INTR */
    bool interruptible;
    uint32_t clear_child_tid;
    uint64_t sig_blocked;    /* bit n-1 is signal n */
    uint64_t sig_pending;
    uint32_t altstack_sp;
    uint32_t altstack_size;
    uint32_t altstack_flags;
    uint32_t slice_left;     /* ticks before a preemption */
    struct task *run_next;   /* run queue link */
};

/* The boot context becomes the idle task (tid 0). */
void sched_init(void);

/* A fresh task with a kernel stack whose trapframe is zeroed; it is not
 * runnable until sched_start. NULL when out of tasks or memory. */
struct task *sched_new_task(struct proc *p);

/* The user context the task will resume (top of its kernel stack). */
struct trapframe *sched_trapframe(struct task *t);

/* Makes a new task runnable; its first run returns to user mode through
 * its trapframe. */
void sched_start(struct task *t);

struct task *sched_current(void);

/* Gives the CPU to the next runnable task (or idle). Returns when the
 * current task is scheduled again. */
void sched_yield(void);

/* Sleeps on (chan, key) until woken, until deadline_ns (monotonic; 0 = no
 * deadline) or, if interruptible, until a signal. Returns 0, KERR_TIMEDOUT
 * or KERR_INTR. */
int sched_sleep(const void *chan, uint32_t key, uint64_t deadline_ns, bool interruptible);

/* Wakes up to max tasks sleeping on (chan, key); returns how many. */
uint32_t sched_wake(const void *chan, uint32_t key, uint32_t max);

/* Ends t's interruptible sleep with KERR_INTR (a signal arrived). */
void sched_interrupt(struct task *t);

/* The tick: wakes expired sleepers and charges the running slice. */
void sched_tick(uint64_t now_ns);

/* Called on the way back to user mode: switches away if the slice ran out. */
void sched_preempt_point(void);

/* Ends the current task (never returns). The caller has already dealt with
 * its process. */
__attribute__((noreturn)) void sched_exit_current(void);

/* Removes another (not running) task: it is unlinked and freed. */
void sched_kill_task(struct task *t);

/* The idle loop: runs whatever is runnable, else waits for an interrupt. */
__attribute__((noreturn)) void sched_idle_loop(void);

/* Calls fn(ctx, t) for every live task. */
void sched_for_each(void (*fn)(void *ctx, struct task *t), void *ctx);

/* The task with this tid, or NULL. */
struct task *sched_find(uint32_t tid);

#endif
