#include "kernel/nbd.h"

#include <stddef.h>

#include "core/console.h"
#include "core/errno.h"
#include "core/nbd.h"
#include "core/net/tcp.h"
#include "kernel/devfs.h"
#include "kernel/irq.h"
#include "kernel/mem.h"
#include "kernel/net.h"
#include "kernel/time.h"
#include "kernel/vfs.h"

/* Long enough for a server on a busy LAN to answer a 64 KiB request, short
 * enough that a dead server does not hang a boot for ever. */
#define KNBD_IO_TIMEOUT_MS      10000u
#define KNBD_CONNECT_TIMEOUT_MS 5000u

/* The network comes up in the background, so a boot-time connect has to
 * wait for the lease — the same 15 seconds netboot allows. */
#define KNBD_LEASE_WAIT_MS 15000u

/* Everything the live client owns. One of them: one volume. */
static struct {
    struct tcp_conn *conn;
    struct nbd proto;
    struct blockdev dev;
    struct bcache cache;
    struct nbd_transport transport;
    uint64_t io_errors;
    bool up;
} knbd;

static uint64_t now_ms(void) {
    return time_monotonic_ns() / 1000000u;
}

/* Waits until the connection can make progress, or the deadline passes. A
 * closed or reset connection is an error now rather than a wait. */
static int wait_for(uint32_t want, uint64_t deadline) {
    for (;;) {
        uint32_t ready = tcp_poll(knbd.conn);
        if ((ready & want) != 0) {
            return KERR_OK;
        }
        if ((ready & (TCP_POLL_ERR | TCP_POLL_HUP)) != 0) {
            return KERR_IO;
        }
        if (now_ms() >= deadline) {
            return KERR_TIMEDOUT;
        }
        irq_wait();
    }
}

/* The transport's send: all n bytes, or a failure. The send queue is
 * finite, so this is a loop over what TCP will take. */
static int transport_send(void *self, const void *buf, uint32_t n) {
    const uint8_t *at = buf;
    uint64_t deadline = now_ms() + KNBD_IO_TIMEOUT_MS;
    (void)self;
    while (n > 0) {
        int32_t sent;
        int rc = wait_for(TCP_POLL_OUT, deadline);
        if (rc != KERR_OK) {
            return rc;
        }
        sent = tcp_send(knbd.conn, at, n);
        if (sent < 0) {
            return (int)sent;
        }
        at += sent;
        n -= (uint32_t)sent;
    }
    return KERR_OK;
}

/* The transport's recv: exactly n bytes. A server that stops mid-reply ends
 * at the deadline, which is what keeps a hostile peer from wedging us. */
static int transport_recv(void *self, void *buf, uint32_t n) {
    uint8_t *at = buf;
    uint64_t deadline = now_ms() + KNBD_IO_TIMEOUT_MS;
    (void)self;
    while (n > 0) {
        int32_t got;
        int rc = wait_for(TCP_POLL_IN, deadline);
        if (rc != KERR_OK) {
            return rc;
        }
        got = tcp_recv(knbd.conn, at, n, false);
        if (got < 0) {
            return (int)got;
        }
        at += got;
        n -= (uint32_t)got;
    }
    return KERR_OK;
}

/* Opens the connection and waits for the handshake to be possible. */
static int dial(ip4_t server, uint16_t port) {
    uint64_t deadline = now_ms() + KNBD_CONNECT_TIMEOUT_MS;
    int rc;
    knbd.conn = tcp_alloc(knet_tcp());
    if (knbd.conn == NULL) {
        return KERR_NOMEM;
    }
    rc = tcp_connect(knbd.conn, server, port);
    if (rc != KERR_OK) {
        return rc;
    }
    while (!tcp_connect_done(knbd.conn)) {
        if (now_ms() >= deadline) {
            return KERR_TIMEDOUT;
        }
        irq_wait();
    }
    return knbd.conn->error != 0 ? knbd.conn->error : KERR_OK;
}

/* Gives the cache its memory: KNBD_CACHE_SLOTS blocks, taken whole pages at
 * a time while memory is still unfragmented. */
static int build_cache(void) {
    uint32_t bytes = KNBD_CACHE_SLOTS * KNBD_BLOCK;
    void *mem = page_alloc(bytes / 4096u, 1);
    if (mem == NULL) {
        return KERR_NOMEM;
    }
    return bcache_init(&knbd.cache, &knbd.dev, mem, KNBD_CACHE_SLOTS);
}

