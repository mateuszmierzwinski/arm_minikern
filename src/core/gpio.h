/* gpio — naming a pin, and nothing else.
 *
 * A pin is a board fact: which one switches the CPU's supply is in the
 * schematic of the board in front of you, and it differs between boards
 * that share this chip. So it is **configuration**, not a constant compiled
 * in — and this is the part of that which can be tested, because turning
 * "PA10" into a port and a pin needs no hardware at all.
 */
#ifndef MINIKERN_CORE_GPIO_H
#define MINIKERN_CORE_GPIO_H

#include <stdbool.h>
#include <stdint.h>

#define GPIO_PORTS 8u    /* PA..PH on an H3 */
#define GPIO_PINS  32u

struct gpio_pin {
    uint8_t port; /* 0 is PA */
    uint8_t pin;
};

/* Parses a name of the form "PA10" or "pa10" — the P is optional, the
 * letter is the port and the number is the pin. False for anything else,
 * including a port past PH, a pin past 31, and trailing rubbish: a
 * mis-typed pin would drive the wrong line, and guessing which one was
 * meant is not something a kernel should do. */
bool gpio_parse(const char *name, uint32_t len, struct gpio_pin *out);

#endif
