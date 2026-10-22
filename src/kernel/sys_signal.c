/* sys_signal — rt_sigaction, rt_sigprocmask, sigaltstack, rt_sigreturn,
 * kill, tkill and tgkill. */
#include "core/errno.h"
#include "kernel/proc.h"
#include "kernel/signal.h"
#include "kernel/syscall.h"

#include <stddef.h>

static int32_t sys_rt_sigaction(struct trapframe *tf) {
    return signal_action((int)tf->r[0], tf->r[1], tf->r[2], tf->r[3]);
}

static int32_t sys_rt_sigprocmask(struct trapframe *tf) {
    return signal_procmask((int)tf->r[0], tf->r[1], tf->r[2], tf->r[3]);
}

static int32_t sys_sigaltstack(struct trapframe *tf) {
    return signal_altstack(tf->r[0], tf->r[1], tf->sp_usr);
}

static int32_t sys_rt_sigreturn(struct trapframe *tf) {
    return signal_return(tf);
}

/* Sends sig to t when t belongs to the caller's process (one process for
 * now: other targets do not exist). sig 0 only checks existence. */
static int32_t send_to(struct task *t, int sig, int code) {
    struct proc *p = proc_current();
    if (t == NULL || t->proc == NULL) {
        return KERR_SRCH;
    }
    if (sig < 0 || sig > (int)NSIG) {
        return KERR_INVAL;
    }
    if (sig != 0) {
        signal_send(t, sig, code, p->pid);
    }
    return KERR_OK;
}

static int32_t sys_tgkill(struct trapframe *tf) {
    struct task *t = sched_find(tf->r[1]);
    if (t != NULL && t->proc != NULL && t->proc->pid != tf->r[0]) {
        return KERR_SRCH;
    }
    return send_to(t, (int)tf->r[2], SI_TKILL);
}

static int32_t sys_tkill(struct trapframe *tf) {
    return send_to(sched_find(tf->r[0]), (int)tf->r[1], SI_TKILL);
}

/* kill(pid, sig): to the process, i.e. its thread that has sig unblocked
 * (else its first thread). Only the caller's own process exists to us. */
struct kill_target {
    struct proc *p;
    int sig;
    struct task *chosen;
};

static void pick_thread(void *ctx, struct task *t) {
    struct kill_target *k = ctx;
    if (t->proc == k->p && (k->chosen == NULL || (k->chosen->sig_blocked & (1ull << (k->sig - 1))))) {
        k->chosen = t;
    }
}

static int32_t sys_kill(struct trapframe *tf) {
    struct proc *p = proc_current();
    int32_t pid = (int32_t)tf->r[0];
    struct kill_target k = {.p = p, .sig = (int)tf->r[1] == 0 ? 1 : (int)tf->r[1]};
    if (pid != 0 && pid != -1 && (uint32_t)pid != p->pid) {
        return KERR_SRCH;
    }
    sched_for_each(pick_thread, &k);
    return send_to(k.chosen, (int)tf->r[1], SI_USER);
}

const struct syscall_entry sys_signal_table[] = {
    {37, "kill", sys_kill},
    {173, "rt_sigreturn", sys_rt_sigreturn},
    {174, "rt_sigaction", sys_rt_sigaction},
    {175, "rt_sigprocmask", sys_rt_sigprocmask},
    {186, "sigaltstack", sys_sigaltstack},
    {238, "tkill", sys_tkill},
    {268, "tgkill", sys_tgkill},
    {0, NULL, NULL},
};
