/* tty — the console's input side: a ring of received bytes, filled from a
 * UART interrupt and drained by read() on the console devices, which
 * sleeps while it is empty. Raw: no echo, no line editing. */
#ifndef MINIKERN_KERNEL_TTY_H
#define MINIKERN_KERNEL_TTY_H

#include "core/ports.h"

/* Called from the receive interrupt with each byte. Drops bytes when full. */
void tty_receive(char c);

/* The input port over the ring, for devfs. */
struct console_input_port tty_input_port(void);

#endif
