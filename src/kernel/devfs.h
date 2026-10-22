/* devfs — the device nodes under /dev: console, ttyS0 and tty (the serial
 * console), null, zero, full, random and urandom, and stdin/stdout/stderr,
 * which are aliases for the opener's descriptors 0-2. */
#ifndef MINIKERN_KERNEL_DEVFS_H
#define MINIKERN_KERNEL_DEVFS_H

#include <stdint.h>

#include "core/ports.h"

struct file;
struct file_ops;

/* The console the console devices read from and write to. */
void devfs_set_console(const struct console_port *out, const struct console_input_port *in);

/* name is the part after "/dev/". */
int devfs_open(const char *name, uint32_t flags, struct file **out, int *alias_fd);

/* Adds a node at run time, for a device the kernel discovers rather than
 * always has: the NBD client registers "nbd0" when it connects. name and
 * ops must outlive the mount (they are not copied). mode carries the file
 * type (S_IFBLK for a disk) and permissions; size is what fstat and
 * lseek(SEEK_END) report, 0 for a stream. KERR_NOMEM when the table is
 * full, KERR_EXIST when that name is already taken. */
int devfs_register(const char *name, const struct file_ops *ops, uint32_t rdev, uint32_t mode, uint32_t size);

/* A new file on the console: the kernel's own fds 0, 1 and 2 for init. */
struct file *devfs_console_file(uint32_t flags);

#endif
