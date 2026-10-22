/* nbd (kernel) — the live network block device.
 *
 * It wires three pure pieces together: the NBD protocol (core/nbd.h), our
 * own TCP as its transport, and a buffer cache (core/bcache.h) over the
 * block device that results. What comes out is `/dev/nbd0`, which a Go
 * program can open, seek, read and write like a disk — and which a
 * filesystem in the kernel can mount.
 *
 * The transport is synchronous: each send and recv pumps the network by
 * idling with interrupts open until the bytes are there or the deadline
 * passes, which is exactly what netboot does. That is safe here because the
 * kernel is not preemptible and a caller is always a thread that may sleep.
 *
 * One connection, one volume, no reconnection: if the server goes away the
 * device starts failing and stays failed, because a block device that
 * silently loses writes is worse than one that stops.
 */
#ifndef MINIKERN_KERNEL_NBD_H
#define MINIKERN_KERNEL_NBD_H

#include <stdint.h>

#include "core/bcache.h"
#include "core/blockdev.h"
#include "core/net/net.h"

/* The block size the cache and any filesystem above it work in. 4 KiB is
 * XFS's default and the server's preferred I/O size. */
#define KNBD_BLOCK 4096u

/* How much of the volume is kept in memory: 64 blocks, 256 KiB. */
#define KNBD_CACHE_SLOTS 64u

/* NBD's registered port, used when the command line names none. */
#define KNBD_DEFAULT_PORT 10809u

/* Connects to a server and presents its export as /dev/nbd0.
 *
 * KERR_OK, or KERR_TIMEDOUT when the server does not answer, KERR_NOENT
 * when it does not have that export, KERR_PROTO when it does not speak NBD,
 * KERR_NOMEM when the cache cannot be allocated. An export larger than
 * 4 GiB is accepted but only its first 4 GiB can be reached through
 * /dev/nbd0, because a file's size is 32 bits (vfs.h).
 */
int knbd_connect(ip4_t server, uint16_t port, const char *export_name);

/* The block device, or NULL when nothing is connected. Borrowed: it lives
 * as long as the kernel does. */
const struct blockdev *knbd_device(void);

/* The buffer cache over it, for a filesystem that wants to hold blocks. */
struct bcache *knbd_cache(void);

/* Writes everything cached and asks the server to make it durable. */
int knbd_flush(void);

/* Prints the volume and the cache's counters (minikern.netstat=1). */
void knbd_report(void);

#endif
