#include "kernel/netboot.h"

#include "core/console.h"
#include "core/errno.h"
#include "core/net/netboot.h"
#include "kernel/irq.h"
#include "kernel/mem.h"
#include "kernel/net.h"
#include "kernel/random.h"
#include "kernel/time.h"
#include "lib/sha256.h"

/* 16 MiB: far above any root filesystem we build (a Go /init is ~2 MiB),
 * and a thirty-second of the NEO's RAM. Taken contiguously at boot, while
 * memory is still unfragmented. */
#define IMAGE_CAPACITY (16u * 1024u * 1024u)
#define LEASE_WAIT_MS  15000u
#define PAGE_BYTES     4096u

static uint32_t bitmap[NETBOOT_BITMAP_WORDS(IMAGE_CAPACITY)];
static struct netboot client;

static uint64_t now_ms(void) {
    return time_monotonic_ns() / 1000000u;
}

/* Reports what was fetched, with the digest the server named. */
static void report_image(const struct netboot *nb) {
    char ip[16], hex[65];
    sha256_format(nb->digest, hex);
    hex[16] = '\0';  /* the first 8 bytes are plenty for a boot log */
    kprintf("netboot: %u bytes from %s in %u blocks (sha256 %s...)\n", nb->image_len, ip4_format(nb->server, ip),
            nb->blocks, hex);
}

/* Drives the session to its end, idling between ticks. */
static int run_session(struct netboot *nb) {
    bool announced = false;
    while (!netboot_finished(nb)) {
        netboot_tick(nb, now_ms());
        if (!announced && nb->state == NETBOOT_FETCHING) {
            char ip[16];
            announced = true;
            kprintf("netboot: server at %s offers %u bytes in %u blocks\n", ip4_format(nb->server, ip),
                    nb->image_len, nb->blocks);
        }
        if (!netboot_finished(nb)) {
            irq_wait();
        }
    }
    netboot_stop(nb);
    if (nb->state != NETBOOT_READY) {
        kprintf("netboot: failed: %s\n", netboot_error_text(nb->error));
        kprintf("netboot: %u probes, %u offers (%u refused), %u reads, %u blocks (%u refused), "
                "%u send errors (last %d)\n", nb->stats.probes_sent, nb->stats.offers_seen,
                nb->stats.offers_refused, nb->stats.reads_sent, nb->stats.blocks_stored,
                nb->stats.blocks_refused, nb->stats.send_errors, nb->stats.last_send_rc);
        return KERR_IO;
    }
    report_image(nb);
    return KERR_OK;
}

/* The board id on the wire: the chip's four words, big-endian. */
static void encode_board_id(const uint32_t board_id[4], uint8_t out[MKNB_BOARD_ID]) {
    for (uint32_t i = 0; i < 4u; i++) {
        out[i * 4] = (uint8_t)(board_id[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(board_id[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(board_id[i] >> 8);
        out[i * 4 + 3] = (uint8_t)board_id[i];
    }
}

/* Takes the image buffer, contiguous and for good. */
static uint8_t *reserve_image(void) {
    uint8_t *buf = page_alloc(IMAGE_CAPACITY / PAGE_BYTES, 1);
    if (buf == NULL) {
        kprintf("netboot: cannot reserve %u MiB for the image\n", IMAGE_CAPACITY / (1024u * 1024u));
    }
    return buf;
}

/* Starts a session with everything it needs. */
/* The pin, kept here until a session exists to apply it to: netboot_start
 * clears the session, so handing it the digest earlier would lose it. */
static uint8_t expected[32];
static bool expected_set;

static int begin(struct netif *nif, const uint32_t board_id[4], ip4_t pinned, uint8_t *buf) {
    uint8_t id[MKNB_BOARD_ID];
    uint32_t xid;
    char ip[16];
    encode_board_id(board_id, id);
    random_fill(&xid, sizeof xid);
    kprintf("netboot: looking for a server %s\n",
            pinned != 0 ? ip4_format(pinned, ip) : "on the local subnet (unicast sweep)");
    int rc = netboot_start(&client, nif, buf, IMAGE_CAPACITY, bitmap, xid, nif->mac, id, pinned);
    if (rc == KERR_OK && expected_set) {
        netboot_expect_digest(&client, expected); /* after the start, which clears the session */
    }
    return rc;
}

/* One hex digit, or -1. */
static int unhex(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

int knetboot_expect(const char *hex, size_t len) {
    uint8_t *digest = expected;
    if (len != 64u) {
        return KERR_INVAL;
    }
    for (uint32_t i = 0; i < 32u; i++) {
        int hi = unhex(hex[i * 2u]), lo = unhex(hex[i * 2u + 1u]);
        if (hi < 0 || lo < 0) {
            return KERR_INVAL;
        }
        digest[i] = (uint8_t)((hi << 4) | lo);
    }
    expected_set = true;
    return KERR_OK;
}

int knetboot_fetch(const uint32_t board_id[4], ip4_t pinned, const uint8_t **image, uint32_t *len) {
    struct netif *nif = knet_interface();
    uint8_t *buf;
    int rc = knet_wait_address(LEASE_WAIT_MS);
    if (rc != KERR_OK) {
        kprintf("netboot: no address after %u s, giving up\n", LEASE_WAIT_MS / 1000u);
    }
    if (rc != KERR_OK) {
        return rc;
    }
    buf = reserve_image();
    if (buf == NULL) {
        return KERR_NOMEM;
    }
    rc = begin(nif, board_id, pinned, buf);
    if (rc == KERR_OK) {
        rc = run_session(&client);
    }
    if (rc != KERR_OK) {
        page_free(buf, IMAGE_CAPACITY / PAGE_BYTES);
        return rc;
    }
    *image = buf;
    *len = client.image_len;
    return KERR_OK;
}
