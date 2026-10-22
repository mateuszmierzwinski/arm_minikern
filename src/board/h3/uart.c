#include "board/h3/uart.h"

#include <stdint.h>

#include "arch/arm/layout.h"
#include "board/h3/h3.h"
#include "kernel/irq.h"
#include "kernel/panic.h"

#define UART_THR 0x00u  /* transmit holding register */
#define UART_RBR 0x00u  /* receive buffer register (read) */
#define UART_IER 0x04u  /* interrupt enable */
#define UART_IER_ERBFI (1u << 0)  /* received data available */
#define UART_LSR_DR (1u << 0)
#define UART_LSR 0x14u  /* line status: register 5, 4-byte stride */
#define UART_LSR_THRE (1u << 5)

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
