/* trap.h — the saved state of an interrupted context, and trap dispatch.
 *
 * The layout is fixed by vectors.S. A trap from user mode leaves its frame
 * at the very top of the thread's kernel stack, so the frame *is* the
 * thread's user context: the syscall layer reads r0-r7 from it and writes
 * the result into r[0], and signal delivery rewrites pc/sp in it. */
#ifndef MINIKERN_ARCH_ARM_TRAP_H
#define MINIKERN_ARCH_ARM_TRAP_H

#include <stdbool.h>
#include <stdint.h>

struct trapframe {
    uint32_t r[13];
    uint32_t lr_svc;
    uint32_t sp_usr;
    uint32_t lr_usr;
    uint32_t pc;    /* where the trap returns to */
    uint32_t spsr;  /* the interrupted CPSR */
};

enum trap_kind {
    TRAP_RESET = 0,
    TRAP_UNDEF = 1,
    TRAP_SVC = 2,
    TRAP_PREFETCH_ABORT = 3,
    TRAP_DATA_ABORT = 4,
    TRAP_IRQ = 6,
    TRAP_FIQ = 7,
};

/* What a trap kind is handed to. Each is set once at boot by trap_set_*;
 * an unset handler (or a trap with no handler) panics with a register
 * dump. Fault handlers return true when they resolved the fault and the
 * interrupted code may resume. */
struct trap_handlers {
    void (*syscall)(struct trapframe *tf);
    void (*irq)(struct trapframe *tf);
    bool (*fault)(struct trapframe *tf, enum trap_kind kind, uint32_t addr, uint32_t status);
    void (*undef)(struct trapframe *tf);
    void (*return_to_user)(struct trapframe *tf);  /* runs last on every return to user mode */
};

/* Installs the vector table (VBAR) and the handlers. */
void trap_init(const struct trap_handlers *handlers);

/* Called from vectors.S with the frame just saved. */
void trap_dispatch(struct trapframe *tf, uint32_t kind);

/* True when the frame was interrupted in user mode. */
static inline bool trap_from_user(const struct trapframe *tf) {
    return (tf->spsr & 0x1fu) == 0x10u;
}

/* Prints every register of the frame to the kernel log. */
void trap_dump(const struct trapframe *tf);

#endif
