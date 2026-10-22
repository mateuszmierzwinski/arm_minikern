#include "kernel/signal.h"

#include "arch/arm/cpu.h"
#include "core/console.h"
#include "core/errno.h"
#include "kernel/proc.h"
#include "arch/arm/trap.h"

/* Flag and layout values: Linux's ARM ABI (asm/signal.h, asm/ucontext.h). */
#define SA_SIGINFO    0x00000004u
#define SA_RESTORER   0x04000000u
#define SA_ONSTACK    0x08000000u
#define SA_RESTART    0x10000000u
#define SA_NODEFER    0x40000000u
#define SA_RESETHAND  0x80000000u
#define SIG_DFL       0u
#define SIG_IGN       1u
#define SS_ONSTACK    1u
#define SS_DISABLE    2u
#define MINSIGSTKSZ   2048u
#define SIG_BLOCK     0
#define SIG_UNBLOCK   1
#define SIG_SETMASK   2
#define VFP_MAGIC     0x56465001u
#define CPSR_USER_MASK 0xf80f0200u  /* NZCVQ, GE, E: what user code may set */
#define CPSR_T        0x20u
#define MODE_USR      0x10u
#define TRAP_NO_FAULT 14u

static uint64_t sig_bit(int sig) {
    return 1ull << (sig - 1);
}

static const uint64_t unblockable = (1ull << (SIGKILL - 1)) | (1ull << (SIGSTOP - 1));

/* siginfo_t, 128 bytes; the union's fault and kill views share offset 12. */
struct k_siginfo {
    int32_t signo;
    int32_t err;
    int32_t code;
    uint32_t a;  /* si_addr, or si_pid */
    uint32_t b;  /* si_uid */
    uint32_t pad[27];
};

struct k_sigcontext {
    uint32_t trap_no, error_code, oldmask;
    uint32_t r[11];
    uint32_t fp, ip, sp, lr, pc, cpsr, fault_address;
};

/* The VFP block in uc_regspace: Linux's struct vfp_sigframe shape. */
struct k_vfp_frame {
    uint32_t magic;
    uint32_t size;
    struct vfp_state state;
    uint32_t fpexc, fpinst, fpinst2, pad;
};

struct k_ucontext {
    uint32_t flags;
    uint32_t link;
    uint32_t ss_sp, ss_flags, ss_size;
    struct k_sigcontext mcontext;
    uint64_t sigmask;
    uint32_t unused[30];
    uint32_t regspace[128] __attribute__((aligned(8)));
};

struct k_rt_sigframe {
    struct k_siginfo info;
    struct k_ucontext uc;
};

_Static_assert(sizeof(struct k_siginfo) == 128, "siginfo is 128 bytes");
_Static_assert(__builtin_offsetof(struct k_ucontext, mcontext) == 20, "uc_mcontext at 20");
_Static_assert(__builtin_offsetof(struct k_ucontext, sigmask) == 104, "uc_sigmask at 104");
_Static_assert(__builtin_offsetof(struct k_ucontext, regspace) == 232, "uc_regspace at 232");
_Static_assert(sizeof(struct k_vfp_frame) <= sizeof(((struct k_ucontext *)0)->regspace), "VFP fits");

/* The per-thread record of a fault being forced, used by the next delivery. */
struct fault_info {
    int sig;
    int code;
    uint32_t addr;
    uint32_t fsr;
};

static struct fault_info pending_fault[SCHED_MAX_TASKS];

static struct fault_info *fault_slot(const struct task *t) {
    return &pending_fault[t->tid % SCHED_MAX_TASKS];
}

/* Default action of sig: true means "terminate the process". */
static bool default_kills(int sig) {
    return sig != SIGCHLD && sig != SIGURG && sig != SIGWINCH && sig != SIGCONT && sig != SIGSTOP &&
           sig != SIGTSTP;
}

