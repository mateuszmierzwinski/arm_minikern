/* tmpfs — a small writable filesystem in memory, mounted at /tmp.
 *
 * It exists because a great deal of Go code assumes it can write
 * *somewhere*: os.CreateTemp, a log, a lock file, the output of something
 * being staged before it is sent. The root filesystem here is a read-only
 * archive and the network volume is read-only too, so without this there is
 * nowhere at all.
 *
 * Deliberately flat and small: files directly in /tmp, no subdirectories,
 * a bounded number of them and a bounded size each, out of the kernel heap.
 * A board with 512 MiB should not be able to lose all of it to a program
 * that forgot to delete something.
 */
#ifndef MINIKERN_KERNEL_TMPFS_H
#define MINIKERN_KERNEL_TMPFS_H

#include <stdbool.h>
#include <stdint.h>

struct file;

/* How much there is: sixteen files of at most 256 KiB, so 4 MiB at worst. */
#define TMPFS_MAX_FILES 16u
#define TMPFS_MAX_BYTES (256u * 1024u)

/* Opens a path relative to the mount point ("" is /tmp itself). Honours
 * O_CREAT, O_TRUNC and O_APPEND. KERR_NOENT for a name that is not there
 * and was not to be created, KERR_NOSPC when the table or the size limit
 * is reached, KERR_INVAL for a name with a '/' in it (there are no
 * subdirectories). */
int tmpfs_open(const char *rest, uint32_t flags, struct file **out);

/* Removes a name. KERR_NOENT when it is not there, KERR_ISDIR for the
 * mount point itself. The bytes go when the last open file does. */
int tmpfs_unlink(const char *rest);

/* True when this file is one of ours (the directory listing asks). */
bool tmpfs_owns(const struct file *f);

/* Lists /tmp, skipping the first `skip` names: returns how many records it
 * wrote into buf and how many names it emitted. */
int32_t tmpfs_getdents(uint32_t skip, uint8_t *buf, uint32_t cap, uint32_t *emitted);

#endif
