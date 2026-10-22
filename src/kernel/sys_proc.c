/* sys_proc — process and thread syscalls: exit, exit_group, clone (threads),
 * the ids, set_tid_address, scheduling, limits, uname, prctl, getrandom,
 * and ARM's private set_tls and cacheflush. */
#include "arch/arm/mmu.h"
#include "core/errno.h"
#include "kernel/mem.h"
#include "kernel/proc.h"
#include "kernel/random.h"
#include "kernel/syscall.h"

#include <stddef.h>

#define CLONE_VM             0x00000100u
#define CLONE_SIGHAND        0x00000800u
#define CLONE_THREAD         0x00010000u
#define CLONE_SETTLS         0x00080000u
#define CLONE_PARENT_SETTID  0x00100000u
#define CLONE_CHILD_CLEARTID 0x00200000u
#define CLONE_CHILD_SETTID   0x01000000u
#define CLONE_THREAD_MODEL   (CLONE_VM | CLONE_SIGHAND | CLONE_THREAD)
#define RLIMIT_STACK         3u
#define RLIMIT_NOFILE        7u
#define PR_SET_NAME          15u
#define PR_GET_NAME          16u
#define UTS_FIELD            65u
#define GETRANDOM_CHUNK      256u

static int32_t sys_exit(struct trapframe *tf) {
    proc_exit_thread((int)tf->r[0]);
}

static int32_t sys_exit_group(struct trapframe *tf) {
    proc_exit_group((int)((tf->r[0] & 0xffu) << 8));
}

static int32_t sys_getpid(struct trapframe *tf) {
    (void)tf;
    return (int32_t)proc_current()->pid;
}

static int32_t sys_gettid(struct trapframe *tf) {
    (void)tf;
    return (int32_t)sched_current()->tid;
}

static int32_t sys_zero(struct trapframe *tf) {
    (void)tf;
    return 0;
}

static int32_t sys_set_tid_address(struct trapframe *tf) {
    struct task *t = sched_current();
    t->clear_child_tid = tf->r[0];
    return (int32_t)t->tid;
}

static int32_t sys_sched_yield(struct trapframe *tf) {
    (void)tf;
    sched_yield();
    return 0;
}

/* One CPU: the mask is a single word with bit 0 set. Returns its size. */
static int32_t sys_sched_getaffinity(struct trapframe *tf) {
    uint32_t mask = 1;
    if (tf->r[1] < sizeof mask) {
        return KERR_INVAL;
    }
    return mm_copy_out(proc_current()->mm, tf->r[2], &mask, sizeof mask) == KERR_OK ? (int32_t)sizeof mask
                                                                                    : KERR_FAULT;
}

/* The thread inherits the caller's registers, mask and VFP state, and
 * returns 0 from the same svc, on its own stack. */
static struct task *clone_thread(struct trapframe *tf, uint32_t flags) {
    struct task *parent = sched_current();
    struct task *child = sched_new_task(parent->proc);
    struct trapframe *ctf;
    if (child == NULL) {
        return NULL;
    }
    ctf = sched_trapframe(child);
    *ctf = *tf;
    ctf->r[0] = 0;
    ctf->sp_usr = tf->r[1] != 0 ? tf->r[1] : tf->sp_usr;
    child->tls = (flags & CLONE_SETTLS) ? tf->r[3] : parent->tls;
    child->sig_blocked = parent->sig_blocked;
    child->clear_child_tid = (flags & CLONE_CHILD_CLEARTID) ? tf->r[4] : 0;
    arm_vfp_save(&child->vfp);
    proc_attach_thread(parent->proc, child);
    return child;
}

/* clone(flags, newsp, parent_tid, tls, child_tid): threads only. */
static int32_t sys_clone(struct trapframe *tf) {
    uint32_t flags = tf->r[0];
    struct mm *mm = proc_current()->mm;
    struct task *child;
    if ((flags & CLONE_THREAD_MODEL) != CLONE_THREAD_MODEL) {
        return KERR_NOSYS;  /* fork and friends: not yet */
    }
    child = clone_thread(tf, flags);
    if (child == NULL) {
        return KERR_AGAIN;
    }
    if (flags & CLONE_PARENT_SETTID) {
        (void)mm_copy_out(mm, tf->r[2], &child->tid, 4);
    }
    if (flags & CLONE_CHILD_SETTID) {
        (void)mm_copy_out(mm, tf->r[4], &child->tid, 4);
    }
    sched_start(child);
    return (int32_t)child->tid;
}

/* prlimit64(pid, resource, new, old): limits are fixed; new ones are
 * accepted and ignored (Go raises RLIMIT_NOFILE to its max at start). */
