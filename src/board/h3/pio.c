#include "board/h3/pio.h"

#include "arch/arm/layout.h"
#include "board/h3/h3.h"

/* The pin controller: one bank of registers per port, 0x24 apart. */
#define H3_PIO_BASE   0x01C20800u
#define PIO_PORT_SIZE 0x24u
#define PIO_CFG0      0x00u /* four registers, eight pins each, four bits per pin */
#define PIO_DATA      0x10u

#define PIO_CFG_INPUT  0u
#define PIO_CFG_OUTPUT 1u
#define PIO_CFG_MASK   0x7u
#define PIO_PINS_PER_CFG 8u
#define PIO_CFG_BITS     4u

static volatile uint32_t *pio(struct gpio_pin pin, uint32_t off) {
    return (volatile uint32_t *)io_virt(H3_PIO_BASE + pin.port * PIO_PORT_SIZE + off);
}

void h3_pio_output(struct gpio_pin pin) {
    uint32_t reg = pin.pin / PIO_PINS_PER_CFG;
    uint32_t shift = (pin.pin % PIO_PINS_PER_CFG) * PIO_CFG_BITS;
    volatile uint32_t *cfg = pio(pin, PIO_CFG0 + reg * 4u);
    *cfg = (*cfg & ~(PIO_CFG_MASK << shift)) | (PIO_CFG_OUTPUT << shift);
}

void h3_pio_write(struct gpio_pin pin, bool high) {
    volatile uint32_t *data = pio(pin, PIO_DATA);
    uint32_t bit = 1u << pin.pin;
    *data = high ? (*data | bit) : (*data & ~bit);
}
