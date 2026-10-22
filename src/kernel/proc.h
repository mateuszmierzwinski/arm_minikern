/* proc — a process: a thread group sharing one address space, one file
 * table and one set of signal actions (Go's threads are clone()s with
 * CLONE_VM | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD, so that is the only
 * sharing model there is). The pid is the first thread's tid, as in Linux. */
#ifndef MINIKERN_KERNEL_PROC_H
#define MINIKERN_KERNEL_PROC_H

#include <stdbool.h>
#include <stdint.h>

#include "kernel/mm.h"
#include "kernel/sched.h"

#define NSIG 64u
#define SIG_IGN_VALUE 1u   /* SIG_IGN, as user space passes it */
#define PROC_MAX_FDS 64u

struct file;

/* struct sigaction as the ARM kernel ABI lays it out (20 bytes). */
struct k_sigaction {
    uint32_t handler;
    uint32_t flags;
    uint32_t restorer;
    uint64_t mask;
};

struct proc {
    uint32_t pid;
    uint32_t ppid;
    struct mm *mm;
    bool mm_borrowed;                 /* the address space belongs to the vfork parent */
    bool zombie;                      /* ended, and kept until its parent reaps it */
    bool vfork_pending;               /* a parent is suspended waiting for this child */
    struct file *fds[PROC_MAX_FDS];
    uint64_t cloexec;                 /* bit n: fd n is close-on-exec */
    struct k_sigaction actions[NSIG]; /* index n-1 is signal n */
    uint32_t nthreads;
    bool exiting;
    int exit_status;                  /* Linux wait status: code << 8, or the signal */
    uint32_t start_stack;             /* the initial sp: /proc/self/stat field 28 */
    char name[16];
};

/* What runs when the **first** process's last thread is gone: report how it
 * ended, and power the machine off if the command line asked. It fires only
 * for the process the board started (the one with no parent), never for a
 * child — a child's exit is an ordinary event. */
struct proc_exit_hook {
    void (*exited)(void *self, const struct proc *p);
    void *self;
};

void proc_set_exit_hook(const struct proc_exit_hook *hook);

/* A new process with an empty address space and no threads. */
struct proc *proc_create(const char *name);

/* The calling thread's process (NULL in the idle task). */
struct proc *proc_current(void);

/* Adds a thread (counts it; the task is created by the caller). */
void proc_attach_thread(struct proc *p, struct task *t);

/* Thread exit: clears clear_child_tid (with a futex wake), and ends the
 * process when it was the last thread. Never returns. */
__attribute__((noreturn)) void proc_exit_thread(int code);

/* exit_group: every thread of the current process ends. status is a Linux
 * wait status (exit code << 8, or a terminating signal number). */
__attribute__((noreturn)) void proc_exit_group(int status);

/* Makes to's address space current (from's is left); either may be NULL. */
void proc_switch_mm(const struct proc *from, const struct proc *to);

/* A new process that **shares** the caller's address space and file table
 * until it execs or exits: Linux's vfork, and what Go's `os/exec` asks for
 * (`CLONE_VM | CLONE_VFORK`). Sharing a stack between two processes is only
 * safe because the parent is suspended for exactly that window, so the two
 * halves of this — the share and the suspension — must not be separated. */
struct proc *proc_vfork(struct proc *parent, const char *name);

/* Lets a suspended vfork parent run again: the child has execed into its own
 * address space, or ended. Idempotent, because both of those can happen. */
void proc_vfork_done(struct proc *child);

/* Waits for a child to end. pid is a specific child or (uint32_t)-1 for any.
 * On success *status is the Linux wait status and the return is the child's
 * pid; KERR_CHILD when there is no such child, KERR_AGAIN when nohang is set
 * and none has ended yet. The reaped process is freed here, which is the
 * point: until it is, it is a zombie holding its exit status. */
int proc_wait(uint32_t pid, bool nohang, uint32_t *status);

/* Makes fresh the process's address space and the current one, dropping the
 * old address space only if the process owned it — a vfork child is running
 * in its parent's and must not free it. Infallible by construction: the
 * caller builds the new space first, so that a failed execve can still be
 * reported to a process that still has its old image. */
void proc_adopt_mm(struct proc *p, struct mm *fresh);

/* Closes every descriptor marked close-on-exec. */
void proc_close_on_exec(struct proc *p);

/* Every live process and what state it is in: the other half of a wedged
 * kernel's picture, since a task's channel says what it waits for but not
 * what its process is waiting to become. */
void proc_dump(void);

/* Leaves the calling thread as the only one in its process, which is what
 * execve needs: the new image must not share an address space with threads
 * still running the old one.
 *
 * A sibling on another core cannot be stopped synchronously — it is marked
 * and ends itself at its next entry into the kernel, which the tick makes a
 * millisecond — so this waits for the count to fall, and gives up with
 * KERR_AGAIN rather than waiting for ever if one does not go.
 *
 * It is **not** reversible: the siblings are gone whether the exec that
 * follows succeeds or not, so the caller runs it only once the new image is
 * known to be loadable (kernel/exec.c). */
int proc_become_single_threaded(struct proc *p);

/* Renames the process, which is what /proc/self/stat and the logs show. */
void proc_rename(struct proc *p, const char *name);

/* Forgets which address space this core has installed, without touching
 * the hardware — for a core that has just set its own translation
 * registers from nothing and whose record of them is therefore stale.
 *
 * A core that powers itself off and is started again (arch/arm/smp.h)
 * re-enters with `mmu_init_cpu` pointing TTBR0 at the empty table, while
 * this record still named the address space it had before. The next switch
 * to that same process then **skipped the TTBR0 write**, and the core ran
 * user code with no user mapping at all: a prefetch abort on the first
 * instruction, in SVC with interrupts masked, which looks like a wedged
 * machine rather than a fault. It is the same mistake as the vfork one
 * above, arrived at from the other direction — there the record was
 * guessed, here it outlived the register it described.
 */
void proc_forget_installed_mm(void);

#endif
