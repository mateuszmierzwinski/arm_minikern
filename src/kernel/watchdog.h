/* watchdog — keeps the board's watchdog fed while the kernel is alive, so
 * that a kernel which stops being alive is rebooted rather than left.
 *
 * What counts as alive is **the tick still arriving**, because that is the
 * failure this can catch and nothing else can: a kernel that has stopped
 * taking interrupts cannot print, cannot panic and cannot be asked
 * anything. A deadlock with interrupts masked looks exactly like that, and
 * three of the SMP bugs in TASKS.md P10.3 presented that way.
 *
 * It is deliberately not cleverer than that. A watchdog that tried to
 * decide whether work was *progressing* would reboot a board that was
 * merely idle, which is the failure mode that gets watchdogs turned off.
 */
#ifndef MINIKERN_KERNEL_WATCHDOG_H
#define MINIKERN_KERNEL_WATCHDOG_H

#include <stdint.h>

/* What the kernel needs from a watchdog: start it, feed it, stop it. */
struct watchdog_port {
    uint32_t (*start)(uint32_t seconds); /* returns the interval it got, or 0 */
    void (*pet)(void);
    void (*stop)(void);
};

/* Starts the watchdog with the given timeout and feeds it on every tick.
 * Returns the interval the hardware gave, or 0 if it refused — in which
 * case nothing is running and the caller should say so. */
uint32_t watchdog_start(const struct watchdog_port *port, uint32_t seconds);

/* Stops it, for a power-off that must not be cut short. */
void watchdog_stop(void);

#endif
