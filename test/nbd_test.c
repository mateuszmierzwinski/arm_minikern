#include "core/nbd.h"

#include <string.h>

#include "core/errno.h"
#include "ktest.h"

/* A fake NBD server, driven by what the client sends: enough of a real one
 * to take the client through a handshake and real I/O, with a switch for
 * each way a server can be wrong or hostile. The live test
 * (net_nbd_live_test.sh) runs the same client against qemu-nbd, so the
 * happy path is confirmed by something that is not also ours. */

#define VOL_BYTES  (160u * 1024u)
#define FAKE_CAP   (320u * 1024u)
#define BLOCK      4096u

struct fake {
    uint8_t vol[VOL_BYTES];
    uint8_t out[FAKE_CAP];  /* server -> client, waiting to be read */
    uint32_t out_len, out_at;
    uint8_t in[FAKE_CAP];   /* client -> server, accumulating */
    uint32_t in_len;
    uint32_t phase;         /* 0: client flags, 1: option, 2: transmission */
    uint32_t infos;         /* NBD_REP_INFO replies sent so far */
    /* How to misbehave. */
    int bad_greeting_magic, no_fixed_newstyle, unknown_export, no_export_info;
    int wrong_handle, bad_reply_magic, giant_info, endless_info, hang_up;
    int read_only, no_flush;
    uint32_t error_code;
    struct nbd_transport t;
};

static struct fake f;

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v) {
    put16(p, (uint16_t)(v >> 16));
    put16(p + 2, (uint16_t)v);
}

static void put64(uint8_t *p, uint64_t v) {
    put32(p, (uint32_t)(v >> 32));
    put32(p + 4, (uint32_t)v);
}

static uint16_t get16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t get32(const uint8_t *p) {
    return ((uint32_t)get16(p) << 16) | get16(p + 2);
}

static uint64_t get64(const uint8_t *p) {
    return ((uint64_t)get32(p) << 32) | get32(p + 4);
}

static void emit(const void *buf, uint32_t n) {
    if (f.out_len + n <= FAKE_CAP) {
        memcpy(f.out + f.out_len, buf, n);
        f.out_len += n;
    }
}

static void emit_greeting(void) {
    uint8_t hello[18];
    put64(hello, f.bad_greeting_magic ? 0x4242424242424242ull : 0x4e42444d41474943ull);
    put64(hello + 8, 0x49484156454f5054ull);
    put16(hello + 16, f.no_fixed_newstyle ? 0u : 1u);
    emit(hello, sizeof hello);
}

static void emit_opt_reply(uint32_t type, const void *body, uint32_t len) {
    uint8_t head[20];
    put64(head, 0x0003e889045565a9ull);
    put32(head + 8, 7u);  /* NBD_OPT_GO */
    put32(head + 12, type);
    put32(head + 16, f.giant_info && type == 3u ? 0xfffff000u : len);
    emit(head, sizeof head);
    emit(body, len);
}

static void emit_export_info(void) {
    uint8_t body[12];
    uint16_t flags = 1u;  /* HAS_FLAGS */
    flags = (uint16_t)(flags | (f.read_only ? 2u : 0u) | (f.no_flush ? 0u : 4u));
    put16(body, 0u);  /* NBD_INFO_EXPORT */
    put64(body + 2, VOL_BYTES);
    put16(body + 10, flags);
    emit_opt_reply(3u, body, sizeof body);
}

static void emit_block_size_info(void) {
    uint8_t body[14];
    put16(body, 3u);  /* NBD_INFO_BLOCK_SIZE */
    put32(body + 2, 512u);
    put32(body + 6, BLOCK);
    put32(body + 10, 32u * 1024u * 1024u);
    emit_opt_reply(3u, body, sizeof body);
}

