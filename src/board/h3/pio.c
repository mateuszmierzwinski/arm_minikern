#include "board/h3/pio.h"

#include "arch/arm/layout.h"
#include "board/h3/h3.h"

/* The pin controllers: one bank of registers per port, 0x24 apart, and the
 * same layout in both blocks.
 *
 * There are **two**. PA..PH are in the main controller; **PL is in the
 * always-on one** at 0x01F02C00, and that is the controller that matters
 * for the one job this driver has, because on every H3 board with a
 * switchable CPU supply the pin that switches it is on PL (gpio.h). Driving
 * PL through the main block's base would have written to PA..PH's
 * registers — a different pin entirely.
 *
 * The always-on block also needs its clock ungated first, from the R_CCU at
 * 0x01F01400, which is the same trap the thermal sensor fell into
 * (CLAUDE.md §10): registers that look writable and are not. */
#define H3_PIO_BASE   0x01C20800u
#define H3_R_PIO_BASE 0x01F02C00u
#define R_CCU_BASE    0x01F01400u
#define R_CCU_APB0_GATE 0x28u
#define R_CCU_PIO_BIT   (1u << 0)
#define PIO_PORT_SIZE 0x24u
#define PIO_CFG0      0x00u /* four registers, eight pins each, four bits per pin */
#define PIO_DATA      0x10u

#define PIO_CFG_INPUT  0u
#define PIO_CFG_OUTPUT 1u
#define PIO_CFG_MASK   0x7u
#define PIO_PINS_PER_CFG 8u
#define PIO_CFG_BITS     4u

/* Ungates the always-on pin controller. Set-only, and harmless to repeat:
 * these are hardware facts this kernel has not checked on silicon, and
 * setting a gate for the wrong block costs nothing where clearing one
 * could stop something else. */
static void r_pio_clock_on(void) {
    volatile uint32_t *gate = (volatile uint32_t *)io_virt(R_CCU_BASE + R_CCU_APB0_GATE);
    *gate |= R_CCU_PIO_BIT;
}

static volatile uint32_t *pio(struct gpio_pin pin, uint32_t off) {
    if (gpio_is_r_port(pin)) {
        /* PL is port 0 of the always-on controller, not port 11 of the main
         * one: the index is a letter, the register bank is per block. */
        return (volatile uint32_t *)io_virt(H3_R_PIO_BASE + off);
    }
    return (volatile uint32_t *)io_virt(H3_PIO_BASE + pin.port * PIO_PORT_SIZE + off);
}

void h3_pio_output(struct gpio_pin pin) {
    uint32_t reg = pin.pin / PIO_PINS_PER_CFG;
    if (gpio_is_r_port(pin)) {
        r_pio_clock_on();
    }
    uint32_t shift = (pin.pin % PIO_PINS_PER_CFG) * PIO_CFG_BITS;
    volatile uint32_t *cfg = pio(pin, PIO_CFG0 + reg * 4u);
    *cfg = (*cfg & ~(PIO_CFG_MASK << shift)) | (PIO_CFG_OUTPUT << shift);
}

void h3_pio_write(struct gpio_pin pin, bool high) {
    volatile uint32_t *data = pio(pin, PIO_DATA);
    uint32_t bit = 1u << pin.pin;
    *data = high ? (*data | bit) : (*data & ~bit);
}
