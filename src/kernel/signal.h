/* signal — POSIX signals as Linux delivers them on ARM.
 *
 * A pending, unblocked signal is acted on just before its thread returns
 * to user mode: ignored, a default action (terminate, or ignore for the
 * SIGCHLD/SIGURG/SIGWINCH kind), or a handler call. A handler call pushes
 * Linux's ARM rt_sigframe (siginfo, then a ucontext with the interrupted
 * registers, the old mask, and the VFP state in uc_regspace) on the user
 * stack, or on the sigaltstack for SA_ONSTACK, and enters the handler with
 * lr = sa_restorer, which calls rt_sigreturn. The layout is an ABI fact:
 * Go's runtime reads and rewrites uc_mcontext to preempt goroutines. */
#ifndef MINIKERN_KERNEL_SIGNAL_H
#define MINIKERN_KERNEL_SIGNAL_H

#include <stdint.h>

#include "arch/arm/trap.h"
#include "kernel/sched.h"

#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGILL   4
#define SIGTRAP  5
#define SIGABRT  6
#define SIGBUS   7
#define SIGFPE   8
#define SIGKILL  9
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGUSR2  12
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19
#define SIGTSTP  20
#define SIGURG   23
#define SIGPROF  27
#define SIGWINCH 28

#define SEGV_MAPERR 1
#define SEGV_ACCERR 2
#define ILL_ILLOPC  1
#define SI_TKILL    (-6)
#define SI_USER     0

/* Queues sig for thread t (from kill/tgkill: code SI_TKILL or SI_USER). */
void signal_send(struct task *t, int sig, int code, uint32_t sender_pid);

/* A fault the thread caused: delivered now, unblocked and with a default
 * action if it was blocked or ignored (Linux's force_sig). addr is the
 * faulting address. */
void signal_force_fault(struct trapframe *tf, int sig, int code, uint32_t addr, uint32_t fsr);

/* Acts on pending signals on the way back to user mode (trap layer hook). */
void signal_deliver(struct trapframe *tf);

/* rt_sigreturn: restores the context saved by signal_deliver. Returns the
 * restored r0, or kills the process when the frame is garbage. */
int32_t signal_return(struct trapframe *tf);

/* rt_sigaction / rt_sigprocmask / sigaltstack, with user pointers. */
int signal_action(int sig, uint32_t act_uaddr, uint32_t oldact_uaddr, uint32_t sigsetsize);
int signal_procmask(int how, uint32_t set_uaddr, uint32_t oldset_uaddr, uint32_t sigsetsize);
int signal_altstack(uint32_t ss_uaddr, uint32_t old_uaddr, uint32_t user_sp);

#endif
