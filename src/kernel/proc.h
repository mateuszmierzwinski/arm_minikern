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
    struct mm *mm;
    struct file *fds[PROC_MAX_FDS];
    uint64_t cloexec;                 /* bit n: fd n is close-on-exec */
    struct k_sigaction actions[NSIG]; /* index n-1 is signal n */
    uint32_t nthreads;
    bool exiting;
    int exit_status;                  /* Linux wait status: code << 8, or the signal */
    uint32_t start_stack;             /* the initial sp: /proc/self/stat field 28 */
    char name[16];
};

/* What runs when a process's last thread is gone (init: report, power off). */
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

#endif