/* The option phase: the information the client asked to go, then the ack. */
static void answer_option(void) {
    if (f.unknown_export) {
        emit_opt_reply(0x80000006u, NULL, 0);  /* NBD_REP_ERR_UNKNOWN */
        return;
    }
    if (f.endless_info) {
        for (uint32_t i = 0; i < 64u; i++) {
            emit_block_size_info();
        }
        return;
    }
    if (!f.no_export_info) {
        emit_export_info();
    }
    emit_block_size_info();
    emit_opt_reply(1u, NULL, 0);  /* NBD_REP_ACK */
}

static void emit_reply(uint64_t handle, uint32_t err) {
    uint8_t rep[16];
    put32(rep, f.bad_reply_magic ? 0xdeadbeefu : 0x67446698u);
    put32(rep + 4, err);
    put64(rep + 8, f.wrong_handle ? handle ^ 0xffull : handle);
    emit(rep, sizeof rep);
}

/* One transmission request, if a whole one has arrived. Returns how many
 * bytes it consumed, or 0 when more are needed. */
static uint32_t answer_request(const uint8_t *req, uint32_t avail) {
    uint16_t cmd = get16(req + 6);
    uint64_t handle = get64(req + 8);
    uint64_t off = get64(req + 16);
    uint32_t len = get32(req + 24);
    if (get32(req) != 0x25609513u || off + len > VOL_BYTES) {
        emit_reply(handle, 22u);  /* EINVAL */
        return 28u;
    }
    if (cmd == 1u && avail < 28u + len) {
        return 0;  /* the data is still on its way */
    }
    if (f.error_code != 0) {
        emit_reply(handle, f.error_code);
        return cmd == 1u ? 28u + len : 28u;
    }
    if (cmd == 0u) {  /* READ */
        emit_reply(handle, 0);
        emit(f.vol + off, len);
    } else if (cmd == 1u) {  /* WRITE */
        memcpy(f.vol + off, req + 28, len);
        emit_reply(handle, 0);
        return 28u + len;
    } else if (cmd == 3u) {  /* FLUSH */
        emit_reply(handle, 0);
    }
    return 28u;  /* DISC gets no reply */
}

/* Consumes every complete message the client has sent. */
static void pump(void) {
    for (;;) {
        uint32_t need = f.phase == 0 ? 4u : f.phase == 1u ? 20u : 28u;
        uint32_t used;
        if (f.in_len < need) {
            return;
        }
        if (f.phase == 0) {
            used = 4u;
            f.phase = 1u;
        } else if (f.phase == 1u) {
            uint32_t name_len = get32(f.in + 16);
            if (f.in_len < 20u + name_len + 2u) {
                return;
            }
            used = 20u + name_len + 2u;
            f.phase = 2u;
            answer_option();
        } else {
            used = answer_request(f.in, f.in_len);
            if (used == 0) {
                return;
            }
        }
        memmove(f.in, f.in + used, f.in_len - used);
        f.in_len -= used;
    }
}

static int fake_send(void *self, const void *buf, uint32_t n) {
    (void)self;
    if (f.in_len + n > FAKE_CAP) {
        return KERR_IO;
    }
    memcpy(f.in + f.in_len, buf, n);
    f.in_len += n;
    pump();
    return KERR_OK;
}

/* A recv with nothing to give is a server that stopped talking, which is
 * what the transport reports when its deadline passes. */
static int fake_recv(void *self, void *buf, uint32_t n) {
    (void)self;
    if (f.hang_up || f.out_len - f.out_at < n) {
        return KERR_TIMEDOUT;
    }
    memcpy(buf, f.out + f.out_at, n);
    f.out_at += n;
    return KERR_OK;
}

static struct nbd nbd;

static void setup(void) {
    memset(&f, 0, sizeof f);
    for (uint32_t i = 0; i < VOL_BYTES; i++) {
        f.vol[i] = (uint8_t)(i * 7u + (i >> 9));
    }
    f.t = (struct nbd_transport){.send = fake_send, .recv = fake_recv};
    emit_greeting();
}

