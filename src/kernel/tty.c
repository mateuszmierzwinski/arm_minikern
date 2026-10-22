#include "kernel/tty.h"

#include "kernel/sched.h"
#include "kernel/vfs.h"

#define TTY_RING 256u

static char ring[TTY_RING];
static uint32_t head, tail;  /* head: next write; tail: next read */

void tty_receive(char c) {
    if (head - tail < TTY_RING) {
        ring[head++ % TTY_RING] = c;
        sched_wake(ring, 0, SCHED_MAX_TASKS);
        vfs_poll_notify(NULL);
    }
}

/* Blocks until a byte is there; returns what was copied or KERR_INTR. */
static int32_t tty_read(void *self, void *buf, uint32_t n) {
    char *b = buf;
    uint32_t got = 0;
    (void)self;
    while (head == tail) {
        int rc = sched_sleep(ring, 0, 0, true);
        if (rc != 0) {
            return rc;
        }
    }
    while (got < n && tail != head) {
        b[got++] = ring[tail++ % TTY_RING];
    }
    return (int32_t)got;
}

static int tty_ready(void *self) {
    (void)self;
    return head != tail;
}

struct console_input_port tty_input_port(void) {
    return (struct console_input_port){.read = tty_read, .ready = tty_ready, .self = 0};
}