/* ── /dev/nbd0 ─────────────────────────────────────────────────────────────
 *
 * A block device as a file: byte-granular reads and writes at f->pos,
 * through the cache, so a Go program can treat it as a disk image. Writes
 * are not flushed per call — fsync is what a filesystem (or mkfs) uses to
 * say "now", which keeps formatting a volume from costing one round trip
 * per structure. */

static uint64_t device_bytes(void) {
    return knbd.dev.blocks * knbd.dev.block_size;
}

/* How much of a request fits before the end of the device. */
static uint32_t clamp(uint64_t pos, uint32_t n) {
    uint64_t bytes = device_bytes();
    if (pos >= bytes) {
        return 0;
    }
    return bytes - pos < n ? (uint32_t)(bytes - pos) : n;
}

static int32_t nbd_file_read(struct file *f, void *buf, uint32_t n) {
    uint32_t want = clamp(f->pos, n);
    int rc;
    if (!knbd.up) {
        return KERR_NODEV;
    }
    if (want == 0) {
        return 0;  /* at or past the end: end of file */
    }
    rc = bcache_pread(&knbd.cache, f->pos, buf, want);
    if (rc != KERR_OK) {
        knbd.io_errors++;
        return rc;
    }
    f->pos += want;
    return (int32_t)want;
}

static int32_t nbd_file_write(struct file *f, const void *buf, uint32_t n) {
    uint32_t want = clamp(f->pos, n);
    int rc;
    if (!knbd.up) {
        return KERR_NODEV;
    }
    if (want == 0) {
        return KERR_NOSPC;  /* a block device does not grow */
    }
    rc = bcache_pwrite(&knbd.cache, f->pos, buf, want);
    if (rc != KERR_OK) {
        knbd.io_errors++;
        return rc;
    }
    f->pos += want;
    return (int32_t)want;
}

/* fsync on /dev/nbd0 writes the cache back and flushes the server, which is
 * how mkfs says the volume is complete. */
static int nbd_file_sync(struct file *f) {
    (void)f;
    return knbd_flush();
}

static const struct file_ops nbd_ops = {
    .read = nbd_file_read,
    .write = nbd_file_write,
    .fsync = nbd_file_sync,
};

int knbd_flush(void) {
    if (!knbd.up) {
        return KERR_NODEV;
    }
    return bcache_flush(&knbd.cache);
}

const struct blockdev *knbd_device(void) {
    return knbd.up ? &knbd.dev : NULL;
}

struct bcache *knbd_cache(void) {
    return knbd.up ? &knbd.cache : NULL;
}

int knbd_connect(ip4_t server, uint16_t port, const char *export_name) {
    char ip[16];
    int rc;
    knbd.transport = (struct nbd_transport){.send = transport_send, .recv = transport_recv};
    rc = knet_wait_address(KNBD_LEASE_WAIT_MS);
    if (rc != KERR_OK) {
        kprintf("nbd: no address after %u s, not connecting\n", KNBD_LEASE_WAIT_MS / 1000u);
        return rc;
    }
    kprintf("nbd: connecting to %s:%u export \"%s\"\n", ip4_format(server, ip), port, export_name);
    rc = dial(server, port);
    if (rc == KERR_OK) {
        rc = nbd_connect(&knbd.proto, &knbd.transport, export_name);
    }
    if (rc == KERR_OK) {
        rc = nbd_blockdev(&knbd.proto, KNBD_BLOCK, &knbd.dev);
    }
    if (rc == KERR_OK) {
        rc = build_cache();
    }
    if (rc != KERR_OK) {
        kprintf("nbd: %s:%u failed (%d)\n", ip4_format(server, ip), port, rc);
        return rc;
    }
    knbd.up = true;
    kprintf("nbd: %u blocks of %u bytes (%u MiB)%s\n", (uint32_t)knbd.dev.blocks, knbd.dev.block_size,
            (uint32_t)(device_bytes() / (1024u * 1024u)), knbd.dev.read_only ? ", read-only" : "");
    return devfs_register("nbd0", &nbd_ops, (43u << 8) | 0u, S_IFBLK | 0600u, (uint32_t)device_bytes());
}

void knbd_report(void) {
    if (!knbd.up) {
        kprintf("nbd: not connected\n");
        return;
    }
    kprintf("nbd: cache %u hits %u misses %u writebacks, %u io errors\n", knbd.cache.hits, knbd.cache.misses,
            knbd.cache.writebacks, (uint32_t)knbd.io_errors);
}
