/* wdt — the H3's watchdog: a timer that reboots the machine unless the
 * kernel keeps telling it not to.
 *
 * It is the one thing that gets a wedged board back, and the one kind of
 * failure nothing else here can report: a kernel that stops taking
 * interrupts cannot print, cannot panic and cannot be asked anything. The
 * software equivalent — `minikern.hangcheck`, which notices every core idle
 * with a sleeper nothing will wake — needs the tick to still be running to
 * notice anything at all. This covers the case where even that has stopped.
 *
 * Unlike the thermal sensor and the PLLs, **QEMU models this one**
 * (`allwinner-wdt-sun6i`), so it is tested rather than written and hoped
 * for: a deliberate spin with interrupts masked must reboot the machine.
 */
#ifndef MINIKERN_BOARD_H3_WDT_H
#define MINIKERN_BOARD_H3_WDT_H

#include <stdbool.h>
#include <stdint.h>

/* Starts the watchdog with the shortest interval that is at least seconds
 * long, and returns the interval it actually got — the hardware offers a
 * fixed set, so asking for 7 gives 8. Returns 0 when seconds is outside
 * what it can do, having changed nothing. */
uint32_t h3_wdt_start(uint32_t seconds);

/* Tells it the kernel is still alive. */
void h3_wdt_pet(void);

/* Stops it, for an orderly power-off that must not be interrupted. */
void h3_wdt_stop(void);

#endif