static void connect_ok(void) {
    setup();
    KTEST_CHECK(nbd_connect(&nbd, &f.t, "vol") == KERR_OK);
}

static void test_handshake(void) {
    connect_ok();
    KTEST_CHECK(nbd.size == VOL_BYTES);
    KTEST_CHECK(nbd.min_block == 512u && nbd.pref_block == BLOCK);
    KTEST_CHECK((nbd.flags & NBD_FLAG_SEND_FLUSH) != 0);
    KTEST_CHECK((nbd.flags & NBD_FLAG_READ_ONLY) == 0);
    KTEST_CHECK(nbd.ready);
    /* The export name reached the server. */
    KTEST_CHECK(memcmp(f.in, "", 0) == 0);
}

static void test_read(void) {
    uint8_t buf[1024];
    connect_ok();
    KTEST_CHECK(nbd_pread(&nbd, 2048u, buf, sizeof buf) == KERR_OK);
    KTEST_CHECK(memcmp(buf, f.vol + 2048u, sizeof buf) == 0);
    KTEST_CHECK(nbd_pread(&nbd, VOL_BYTES - 4u, buf, 4u) == KERR_OK);
}

static void test_write_then_read_back(void) {
    uint8_t buf[600];
    uint8_t back[600];
    connect_ok();
    memset(buf, 0x9c, sizeof buf);
    KTEST_CHECK(nbd_pwrite(&nbd, 1000u, buf, sizeof buf) == KERR_OK);
    KTEST_CHECK(memcmp(f.vol + 1000u, buf, sizeof buf) == 0);
    KTEST_CHECK(nbd_pread(&nbd, 1000u, back, sizeof back) == KERR_OK);
    KTEST_CHECK(memcmp(back, buf, sizeof back) == 0);
    KTEST_CHECK(f.vol[999] != 0x9c && f.vol[1600] != 0x9c);  /* nothing either side */
}

/* More than NBD_MAX_IO has to become several requests, and still arrive. */
static void test_a_big_transfer_is_split(void) {
    static uint8_t big[NBD_MAX_IO + 4096u];
    static uint8_t back[NBD_MAX_IO + 4096u];
    connect_ok();
    for (uint32_t i = 0; i < sizeof big; i++) {
        big[i] = (uint8_t)(i ^ 0x5a);
    }
    KTEST_CHECK(nbd_pwrite(&nbd, 0, big, sizeof big) == KERR_OK);
    KTEST_CHECK(nbd.next_handle == 2u);  /* two requests, not one */
    KTEST_CHECK(nbd_pread(&nbd, 0, back, sizeof back) == KERR_OK);
    KTEST_CHECK(memcmp(back, big, sizeof big) == 0);
}

static void test_flush(void) {
    connect_ok();
    KTEST_CHECK(nbd_flush(&nbd) == KERR_OK);
    setup();
    f.no_flush = 1;
    KTEST_CHECK(nbd_connect(&nbd, &f.t, "vol") == KERR_OK);
    KTEST_CHECK(nbd_flush(&nbd) == KERR_NOTSUP);
}

static void test_a_read_only_export_refuses_writes(void) {
    uint8_t buf[8] = {0};
    setup();
    f.read_only = 1;
    KTEST_CHECK(nbd_connect(&nbd, &f.t, "vol") == KERR_OK);
    KTEST_CHECK(nbd_pwrite(&nbd, 0, buf, sizeof buf) == KERR_ROFS);
    KTEST_CHECK(nbd.next_handle == 0);  /* refused here, never sent */
}