void signal_send(struct task *t, int sig, int code, uint32_t sender_pid) {
    struct fault_info *f = fault_slot(t);
    if (sig < 1 || sig > (int)NSIG) {
        return;
    }
    if (f->sig == 0) {
        *f = (struct fault_info){.sig = 0, .code = code, .addr = sender_pid};
    }
    t->sig_pending |= sig_bit(sig);
    if ((t->sig_blocked & sig_bit(sig)) == 0) {
        sched_interrupt(t);
    }
}

void signal_force_fault(struct trapframe *tf, int sig, int code, uint32_t addr, uint32_t fsr) {
    struct task *t = sched_current();
    struct k_sigaction *a = &t->proc->actions[sig - 1];
    if ((t->sig_blocked & sig_bit(sig)) || a->handler == SIG_IGN) {
        t->sig_blocked &= ~sig_bit(sig);
        a->handler = SIG_DFL;
    }
    *fault_slot(t) = (struct fault_info){.sig = sig, .code = code, .addr = addr, .fsr = fsr};
    t->sig_pending |= sig_bit(sig);
    signal_deliver(tf);
}

/* The lowest-numbered deliverable signal, or 0. */
static int next_signal(const struct task *t) {
    uint64_t ready = t->sig_pending & ~(t->sig_blocked & ~unblockable);
    for (int sig = 1; sig <= (int)NSIG; sig++) {
        if (ready & sig_bit(sig)) {
            return sig;
        }
    }
    return 0;
}

/* Where the frame goes: the alternate stack when asked for and not on it. */
static uint32_t frame_base(const struct task *t, const struct k_sigaction *a, uint32_t sp) {
    bool on_alt = t->altstack_size != 0 && sp - t->altstack_sp <= t->altstack_size;
    if ((a->flags & SA_ONSTACK) && t->altstack_size != 0 && !(t->altstack_flags & SS_DISABLE) && !on_alt) {
        sp = t->altstack_sp + t->altstack_size;
    }
    return (sp - sizeof(struct k_rt_sigframe)) & ~7u;
}

static void save_context(struct k_ucontext *uc, const struct trapframe *tf, const struct task *t,
                         const struct fault_info *f) {
    for (int i = 0; i < 11; i++) {
        uc->mcontext.r[i] = tf->r[i];
    }
    uc->mcontext.fp = tf->r[11];
    uc->mcontext.ip = tf->r[12];
    uc->mcontext.sp = tf->sp_usr;
    uc->mcontext.lr = tf->lr_usr;
    uc->mcontext.pc = tf->pc;
    uc->mcontext.cpsr = tf->spsr;
    uc->mcontext.trap_no = f->fsr != 0 ? TRAP_NO_FAULT : 0;
    uc->mcontext.error_code = f->fsr;
    uc->mcontext.oldmask = (uint32_t)t->sig_blocked;
    uc->mcontext.fault_address = f->addr;
    uc->sigmask = t->sig_blocked;
    uc->ss_sp = t->altstack_sp;
    uc->ss_size = t->altstack_size;
    uc->ss_flags = t->altstack_size == 0 ? SS_DISABLE : 0;
}

/* The VFP registers live in the hardware while in the kernel (it never
 * touches them), so they are read straight into the frame. */
static void save_vfp(struct k_ucontext *uc) {
    struct k_vfp_frame *v = (struct k_vfp_frame *)uc->regspace;
    v->magic = VFP_MAGIC;
    v->size = sizeof *v;
    arm_vfp_save(&v->state);
}

/* Where the handler returns to: sa_restorer when given (musl), else the
 * kernel's trampoline page in the handler's instruction set (Go on ARM). */
static uint32_t return_address(const struct k_sigaction *a) {
    if ((a->flags & SA_RESTORER) && a->restorer != 0) {
        return a->restorer;
    }
    return (a->handler & 1u) ? (MM_SIGPAGE + MM_SIGPAGE_THUMB_RT) | 1u : MM_SIGPAGE + MM_SIGPAGE_ARM_RT;
}

