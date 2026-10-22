#include "arch/arm/trap.h"

#include "core/console.h"
#include "kernel/klock.h"
#include "kernel/panic.h"

extern const uint32_t arm_vectors[];

static struct trap_handlers handlers;

static const char *const trap_names[] = {
    "reset", "undefined instruction", "svc", "prefetch abort", "data abort", "?", "irq", "fiq",
};

static uint32_t read_dfar(void) {
    uint32_t v;
    __asm__ volatile("mrc p15, 0, %0, c6, c0, 0" : "=r"(v));
    return v;
}

static uint32_t read_dfsr(void) {
    uint32_t v;
    __asm__ volatile("mrc p15, 0, %0, c5, c0, 0" : "=r"(v));
    return v;
}

static uint32_t read_ifar(void) {
    uint32_t v;
    __asm__ volatile("mrc p15, 0, %0, c6, c0, 2" : "=r"(v));
    return v;
}

static uint32_t read_ifsr(void) {
    uint32_t v;
    __asm__ volatile("mrc p15, 0, %0, c5, c0, 1" : "=r"(v));
    return v;
}

void trap_init(const struct trap_handlers *h) {
    handlers = *h;
    trap_init_cpu();
}

/* VBAR is per core: a core that never set it takes its first exception to
 * address zero, which is nothing at all. */
void trap_init_cpu(void) {
    __asm__ volatile("mcr p15, 0, %0, c12, c0, 0\n\tisb" : : "r"(arm_vectors) : "memory");
}

void trap_dump(const struct trapframe *tf) {
    for (int i = 0; i < 13; i += 4) {
        kprintf("  r%-2d %08x", i, tf->r[i]);
        for (int j = i + 1; j < i + 4 && j < 13; j++) {
            kprintf("  r%-2d %08x", j, tf->r[j]);
        }
        kprintf("\n");
    }
    kprintf("  pc  %08x  spsr %08x  sp_usr %08x  lr_usr %08x  lr_svc %08x\n", tf->pc, tf->spsr,
            tf->sp_usr, tf->lr_usr, tf->lr_svc);
}

/* The end of the line for a trap nothing claimed. */
static void trap_fatal(struct trapframe *tf, uint32_t kind, uint32_t addr, uint32_t status) {
    kprintf("\n%s at pc %08x (addr %08x, status %03x) in %s mode\n", trap_names[kind & 7u], tf->pc, addr,
            status, trap_from_user(tf) ? "user" : "kernel");
    trap_dump(tf);
    panic("unhandled %s", trap_names[kind & 7u]);
}

/* Aborts: the fault address and status register depend on the side. */
static void trap_abort(struct trapframe *tf, uint32_t kind) {
    bool data = kind == TRAP_DATA_ABORT;
    uint32_t addr = data ? read_dfar() : read_ifar();
    uint32_t status = data ? read_dfsr() : read_ifsr();
    if (handlers.fault == NULL || !handlers.fault(tf, (enum trap_kind)kind, addr, status)) {
        trap_fatal(tf, kind, addr, status);
    }
}

/* Runs the handler for kind, or panics when there is none. */
static void trap_route(struct trapframe *tf, uint32_t kind) {
    if (kind == TRAP_SVC && handlers.syscall != NULL) {
        handlers.syscall(tf);
    } else if (kind == TRAP_IRQ && handlers.irq != NULL) {
        handlers.irq(tf);
    } else if (kind == TRAP_UNDEF && handlers.undef != NULL && trap_from_user(tf)) {
        handlers.undef(tf);
    } else if (kind == TRAP_DATA_ABORT || kind == TRAP_PREFETCH_ABORT) {
        trap_abort(tf, kind);
    } else {
        trap_fatal(tf, kind, 0, 0);
    }
}

/* Every way into the kernel comes through here, which is why the kernel
 * lock is taken here (klock.h).
 *
 * It is **released by `trap_exit`**, not here, because that is the one path
 * out to user mode and a freshly started thread takes it without ever
 * having arrived through this function: it inherits the lock from the core
 * that switched to it and would otherwise hold it for ever — which is
 * exactly how the first version of this deadlocked on init's first
 * syscall. A context switch inside the handler hands the lock on in the
 * same way. */
void trap_dispatch(struct trapframe *tf, uint32_t kind) {
    /* A fault in kernel mode is a kernel bug: kernel/fault.c refuses to
     * recover one, so it ends in a panic. The core that took it is already
     * inside the kernel and therefore already holds the lock, so taking it
     * again would deadlock and the panic would never be printed — which is
     * how a missing per-core VFP enable showed up as a silent freeze
     * instead of a message. An interrupt in kernel mode is different: it
     * can only arrive from irq_wait, which gives the lock up first. */
    bool kernel_fault = !trap_from_user(tf) && kind != TRAP_IRQ;
    if (!kernel_fault) {
        klock_acquire();
    }
    trap_route(tf, kind);
    if (trap_from_user(tf) && handlers.return_to_user != NULL) {
        handlers.return_to_user(tf);
    }
}
