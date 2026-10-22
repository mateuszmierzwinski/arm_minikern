/* xfsfs — an XFS filesystem in the namespace.
 *
 * It is the adapter between the pure reader (core/xfs) and the kernel's
 * files: a path under the mount point becomes an inode, and reading that
 * file or listing that directory goes through the reader, the buffer cache
 * and the network block device beneath it.
 *
 * One mount, read-only, over the volume /dev/nbd0 names. Writing is P9.4
 * and the log comes first — a filesystem this kernel cannot journal is one
 * it must not change.
 */
#ifndef MINIKERN_KERNEL_XFSFS_H
#define MINIKERN_KERNEL_XFSFS_H

#include <stdbool.h>
#include <stdint.h>

struct file;

/* Mounts the network block device's filesystem at an absolute path (no
 * trailing slash, e.g. "/mnt"). The path is not copied, so it must be a
 * literal or outlive the mount.
 *
 * KERR_NODEV when no volume is attached, KERR_BUSY when something is
 * already mounted, KERR_INVAL for a path that is not absolute, or whatever
 * xfs_mount refused it for — which the console explains. */
int xfsfs_mount(const char *at);

/* Whether anything is mounted, and where. */
bool xfsfs_mounted(void);
const char *xfsfs_prefix(void);

/* Opens a path **relative to the mount point** ("" is the mount's root). */
int xfsfs_open(const char *rest, uint32_t flags, struct file **out);

/* True when this file is one of ours: the directory listing needs to know
 * whose directory it is looking at. */
bool xfsfs_owns(const struct file *f);

/* Lists the directory f, skipping the first f->pos entries and appending
 * records to buf. Returns how many bytes were written, or a negative
 * error; *advanced says how many entries were emitted.
 *
 * Resuming by a count rather than a cookie makes a long listing quadratic.
 * It is right for the tens of entries a board reads and wrong for a
 * million, which is the same trade the reader makes by walking directory
 * data instead of its hash index (XFS.md §3). */
int32_t xfsfs_getdents(struct file *f, uint8_t *buf, uint32_t cap, uint32_t *advanced);

#endif
