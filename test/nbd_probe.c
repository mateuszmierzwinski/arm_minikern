/* nbd_probe — the kernel's NBD client (core/nbd.c), on the build machine,
 * over a real socket.
 *
 * It exists so the client can be driven against qemu-nbd: a reference
 * server written by somebody else, which is the only way to find out
 * whether we implement NBD or merely implement our own fake of it. The
 * protocol code here is the same file the kernel links; only the transport
 * differs, which is the point of the transport being a port.
 *
 *   nbd_probe <host> <port> <export> info
 *   nbd_probe <host> <port> <export> read  <off> <len> <file>
 *   nbd_probe <host> <port> <export> write <off> <file>
 *   nbd_probe <host> <port> <export> fill  <off> <len> <byte>
 *   nbd_probe <host> <port> <export> verify <off> <len> <byte>
 *   nbd_probe <host> <port> <export> rofs            (a write must be refused)
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "core/errno.h"
#include "core/nbd.h"

#define CHUNK 65536u

/* The transport: a TCP socket with a deadline, because the port's contract
 * says the transport owns the timeout. */
static int sock_send(void *self, const void *buf, uint32_t n) {
    int fd = *(int *)self;
    const char *at = buf;
    while (n > 0) {
        ssize_t w = send(fd, at, n, 0);
        if (w <= 0) {
            return errno == EAGAIN ? KERR_TIMEDOUT : KERR_IO;
        }
        at += w;
        n -= (uint32_t)w;
    }
    return KERR_OK;
}

static int sock_recv(void *self, void *buf, uint32_t n) {
    int fd = *(int *)self;
    char *at = buf;
    while (n > 0) {
        ssize_t r = recv(fd, at, n, 0);
        if (r <= 0) {
            return errno == EAGAIN ? KERR_TIMEDOUT : KERR_IO;
        }
        at += r;
        n -= (uint32_t)r;
    }
    return KERR_OK;
}

static int dial(const char *host, const char *port) {
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)atoi(port))};
    struct timeval tv = {.tv_sec = 10};
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    if (fd < 0 || inet_pton(AF_INET, host, &a.sin_addr) != 1) {
        return -1;
    }
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void *must_alloc(uint32_t n) {
    void *p = malloc(n);
    if (p == NULL) {
        fprintf(stderr, "nbd_probe: out of memory\n");
        exit(1);
    }
    return p;
}

/* read <off> <len> <file>: the export's bytes, into a file to compare. */
static int do_read(struct nbd *n, char **av) {
    uint64_t off = strtoull(av[0], NULL, 0);
    uint32_t len = (uint32_t)strtoul(av[1], NULL, 0);
    uint8_t *buf = must_alloc(len);
    int rc = nbd_pread(n, off, buf, len);
    FILE *out = rc == KERR_OK ? fopen(av[2], "wb") : NULL;
    if (out != NULL) {
        rc = fwrite(buf, 1, len, out) == len ? rc : KERR_IO;
        fclose(out);
    } else if (rc == KERR_OK) {
        rc = KERR_IO;
    }
    free(buf);
    return rc;
}

static int do_write(struct nbd *n, char **av) {
    uint64_t off = strtoull(av[0], NULL, 0);
    FILE *in = fopen(av[1], "rb");
    uint8_t *buf = must_alloc(CHUNK);
    int rc = in == NULL ? KERR_NOENT : KERR_OK;
    while (rc == KERR_OK && in != NULL) {
        size_t got = fread(buf, 1, CHUNK, in);
        if (got == 0) {
            break;
        }
        rc = nbd_pwrite(n, off, buf, (uint32_t)got);
        off += got;
    }
    if (in != NULL) {
        fclose(in);
    }
    free(buf);
    return rc == KERR_OK ? nbd_flush(n) : rc;
}

