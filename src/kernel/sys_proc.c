/* sys_proc — process and thread syscalls: exit, exit_group, clone (threads),
 * the ids, set_tid_address, scheduling, limits, uname, prctl, getrandom,
 * and ARM's private set_tls and cacheflush. */
#include "arch/arm/smp.h"
#include "arch/arm/mmu.h"
#include "core/errno.h"
#include "kernel/exec.h"
#include "kernel/mem.h"
#include "kernel/proc.h"
#include "kernel/random.h"
#include "kernel/syscall.h"
#include "kernel/vfp.h"

#include <stddef.h>

#define CLONE_VM             0x00000100u
#define CLONE_SIGHAND        0x00000800u
#define CLONE_THREAD         0x00010000u
#define CLONE_SETTLS         0x00080000u
#define CLONE_PARENT_SETTID  0x00100000u
#define CLONE_CHILD_CLEARTID 0x00200000u
#define CLONE_CHILD_SETTID   0x01000000u
#define CLONE_VFORK          0x00004000u
#define CLONE_PIDFD          0x00001000u
#define CLONE_THREAD_MODEL   (CLONE_VM | CLONE_SIGHAND | CLONE_THREAD)

/* How many arguments and how many bytes of them an execve may carry. The
 * limits exist to be hit: an argument list is user-supplied input, and the
 * one page it is copied into is the bound (CLAUDE.md §5, bounded parsing). */
#define EXEC_MAX_ARGS  64u
#define EXEC_ARG_BYTES 4096u

#define WNOHANG 1u
#define P_PIDFD 3u
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

/* Where an execve's strings are copied to. One pool holds the arguments and
 * the environment together, which is both the simpler thing and the right
 * one: the limit a caller must respect is the total, as ARG_MAX is on Linux.
 *
 * It is static because 4 KiB must not go on an 8 KiB kernel stack, and that
 * is safe for the reason every static buffer here is safe: one core is
 * inside the kernel at a time (kernel/klock.h), and nothing between the
 * first byte and the last sleeps. */
struct arg_pool {
    char buf[EXEC_ARG_BYTES];
    uint32_t used;
};

/* Copies a NULL-terminated user vector of user strings into the pool,
 * filling out[] with pointers into it.
 *
 * The pool is a parameter rather than a local static because argv and envp
 * are two calls into one storage: sharing a buffer but restarting at its
 * beginning made the environment overwrite the arguments, and the child
 * execed with "HOME=/" as its program name. */
static int copy_string_vector(struct arg_pool *pool, uint32_t uvec, const char **out, uint32_t cap,
                              uint32_t *count) {
    struct mm *mm = proc_current()->mm;
    uint32_t n = 0;
    for (; uvec != 0 && n < cap; n++) {
        uint32_t ptr = 0;
        int rc = mm_copy_in(mm, &ptr, uvec + n * 4u, 4);
        if (rc != KERR_OK) {
            return rc;
        }
        if (ptr == 0) {
            break;
        }
        if (pool->used >= sizeof pool->buf) {
            return KERR_2BIG;
        }
        rc = mm_copy_string_in(mm, pool->buf + pool->used, ptr, (uint32_t)(sizeof pool->buf - pool->used));
        if (rc < 0) {
            return rc == KERR_NAMETOOLONG ? KERR_2BIG : rc;
        }
        out[n] = pool->buf + pool->used;
        pool->used += (uint32_t)rc + 1u;
    }
    if (n == cap) {
        return KERR_2BIG;
    }
    *count = n;
    return KERR_OK;
}

/* execve: the arguments are copied out of the address space before anything
 * replaces it, because by the end of this they are pointers into memory that
 * no longer exists (kernel/exec.h). */
static int32_t sys_execve(struct trapframe *tf) {
    static struct arg_pool pool;
    static char path[256];
    const char *argv[EXEC_MAX_ARGS];
    const char *envp[EXEC_MAX_ARGS];
    uint32_t argc = 0, envc = 0;
    pool.used = 0;
    /* mm_copy_string_in answers with the string's **length**, not KERR_OK
     * (mm.h). Reading it as a status made execve "fail" with 5 for a
     * five-character path — and because a successful execve never returns,
     * Go's child read that as an error with errno 0 and killed itself. */
    int rc = mm_copy_string_in(proc_current()->mm, path, tf->r[0], sizeof path);
    rc = rc < 0 ? rc : KERR_OK;
    if (rc == KERR_OK) {
        rc = copy_string_vector(&pool, tf->r[1], argv, EXEC_MAX_ARGS, &argc);
    }
    if (rc == KERR_OK) {
        rc = copy_string_vector(&pool, tf->r[2], envp, EXEC_MAX_ARGS, &envc);
    }
    if (rc != KERR_OK) {
        return rc;
    }
    return exec_replace(path, argv, argc, envp, envc, tf);
}

/* wait4: reaps one ended child and reports how it ended. rusage is accepted
 * and ignored, which is what every caller here does with it. */
static int32_t sys_wait4(struct trapframe *tf) {
    uint32_t status = 0;
    int rc = proc_wait(tf->r[0], (tf->r[2] & WNOHANG) != 0, &status);
    if (rc == KERR_AGAIN && (tf->r[2] & WNOHANG) != 0) {
        return 0; /* WNOHANG with nothing to report is 0, not an error */
    }
    if (rc < 0) {
        return (int32_t)rc;
    }
    if (tf->r[1] != 0 && mm_copy_out(proc_current()->mm, tf->r[1], &status, 4) != KERR_OK) {
        return KERR_FAULT;
    }
    return (int32_t)rc;
}