/* Builds the frame and points the thread at the handler. KERR_FAULT when
 * the stack cannot take it. */
static int setup_frame(struct trapframe *tf, struct task *t, int sig, const struct k_sigaction *a,
                       const struct fault_info *f) {
    static struct k_rt_sigframe frame;
    uint32_t base = frame_base(t, a, tf->sp_usr);
    frame = (struct k_rt_sigframe){.info = {.signo = sig, .code = f->code, .a = f->addr}};
    save_context(&frame.uc, tf, t, f);
    save_vfp(&frame.uc);
    if (mm_copy_out(t->proc->mm, base, &frame, sizeof frame) != KERR_OK) {
        return KERR_FAULT;
    }
    tf->r[0] = (uint32_t)sig;
    tf->r[1] = base;
    tf->r[2] = base + (uint32_t)__builtin_offsetof(struct k_rt_sigframe, uc);
    tf->sp_usr = base;
    tf->lr_usr = return_address(a);
    tf->pc = a->handler & ~1u;
    tf->spsr = (tf->spsr & ~CPSR_T) | ((a->handler & 1u) ? CPSR_T : 0u);
    return KERR_OK;
}

/* Kills the process with sig as its wait status, reporting why. */
static void die(struct task *t, int sig, const struct fault_info *f, const struct trapframe *tf) {
    kprintf("%s[%u]: killed by signal %d (pc %08x, addr %08x)\n", t->proc->name, t->tid, sig, tf->pc, f->addr);
    trap_dump(tf);
    proc_exit_group(sig);
}

/* Acts on one signal; returns false when the thread should look again. */
static void act_on(struct trapframe *tf, struct task *t, int sig) {
    struct k_sigaction *a = &t->proc->actions[sig - 1];
    struct fault_info f = *fault_slot(t);
    *fault_slot(t) = (struct fault_info){0};
    t->sig_pending &= ~sig_bit(sig);
    if (a->handler == SIG_IGN || (a->handler == SIG_DFL && !default_kills(sig))) {
        return;
    }
    if (a->handler == SIG_DFL || sig == SIGKILL) {
        die(t, sig, &f, tf);
    }
    if (setup_frame(tf, t, sig, a, &f) != KERR_OK) {
        die(t, SIGSEGV, &f, tf);
    }
    t->sig_blocked |= (a->mask | ((a->flags & SA_NODEFER) ? 0u : sig_bit(sig))) & ~unblockable;
    if (a->flags & SA_RESETHAND) {
        a->handler = SIG_DFL;
    }
}

void signal_deliver(struct trapframe *tf) {
    struct task *t = sched_current();
    int sig = t->proc != NULL ? next_signal(t) : 0;
    if (sig != 0) {
        act_on(tf, t, sig);
    }
}

int32_t signal_return(struct trapframe *tf) {
    struct task *t = sched_current();
    static struct k_ucontext uc;
    uint32_t uc_addr = tf->sp_usr + (uint32_t)__builtin_offsetof(struct k_rt_sigframe, uc);
    struct k_vfp_frame *v = (struct k_vfp_frame *)uc.regspace;
    if (mm_copy_in(t->proc->mm, &uc, uc_addr, sizeof uc) != KERR_OK) {
        kprintf("%s[%u]: bad rt_sigreturn frame at %08x\n", t->proc->name, t->tid, tf->sp_usr);
        proc_exit_group(SIGSEGV);
    }
    for (int i = 0; i < 11; i++) {
        tf->r[i] = uc.mcontext.r[i];
    }
    tf->r[11] = uc.mcontext.fp;
    tf->r[12] = uc.mcontext.ip;
    tf->sp_usr = uc.mcontext.sp;
    tf->lr_usr = uc.mcontext.lr;
    tf->pc = uc.mcontext.pc;
    tf->spsr = (uc.mcontext.cpsr & (CPSR_USER_MASK | CPSR_T)) | MODE_USR;
    t->sig_blocked = uc.sigmask & ~unblockable;
    if (v->magic == VFP_MAGIC) {
        arm_vfp_restore(&v->state);
    }
    return (int32_t)tf->r[0];
}

