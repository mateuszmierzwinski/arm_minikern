/* devfs — the device nodes under /dev: console, ttyS0 and tty (the serial
 * console), null, zero, full, random and urandom, and stdin/stdout/stderr,
 * which are aliases for the opener's descriptors 0-2. */
#ifndef MINIKERN_KERNEL_DEVFS_H
#define MINIKERN_KERNEL_DEVFS_H

#include <stdint.h>

#include "core/ports.h"

struct file;

/* The console the console devices read from and write to. */
void devfs_set_console(const struct console_port *out, const struct console_input_port *in);

/* name is the part after "/dev/". */
int devfs_open(const char *name, uint32_t flags, struct file **out, int *alias_fd);

/* A new file on the console: the kernel's own fds 0, 1 and 2 for init. */
struct file *devfs_console_file(uint32_t flags);

#endif