/* fill and verify share their walk: a byte pattern over a range. */
static int do_pattern(struct nbd *n, char **av, int verify) {
    uint64_t off = strtoull(av[0], NULL, 0);
    uint32_t len = (uint32_t)strtoul(av[1], NULL, 0);
    uint8_t byte = (uint8_t)strtoul(av[2], NULL, 0);
    uint8_t *buf = must_alloc(CHUNK);
    int rc = KERR_OK;
    while (len > 0 && rc == KERR_OK) {
        uint32_t chunk = len < CHUNK ? len : CHUNK;
        memset(buf, byte, chunk);
        if (verify) {
            uint8_t *got = must_alloc(chunk);
            rc = nbd_pread(n, off, got, chunk);
            if (rc == KERR_OK && memcmp(got, buf, chunk) != 0) {
                fprintf(stderr, "nbd_probe: contents differ at %llu\n", (unsigned long long)off);
                rc = KERR_IO;
            }
            free(got);
        } else {
            rc = nbd_pwrite(n, off, buf, chunk);
        }
        off += chunk;
        len -= chunk;
    }
    free(buf);
    return rc == KERR_OK && !verify ? nbd_flush(n) : rc;
}

/* A read-only export must refuse a write, and say so as EROFS. */
static int do_rofs(struct nbd *n) {
    uint8_t byte = 0;
    int rc = nbd_pwrite(n, 0, &byte, 1);
    if ((n->flags & NBD_FLAG_READ_ONLY) == 0) {
        fprintf(stderr, "nbd_probe: the export is not read-only\n");
        return KERR_IO;
    }
    printf("rofs: write refused with %d\n", rc);
    return rc == KERR_ROFS ? KERR_OK : KERR_IO;
}

static int run(struct nbd *n, int argc, char **argv) {
    const char *op = argv[4];
    char **av = &argv[5];
    int have = argc - 5;
    if (strcmp(op, "info") == 0) {
        printf("size %llu flags %04x min %u pref %u flush %s ro %s\n", (unsigned long long)n->size,
               n->flags, n->min_block, n->pref_block,
               (n->flags & NBD_FLAG_SEND_FLUSH) != 0 ? "yes" : "no",
               (n->flags & NBD_FLAG_READ_ONLY) != 0 ? "yes" : "no");
        return KERR_OK;
    }
    if (strcmp(op, "read") == 0 && have >= 3) {
        return do_read(n, av);
    }
    if (strcmp(op, "write") == 0 && have >= 2) {
        return do_write(n, av);
    }
    if (strcmp(op, "fill") == 0 && have >= 3) {
        return do_pattern(n, av, 0);
    }
    if (strcmp(op, "verify") == 0 && have >= 3) {
        return do_pattern(n, av, 1);
    }
    if (strcmp(op, "rofs") == 0) {
        return do_rofs(n);
    }
    fprintf(stderr, "nbd_probe: bad operation\n");
    return KERR_INVAL;
}

int main(int argc, char **argv) {
    struct nbd n;
    struct nbd_transport t = {.send = sock_send, .recv = sock_recv};
    int fd, rc;
    if (argc < 5) {
        fprintf(stderr, "usage: %s <host> <port> <export> <op> [args]\n", argv[0]);
        return 2;
    }
    fd = dial(argv[1], argv[2]);
    if (fd < 0) {
        fprintf(stderr, "nbd_probe: cannot reach %s:%s\n", argv[1], argv[2]);
        return 1;
    }
    t.self = &fd;
    rc = nbd_connect(&n, &t, argv[3]);
    if (rc != KERR_OK) {
        fprintf(stderr, "nbd_probe: handshake failed: %d\n", rc);
        close(fd);
        return 1;
    }
    rc = run(&n, argc, argv);
    nbd_disconnect(&n);
    close(fd);
    if (rc != KERR_OK) {
        fprintf(stderr, "nbd_probe: %s failed: %d\n", argv[4], rc);
    }
    return rc == KERR_OK ? 0 : 1;
}
