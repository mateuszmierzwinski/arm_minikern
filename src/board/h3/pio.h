/* pio — the H3's pin controller, as much of it as driving one output needs.
 *
 * **Unverified against silicon**: QEMU does not model the pin controller at
 * all, so a write here goes nowhere on the emulator and has never moved a
 * real pin. The part that *is* tested is which pin was meant
 * (`core/gpio.h`), because that is the part a mistake would turn into
 * driving the wrong line.
 */
#ifndef MINIKERN_BOARD_H3_PIO_H
#define MINIKERN_BOARD_H3_PIO_H

#include "core/gpio.h"

/* Makes the pin an output. */
void h3_pio_output(struct gpio_pin pin);

/* Drives it. */
void h3_pio_write(struct gpio_pin pin, bool high);

#endif
