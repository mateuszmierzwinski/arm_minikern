/* uart — UART0 of the H3 as the console port (the NEO's 3-pin debug header,
 * 115200 8N1). Phase 0 relies on U-Boot (or QEMU) having programmed the baud
 * rate; programming it ourselves is TASKS.md P0.8. */
#ifndef MINIKERN_BOARD_H3_UART_H
#define MINIKERN_BOARD_H3_UART_H

#include "core/ports.h"

/* The console port over UART0; writes "\r\n" for every "\n". */
struct console_port h3_uart0_console(void);

/* Turns on the receive interrupt; each byte received goes to push. */
void h3_uart0_enable_rx(void (*push)(char c));

#endif
