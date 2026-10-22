/* xfsfs — an XFS filesystem in the namespace.
 *
 * It is the adapter between the pure reader (core/xfs) and the kernel's
 * files: a path under the mount point becomes an inode, and reading that
 * file or listing that directory goes through the reader, the buffer cache
 * and the network block device beneath it.
 *
 * One mount, over the volume /dev/nbd0 names. A read-write mount may write
 * **file data into blocks a file already has** and nothing else: XFS does
 * not journal data, so that changes no metadata. Creating a file, growing
 * one or filling a hole all need the log, which is P9.4 — a filesystem this
 * kernel cannot journal is one it must not restructure.
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
int xfsfs_mount(const char *at, bool read_only);

/* Whether anything is mounted, and where. */
bool xfsfs_mounted(void);
const char *xfsfs_prefix(void);

/* Opens a path **relative to the mount point** ("" is the mount's root). */
/* The permissions a created file gets. There is no umask here and no user
 * to apply one to: one process runs, and it owns everything it makes. */
#define XFSFS_CREATE_MODE 0100644u

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

/* Removes a file from the mounted filesystem. `rest` is the path past the
 * mount point, as xfsfs_open takes it. Refuses what the filesystem refuses,
 * each with its own errno (core/xfs/xfs.h). */
int xfsfs_unlink(const char *rest);

/* Writes back anything the filesystem still holds, if one is mounted.
 *
 * Called when the machine is stopping: a filesystem left dirty because the
 * last program did not think to call fsync is a filesystem that loses what
 * it was told, and "the program should have synced" is not an answer a
 * kernel gets to give. Harmless when nothing is mounted. */
int xfsfs_flush_all(void);

#endif