static void test_past_the_end_is_refused_locally(void) {
    uint8_t buf[16] = {0};
    connect_ok();
    KTEST_CHECK(nbd_pread(&nbd, VOL_BYTES, buf, 1u) == KERR_INVAL);
    KTEST_CHECK(nbd_pread(&nbd, VOL_BYTES - 4u, buf, 8u) == KERR_INVAL);
    KTEST_CHECK(nbd_pwrite(&nbd, VOL_BYTES - 1u, buf, 2u) == KERR_INVAL);
    KTEST_CHECK(nbd_pread(&nbd, 0, buf, 0) == KERR_OK);  /* nothing is nothing */
    KTEST_CHECK(nbd.next_handle == 0);  /* none of that was even sent */
}

static void test_a_server_that_is_not_an_nbd_server(void) {
    setup();
    f.bad_greeting_magic = 1;
    f.out_len = 0;
    emit_greeting();
    KTEST_CHECK(nbd_connect(&nbd, &f.t, "vol") == KERR_PROTO);
    KTEST_CHECK(!nbd.ready);
}

static void test_an_oldstyle_server_is_refused(void) {
    setup();
    f.no_fixed_newstyle = 1;
    f.out_len = 0;
    emit_greeting();
    KTEST_CHECK(nbd_connect(&nbd, &f.t, "vol") == KERR_NOTSUP);
}

static void test_an_unknown_export(void) {
    setup();
    f.unknown_export = 1;
    KTEST_CHECK(nbd_connect(&nbd, &f.t, "nope") == KERR_NOENT);
    KTEST_CHECK(!nbd.ready);
}

static void test_an_export_with_no_size(void) {
    setup();
    f.no_export_info = 1;
    KTEST_CHECK(nbd_connect(&nbd, &f.t, "vol") == KERR_PROTO);
}

/* A length field that would overrun the client's buffer must be refused by
 * the client, not believed: this is the whole class of bug the stack's
 * hostile tests exist for. */
static void test_a_giant_info_length(void) {
    setup();
    f.giant_info = 1;
    KTEST_CHECK(nbd_connect(&nbd, &f.t, "vol") == KERR_PROTO);
}

static void test_an_endless_stream_of_information(void) {
    setup();
    f.endless_info = 1;
    KTEST_CHECK(nbd_connect(&nbd, &f.t, "vol") == KERR_PROTO);
}

static void test_a_reply_with_the_wrong_magic(void) {
    uint8_t buf[16];
    connect_ok();
    f.bad_reply_magic = 1;
    KTEST_CHECK(nbd_pread(&nbd, 0, buf, sizeof buf) == KERR_PROTO);
    KTEST_CHECK(!nbd.ready);  /* the stream is finished, not retried */
    KTEST_CHECK(nbd_pread(&nbd, 0, buf, sizeof buf) == KERR_PROTO);
}

/* A reply for a request we did not make means the stream is out of step. */
static void test_a_reply_for_another_request(void) {
    uint8_t buf[16];
    connect_ok();
    f.wrong_handle = 1;
    KTEST_CHECK(nbd_pread(&nbd, 0, buf, sizeof buf) == KERR_PROTO);
    KTEST_CHECK(!nbd.ready);
}

static void test_the_servers_error_numbers(void) {
    uint8_t buf[16] = {0};
    connect_ok();
    f.error_code = 22u;
    KTEST_CHECK(nbd_pread(&nbd, 0, buf, sizeof buf) == KERR_INVAL);
    f.error_code = 28u;
    KTEST_CHECK(nbd_pwrite(&nbd, 0, buf, sizeof buf) == KERR_NOSPC);
    f.error_code = 30u;
    KTEST_CHECK(nbd_pwrite(&nbd, 0, buf, sizeof buf) == KERR_ROFS);
    f.error_code = 5u;
    KTEST_CHECK(nbd_pread(&nbd, 0, buf, sizeof buf) == KERR_IO);
    KTEST_CHECK(nbd.ready);  /* an error is not a desync */
}

static void test_a_server_that_stops_talking(void) {
    uint8_t buf[16];
    connect_ok();
    f.hang_up = 1;
    KTEST_CHECK(nbd_pread(&nbd, 0, buf, sizeof buf) == KERR_TIMEDOUT);
}

