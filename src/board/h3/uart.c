#include "board/h3/uart.h"

#include <stdint.h>

#include "arch/arm/layout.h"
#include "board/h3/h3.h"
#include "kernel/irq.h"
#include "kernel/panic.h"

#define UART_THR 0x00u  /* transmit holding register */
#define UART_RBR 0x00u  /* receive buffer register (read) */
#define UART_DLL 0x00u  /* divisor, low byte — with LCR.DLAB set */
#define UART_DLH 0x04u  /* divisor, high byte — likewise */
#define UART_IER 0x04u  /* interrupt enable */
#define UART_IER_ERBFI (1u << 0)  /* received data available */
#define UART_FCR 0x08u  /* FIFO control */
#define UART_LCR 0x0cu  /* line control */
#define UART_LSR_DR (1u << 0)
#define UART_LSR 0x14u  /* line status: register 5, 4-byte stride */
#define UART_LSR_THRE (1u << 5)
#define UART_LSR_TEMT (1u << 6)  /* the shift register is empty too */

#define UART_LCR_8N1  0x03u      /* eight bits, no parity, one stop bit */
#define UART_LCR_DLAB (1u << 7)  /* the divisor shows through the first two registers */
#define UART_FCR_ON   0x07u      /* FIFOs on, both of them cleared */

/* The clock and the reset: UART0 is bit 16 of each. */
#define CCU_BUS_GATE3 0x06cu
#define CCU_BUS_RST4  0x2d8u
#define CCU_UART0_BIT (1u << 16)

/* APB2 feeds the UART at 24 MHz, and a 16550 divides it by sixteen times
 * the divisor. 13 gives 115384 baud against a wanted 115200 — 0.16% out,
 * where a UART tolerates about 2%. */
#define UART_APB2_HZ   24000000u
#define UART_BAUD      115200u
#define UART_DIVISOR   (UART_APB2_HZ / (16u * UART_BAUD))

static inline uint32_t mmio_read(uintptr_t addr) {
    return *(volatile uint32_t *)addr;
}

static inline void mmio_write(uintptr_t addr, uint32_t v) {
    *(volatile uint32_t *)addr = v;
}

static void uart_put(uintptr_t base, char c) {
    while ((mmio_read(base + UART_LSR) & UART_LSR_THRE) == 0) {
    }
    mmio_write(base + UART_THR, (uint8_t)c);
}

static void uart_write(void *self, const char *buf, size_t len) {
    uintptr_t base = (uintptr_t)self;
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '\n') {
            uart_put(base, '\r');
        }
        uart_put(base, buf[i]);
    }
}

/* Sets the port up from nothing: the clock, the reset, the baud rate, the
 * frame and the FIFOs.
 *
 * U-Boot leaves UART0 configured and QEMU needs no setup at all, so this is
 * not needed today — which is exactly the problem it solves. A kernel that
 * depends on whoever ran before it having set the console up has no console
 * the first time something else boots it, and a silent board is the hardest
 * kind to look at. Doing it here costs one function and removes that
 * dependency.
 *
 * It is also self-checking in a way few things here are: get the divisor,
 * the gate or the frame wrong and the console goes silent, which every test
 * in the project notices at once. */
void h3_uart0_init(void) {
    uintptr_t base = io_virt(H3_UART0_BASE);
    volatile uint32_t *gate = (volatile uint32_t *)io_virt(H3_CCU_BASE + CCU_BUS_GATE3);
    volatile uint32_t *reset = (volatile uint32_t *)io_virt(H3_CCU_BASE + CCU_BUS_RST4);
    *gate |= CCU_UART0_BIT;
    *reset |= CCU_UART0_BIT;
    /* Let anything already in the shift register leave before the frame
     * changes under it, or the last line of whoever ran before us becomes
     * rubbish on the wire. */
    while ((mmio_read(base + UART_LSR) & UART_LSR_TEMT) == 0) {
    }
    mmio_write(base + UART_IER, 0);
    mmio_write(base + UART_LCR, UART_LCR_8N1 | UART_LCR_DLAB);
    mmio_write(base + UART_DLL, UART_DIVISOR & 0xffu);
    mmio_write(base + UART_DLH, (UART_DIVISOR >> 8) & 0xffu);
    mmio_write(base + UART_LCR, UART_LCR_8N1);
    mmio_write(base + UART_FCR, UART_FCR_ON);
}

struct console_port h3_uart0_console(void) {
    return (struct console_port){.write = uart_write, .self = (void *)io_virt(H3_UART0_BASE)};
}

static void (*rx_push)(char c);

/* Drains every received byte; reading RBR clears the interrupt. */
static void uart_rx_irq(void *self) {
    uintptr_t base = (uintptr_t)self;
    while (mmio_read(base + UART_LSR) & UART_LSR_DR) {
        char c = (char)mmio_read(base + UART_RBR);
        if (rx_push != NULL) {
            rx_push(c);
        }
    }
}

void h3_uart0_enable_rx(void (*push)(char c)) {
    static struct irq_handler handler = {.handle = uart_rx_irq};
    uintptr_t base = io_virt(H3_UART0_BASE);
    rx_push = push;
    handler.self = (void *)base;
    if (irq_register(H3_IRQ_UART0, &handler) != 0) {
        panic("uart: receive interrupt already claimed");
    }
    mmio_write(base + UART_IER, mmio_read(base + UART_IER) | UART_IER_ERBFI);
}
