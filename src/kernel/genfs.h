/* genfs — the handful of files the kernel makes up rather than stores.
 *
 * `/etc/resolv.conf` is the one that matters: a Go program's DNS resolver
 * reads it, and the address to put in it arrives in the DHCP lease. So
 * there is no resolver in this kernel at all — the lease becomes a file,
 * and Go's own pure-Go resolver does the rest in user space over the UDP
 * sockets it already has.
 *
 * The root filesystem is a read-only archive, so a generated file cannot
 * simply be put in it: these are matched by exact path, ahead of the
 * archive, and rendered when they are opened. A file is therefore a
 * snapshot — reopen it after the lease changes to see the change.
 */
#ifndef MINIKERN_KERNEL_GENFS_H
#define MINIKERN_KERNEL_GENFS_H

#include <stdbool.h>
#include <stdint.h>

struct file;

/* Opens path when it is one of the generated files. Returns false when it
 * is not one, in which case nothing is touched. */
bool genfs_open(const char *path, uint32_t flags, struct file **out, int *rc);

/* The names, for a directory listing of /etc. */
uint32_t genfs_count(void);
const char *genfs_name(uint32_t index, const char **dir);

#endif