/* pidfd_open: refused on purpose, and refused by name so that the log does
 * not report it as a syscall nobody has thought about.
 *
 * A pidfd is a descriptor standing for a process, which solves a problem
 * this kernel does not have: nothing here recycles a pid while a parent
 * still holds it. Go probes for one, takes ENOSYS for an answer and uses
 * wait4 instead — measured, not assumed (CLAUDE.md §7). */
static int32_t sys_pidfd_open(struct trapframe *tf) {
    (void)tf;
    return KERR_NOSYS;
}

/* waitid, for the one use it is put to here: Go probes whether it can wait
 * on a pidfd before it tries to make one. There are none, so the honest
 * answer is that there is no such child — which is what Linux answers too
 * when the descriptor is not one (measured, CLAUDE.md §7). */
static int32_t sys_waitid(struct trapframe *tf) {
    return tf->r[0] == P_PIDFD ? KERR_CHILD : KERR_NOSYS;
}

static int32_t sys_sched_yield(struct trapframe *tf) {
    (void)tf;
    sched_yield();
    return 0;
}

/* The cores the scheduler is really using, as a one-word mask. Returns its
 * size, as Linux does. Go reads this to size GOMAXPROCS, so reporting one
 * core while running four would waste exactly the parallelism SMP bought. */
static int32_t sys_sched_getaffinity(struct trapframe *tf) {
    uint32_t mask = (1u << smp_scheduling()) - 1u;
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
    /* The child starts with the parent's floating-point state, which may
     * be in the registers rather than in the parent's structure: asking the
     * unit for it is the only way to copy what the parent actually has. */
    vfp_capture(parent);
    child->vfp = parent->vfp;
    proc_attach_thread(parent->proc, child);
    return child;
}

/* clone(flags, newsp, parent_tid, tls, child_tid): threads only. */
/* The child of a vfork: a process of its own sharing the parent's address
 * space, with one thread that returns 0 from this same syscall on the same
 * user stack. The parent is then suspended until the child execs or exits,
 * which is what makes sharing a stack safe — and why the suspension is not
 * optional (proc.h).
 *
 * Go's `os/exec` asks for exactly this (`CLONE_VM | CLONE_VFORK`), measured
 * on linux/arm; see CLAUDE.md §7. */
static int32_t clone_vfork(struct trapframe *tf, uint32_t flags) {
    struct proc *parent = proc_current();
    struct proc *child = proc_vfork(parent, parent->name);
    struct task *t = child == NULL ? NULL : sched_new_task(child);
    struct trapframe *ctf;
    if (t == NULL) {
        return KERR_AGAIN;
    }
    ctf = sched_trapframe(t);
    *ctf = *tf;
    ctf->r[0] = 0;
    ctf->sp_usr = tf->r[1] != 0 ? tf->r[1] : tf->sp_usr;
    t->tls = sched_current()->tls;
    vfp_capture(sched_current());
    t->vfp = sched_current()->vfp;
    proc_attach_thread(child, t);
    sched_start(t);
    /* The child's struct outlives its exit — it becomes a zombie, because
     * this parent has not reaped it yet — so waiting on a flag inside it is
     * safe. Uninterruptible: a vfork parent that woke early would run on a
     * stack the child is still using. */
    while ((flags & CLONE_VFORK) != 0 && child->vfork_pending) {
        (void)sched_sleep(&child->ppid, 0, 0, false);
    }
    return (int32_t)child->pid;
}

static int32_t sys_clone(struct trapframe *tf) {
    uint32_t flags = tf->r[0];
    struct proc *self = proc_current();
    struct mm *mm = self->mm;
    struct task *child;
    /* A thread must not be born into a group that is already ending.
     *
     * exit_group marks its siblings and they stop at their next entry into
     * the kernel, so a clone can be in flight on another core while the
     * sweep runs: the new thread then arrives *after* it, keeps the thread
     * count above zero, and the process never ends — its parent waits in
     * wait4 for ever and every core goes idle. The caller is itself marked
     * dead and exits on its way back to user mode, so refusing here costs
     * it nothing. Found by the hang check printing the task table: a
     * sleeping thread belonging to a process with no other threads.
     *
     * The same applies to a thread that has itself been marked dead — by a
     * sibling's exit_group, or by an execve clearing the way for a new
     * image — which is why its own flag is checked too: a thread on its way
     * out must not leave another behind it. */
    if (self->exiting || sched_killed()) {
        return KERR_AGAIN;
    }
    if ((flags & CLONE_PIDFD) != 0) {
        /* No pidfds here. Go asks for one first and falls back when it is
         * refused, which is measured behaviour, not a hope: see the trace in
         * CLAUDE.md §7. */
        return KERR_INVAL;
    }
    if ((flags & CLONE_THREAD) == 0 && (flags & CLONE_VM) != 0) {
        return clone_vfork(tf, flags);
    }
    if ((flags & CLONE_THREAD_MODEL) != CLONE_THREAD_MODEL) {
        return KERR_NOSYS;  /* a fork with its own address space: not yet */
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
    {11, "execve", sys_execve},
    {114, "wait4", sys_wait4},
    {280, "waitid", sys_waitid},
    {434, "pidfd_open", sys_pidfd_open},
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