static void test_disconnect(void) {
    connect_ok();
    KTEST_CHECK(nbd_disconnect(&nbd) == KERR_OK);
    KTEST_CHECK(!nbd.ready);
    KTEST_CHECK(nbd_disconnect(&nbd) == KERR_PROTO);
    KTEST_CHECK(nbd_flush(&nbd) == KERR_PROTO);
}

static void test_the_block_device_view(void) {
    struct blockdev dev;
    uint8_t block[BLOCK];
    connect_ok();
    KTEST_CHECK(nbd_blockdev(&nbd, 500u, &dev) == KERR_INVAL);   /* not a power of two */
    KTEST_CHECK(nbd_blockdev(&nbd, 256u, &dev) == KERR_INVAL);   /* under the minimum */
    KTEST_CHECK(nbd_blockdev(&nbd, BLOCK, &dev) == KERR_OK);
    KTEST_CHECK(dev.block_size == BLOCK && dev.blocks == VOL_BYTES / BLOCK);
    KTEST_CHECK(!dev.read_only);
    KTEST_CHECK(dev.read(dev.self, 2, 1, block) == KERR_OK);
    KTEST_CHECK(memcmp(block, f.vol + 2u * BLOCK, BLOCK) == 0);
    memset(block, 0x11, sizeof block);
    KTEST_CHECK(dev.write(dev.self, 2, 1, block) == KERR_OK);
    KTEST_CHECK(f.vol[2u * BLOCK] == 0x11 && f.vol[3u * BLOCK - 1u] == 0x11);
    KTEST_CHECK(dev.flush(dev.self) == KERR_OK);
    KTEST_CHECK(dev.read(dev.self, dev.blocks, 1, block) == KERR_INVAL);
    KTEST_CHECK(dev.read(dev.self, 0, 0, block) == KERR_INVAL);
    KTEST_CHECK(dev.read(dev.self, 0, 1000u, block) == KERR_INVAL);  /* more than one I/O */
}

/* A read-only export must present a read-only device, so the buffer cache
 * refuses a write before it ever reaches the network. */
static void test_a_read_only_block_device(void) {
    struct blockdev dev;
    uint8_t block[BLOCK] = {0};
    setup();
    f.read_only = 1;
    KTEST_CHECK(nbd_connect(&nbd, &f.t, "vol") == KERR_OK);
    KTEST_CHECK(nbd_blockdev(&nbd, BLOCK, &dev) == KERR_OK);
    KTEST_CHECK(dev.read_only);
    KTEST_CHECK(dev.write(dev.self, 0, 1, block) == KERR_ROFS);
}

int main(void) {
    KTEST_RUN(test_handshake);
    KTEST_RUN(test_read);
    KTEST_RUN(test_write_then_read_back);
    KTEST_RUN(test_a_big_transfer_is_split);
    KTEST_RUN(test_flush);
    KTEST_RUN(test_a_read_only_export_refuses_writes);
    KTEST_RUN(test_past_the_end_is_refused_locally);
    KTEST_RUN(test_a_server_that_is_not_an_nbd_server);
    KTEST_RUN(test_an_oldstyle_server_is_refused);
    KTEST_RUN(test_an_unknown_export);
    KTEST_RUN(test_an_export_with_no_size);
    KTEST_RUN(test_a_giant_info_length);
    KTEST_RUN(test_an_endless_stream_of_information);
    KTEST_RUN(test_a_reply_with_the_wrong_magic);
    KTEST_RUN(test_a_reply_for_another_request);
    KTEST_RUN(test_the_servers_error_numbers);
    KTEST_RUN(test_a_server_that_stops_talking);
    KTEST_RUN(test_disconnect);
    KTEST_RUN(test_the_block_device_view);
    KTEST_RUN(test_a_read_only_block_device);
    return ktest_failures();
}