static int32_t sys_prlimit64(struct trapframe *tf) {
    uint64_t lim[2] = {~0ull, ~0ull};
    if (tf->r[1] == RLIMIT_NOFILE) {
        lim[0] = lim[1] = PROC_MAX_FDS;
    } else if (tf->r[1] == RLIMIT_STACK) {
        lim[0] = MM_STACK_SIZE;
    }
    if (tf->r[3] != 0 && mm_copy_out(proc_current()->mm, tf->r[3], lim, sizeof lim) != KERR_OK) {
        return KERR_FAULT;
    }
    return KERR_OK;
}

/* ugetrlimit(resource, rlim): the 32-bit form. */
static int32_t sys_ugetrlimit(struct trapframe *tf) {
    uint32_t lim[2] = {~0u, ~0u};
    if (tf->r[0] == RLIMIT_NOFILE) {
        lim[0] = lim[1] = PROC_MAX_FDS;
    } else if (tf->r[0] == RLIMIT_STACK) {
        lim[0] = MM_STACK_SIZE;
    }
    return mm_copy_out(proc_current()->mm, tf->r[1], lim, sizeof lim);
}

static void put_field(char *uts, uint32_t field, const char *s) {
    for (uint32_t i = 0; s[i] != '\0' && i + 1 < UTS_FIELD; i++) {
        uts[field * UTS_FIELD + i] = s[i];
    }
}

/* A release Go's version checks accept as modern Linux. */
static int32_t sys_uname(struct trapframe *tf) {
    static char uts[6 * UTS_FIELD];
    static const char *const fields[] = {"Linux", "minikern", "6.1.0-minikern", "#1 minikern", "armv7l", "(none)"};
    for (uint32_t i = 0; i < sizeof uts; i++) {
        uts[i] = 0;
    }
    for (uint32_t f = 0; f < 6; f++) {
        put_field(uts, f, fields[f]);
    }
    return mm_copy_out(proc_current()->mm, tf->r[0], uts, sizeof uts);
}

static int32_t sys_prctl(struct trapframe *tf) {
    struct proc *p = proc_current();
    if (tf->r[0] == PR_SET_NAME) {
        return mm_copy_string_in(p->mm, p->name, tf->r[1], sizeof p->name) >= 0 ? KERR_OK : KERR_FAULT;
    }
    if (tf->r[0] == PR_GET_NAME) {
        return mm_copy_out(p->mm, tf->r[1], p->name, sizeof p->name);
    }
    return KERR_INVAL;
}

static int32_t sys_getrandom(struct trapframe *tf) {
    uint8_t buf[GETRANDOM_CHUNK];
    uint32_t done = 0, len = tf->r[1];
    while (done < len) {
        uint32_t n = len - done < sizeof buf ? len - done : sizeof buf;
        random_fill(buf, n);
        if (mm_copy_out(proc_current()->mm, tf->r[0] + done, buf, n) != KERR_OK) {
            return done > 0 ? (int32_t)done : KERR_FAULT;
        }
        done += n;
    }
    return (int32_t)done;
}

/* ARM private set_tls (0xf0005): the user read-only thread register. */
static int32_t sys_set_tls(struct trapframe *tf) {
    sched_current()->tls = tf->r[0];
    __asm__ volatile("mcr p15, 0, %0, c13, c0, 3" : : "r"(tf->r[0]));
    return 0;
}

/* ARM private cacheflush (0xf0002): makes JIT-written code executable. */
static int32_t sys_cacheflush(struct trapframe *tf) {
    struct mm *mm = proc_current()->mm;
    for (uint32_t va = tf->r[0] & ~4095u; va < tf->r[1]; va += 4096u) {
        void *k = mm_user_page(mm, va, PT_READ);
        if (k != NULL) {
            mmu_sync_icache(k, 4096u);
        }
    }
    return 0;
}

const struct syscall_entry sys_proc_table[] = {
    {1, "exit", sys_exit},
    {20, "getpid", sys_getpid},
    {64, "getppid", sys_zero},
    {120, "clone", sys_clone},
    {122, "uname", sys_uname},
    {158, "sched_yield", sys_sched_yield},
    {172, "prctl", sys_prctl},
    {191, "ugetrlimit", sys_ugetrlimit},
    {199, "getuid32", sys_zero},
    {200, "getgid32", sys_zero},
    {201, "geteuid32", sys_zero},
    {202, "getegid32", sys_zero},
    {224, "gettid", sys_gettid},
    {241, "sched_setaffinity", sys_zero},
    {242, "sched_getaffinity", sys_sched_getaffinity},
    {248, "exit_group", sys_exit_group},
    {256, "set_tid_address", sys_set_tid_address},
    {338, "set_robust_list", sys_zero},
    {369, "prlimit64", sys_prlimit64},
    {384, "getrandom", sys_getrandom},
    {0xf0002, "cacheflush", sys_cacheflush},
    {0xf0005, "set_tls", sys_set_tls},
    {0, NULL, NULL},
};
