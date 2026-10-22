/* fault — what a user-mode abort or undefined instruction means: a lazy
 * page to fault in, or a signal (SIGSEGV, SIGBUS, SIGILL) for the thread.
 * Kernel-mode faults are bugs and are left to the trap layer's panic. */
#ifndef MINIKERN_KERNEL_FAULT_H
#define MINIKERN_KERNEL_FAULT_H

#include <stdbool.h>
#include <stdint.h>

#include "arch/arm/trap.h"

/* The trap layer's fault hook: true when handled (resolved or signalled). */
bool fault_handle(struct trapframe *tf, enum trap_kind kind, uint32_t addr, uint32_t status);

/* The trap layer's undefined-instruction hook (user mode only). */
void fault_undefined(struct trapframe *tf);

#endif