int signal_action(int sig, uint32_t act_uaddr, uint32_t oldact_uaddr, uint32_t sigsetsize) {
    struct proc *p = proc_current();
    struct k_sigaction act;
    if (sig < 1 || sig > (int)NSIG || sigsetsize != 8) {
        return KERR_INVAL;
    }
    if (oldact_uaddr != 0 && mm_copy_out(p->mm, oldact_uaddr, &p->actions[sig - 1], 20) != KERR_OK) {
        return KERR_FAULT;
    }
    if (act_uaddr == 0) {
        return KERR_OK;
    }
    if (sig == SIGKILL || sig == SIGSTOP) {
        return KERR_INVAL;
    }
    if (mm_copy_in(p->mm, &act, act_uaddr, 20) != KERR_OK) {
        return KERR_FAULT;
    }
    p->actions[sig - 1] = act;
    return KERR_OK;
}

int signal_procmask(int how, uint32_t set_uaddr, uint32_t oldset_uaddr, uint32_t sigsetsize) {
    struct task *t = sched_current();
    uint64_t set;
    if (sigsetsize != 8) {
        return KERR_INVAL;
    }
    if (oldset_uaddr != 0 && mm_copy_out(t->proc->mm, oldset_uaddr, &t->sig_blocked, 8) != KERR_OK) {
        return KERR_FAULT;
    }
    if (set_uaddr == 0) {
        return KERR_OK;
    }
    if (mm_copy_in(t->proc->mm, &set, set_uaddr, 8) != KERR_OK) {
        return KERR_FAULT;
    }
    if (how == SIG_BLOCK) {
        t->sig_blocked |= set;
    } else if (how == SIG_UNBLOCK) {
        t->sig_blocked &= ~set;
    } else if (how == SIG_SETMASK) {
        t->sig_blocked = set;
    } else {
        return KERR_INVAL;
    }
    t->sig_blocked &= ~unblockable;
    return KERR_OK;
}

/* stack_t on ARM: ss_sp, ss_flags, ss_size. */
struct k_stack {
    uint32_t sp;
    uint32_t flags;
    uint32_t size;
};

/* Installs (or, with SS_DISABLE, removes) the thread's alternate stack. */
static int altstack_set(struct task *t, const struct k_stack *ss) {
    if (ss->flags & SS_DISABLE) {
        t->altstack_sp = t->altstack_size = 0;
        return KERR_OK;
    }
    if ((ss->flags & ~SS_ONSTACK) != 0) {
        return KERR_INVAL;
    }
    if (ss->size < MINSIGSTKSZ) {
        return KERR_NOMEM;
    }
    t->altstack_sp = ss->sp;
    t->altstack_size = ss->size;
    t->altstack_flags = 0;
    return KERR_OK;
}

int signal_altstack(uint32_t ss_uaddr, uint32_t old_uaddr, uint32_t user_sp) {
    struct task *t = sched_current();
    bool on_alt = t->altstack_size != 0 && user_sp - t->altstack_sp <= t->altstack_size;
    struct k_stack old = {.sp = t->altstack_sp, .size = t->altstack_size,
                          .flags = t->altstack_size == 0 ? SS_DISABLE : (on_alt ? SS_ONSTACK : 0u)};
    struct k_stack ss;
    if (old_uaddr != 0 && mm_copy_out(t->proc->mm, old_uaddr, &old, sizeof old) != KERR_OK) {
        return KERR_FAULT;
    }
    if (ss_uaddr == 0) {
        return KERR_OK;
    }
    if (mm_copy_in(t->proc->mm, &ss, ss_uaddr, sizeof ss) != KERR_OK) {
        return KERR_FAULT;
    }
    return on_alt ? KERR_PERM : altstack_set(t, &ss);
}
