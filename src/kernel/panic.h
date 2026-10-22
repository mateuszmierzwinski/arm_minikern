/* panic — the kernel's last words. Prints the message and then does what
 * the command line asked for at boot (power off under tests, otherwise
 * stop the CPU with interrupts masked so the log stays on screen). */
#ifndef MINIKERN_KERNEL_PANIC_H
#define MINIKERN_KERNEL_PANIC_H

#include "core/ports.h"

/* Sets the power port panic uses and whether it powers off. */
void panic_attach(const struct power_port *power, int poweroff);

__attribute__((noreturn, format(printf, 1, 2))) void panic(const char *fmt, ...);

#endif
