#include "kernel/pipe.h"

#include "core/errno.h"
#include "kernel/mem.h"
#include "kernel/sched.h"
#include "kernel/vfs.h"

#include <stdbool.h>
#include <stdint.h>

#define PIPE_MODE 0010000u /* S_IFIFO */

/* The shared state behind both ends. `readers` and `writers` count open
 * files rather than descriptors, so a dup keeps the pipe alive and the last
 * close of an end is what the other end observes. */
struct pipe {
    uint8_t buf[PIPE_BUF];
    uint32_t head; /* next byte to read */
    uint32_t len;  /* bytes held */
    uint32_t readers;
    uint32_t writers;
    struct file *rend;
    struct file *wend;
};

static uint32_t room(const struct pipe *p) {
    return PIPE_BUF - p->len;
}

/* Wakes whoever waits on either end and tells epoll the state moved. One
 * call after every change, because a reader that freed space and a poller
 * waiting for it are two different sleepers. */
static void pipe_changed(struct pipe *p) {
    sched_wake(&p->readers, 0, UINT32_MAX);
    sched_wake(&p->writers, 0, UINT32_MAX);
    if (p->rend != NULL) {
        vfs_poll_notify(p->rend);
    }
    if (p->wend != NULL) {
        vfs_poll_notify(p->wend);
    }
}

static void take(struct pipe *p, uint8_t *out, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        out[i] = p->buf[(p->head + i) % PIPE_BUF];
    }
    p->head = (p->head + n) % PIPE_BUF;
    p->len -= n;
}

static void give(struct pipe *p, const uint8_t *in, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        p->buf[(p->head + p->len + i) % PIPE_BUF] = in[i];
    }
    p->len += n;
}

/* The pipe behind one of its ends. It lives in `shared` rather than `owned`
 * because both ends point at it and `owned` is freed with the file: the first
 * end to close would free the pipe and the second would free it again. The
 * pipe is freed here instead, when both ends have gone. */
static struct pipe *pipe_of(struct file *f) {
    return (struct pipe *)f->shared;
}

static int32_t pipe_read(struct file *f, void *buf, uint32_t n) {
    struct pipe *p = pipe_of(f);
    uint32_t got;
    while (p->len == 0) {
        if (p->writers == 0) {
            return 0; /* every writer is gone: end of file */
        }
        if ((f->flags & O_NONBLOCK) != 0) {
            return KERR_AGAIN;
        }
        int rc = sched_sleep(&p->readers, 0, 0, true);
        if (rc != KERR_OK) {
            return rc;
        }
    }
    got = n < p->len ? n : p->len;
    take(p, buf, got);
    pipe_changed(p);
    return (int32_t)got;
}

/* A write of at most PIPE_BUF waits for room for all of it, so it cannot be
 * split: that atomicity is what lets a child's exec error arrive whole. */
static int32_t pipe_write(struct file *f, const void *buf, uint32_t n) {
    struct pipe *p = pipe_of(f);
    uint32_t want = n < PIPE_BUF ? n : 1u;
    uint32_t put;
    while (room(p) < want) {
        if (p->readers == 0) {
            return KERR_PIPE;
        }
        if ((f->flags & O_NONBLOCK) != 0) {
            return KERR_AGAIN;
        }
        int rc = sched_sleep(&p->writers, 0, 0, true);
        if (rc != KERR_OK) {
            return rc;
        }
    }
    if (p->readers == 0) {
        return KERR_PIPE;
    }
    put = n < room(p) ? n : room(p);
    give(p, buf, put);
    pipe_changed(p);
    return (int32_t)put;
}

static uint32_t pipe_poll_read(struct file *f) {
    struct pipe *p = pipe_of(f);
    uint32_t ready = p->len > 0 ? EPOLLIN : 0;
    return p->writers == 0 ? ready | EPOLLHUP : ready;
}

static uint32_t pipe_poll_write(struct file *f) {
    struct pipe *p = pipe_of(f);
    uint32_t ready = room(p) > 0 ? EPOLLOUT : 0;
    return p->readers == 0 ? ready | EPOLLERR : ready;
}

/* The last close of either end frees the pipe, and the other end sees the
 * change first: a reader blocked on an empty pipe must observe writers == 0
 * rather than keep waiting for a writer that has gone. */
static void end_closed(struct file *f, bool reader) {
    struct pipe *p = pipe_of(f);
    if (p == NULL) {
        return; /* a pipe that failed half-way through being built */
    }
    if (reader) {
        p->readers--;
        p->rend = NULL;
    } else {
        p->writers--;
        p->wend = NULL;
    }
    pipe_changed(p);
    if (p->readers == 0 && p->writers == 0) {
        kfree(p);
    }
}

static void pipe_release_read(struct file *f) {
    end_closed(f, true);
}

static void pipe_release_write(struct file *f) {
    end_closed(f, false);
}

static const struct file_ops read_ops = {
    .read = pipe_read,
    .release = pipe_release_read,
    .poll = pipe_poll_read,
};

static const struct file_ops write_ops = {
    .write = pipe_write,
    .release = pipe_release_write,
    .poll = pipe_poll_write,
};

int pipe_create(uint32_t flags, struct file **read_end, struct file **write_end) {
    struct pipe *p = kzalloc(sizeof *p);
    struct file *r = p == NULL ? NULL : file_new_memory(&read_ops, NULL, 0, PIPE_MODE);
    struct file *w = r == NULL ? NULL : file_new_memory(&write_ops, NULL, 0, PIPE_MODE);
    if (w == NULL) {
        kfree(p);
        if (r != NULL) {
            file_put(r); /* shared is still NULL, which end_closed tolerates */
        }
        return KERR_NOMEM;
    }
    p->readers = 1;
    p->writers = 1;
    p->rend = r;
    p->wend = w;
    r->shared = p;
    w->shared = p;
    r->flags = O_RDONLY | (flags & O_NONBLOCK);
    w->flags = O_WRONLY | (flags & O_NONBLOCK);
    *read_end = r;
    *write_end = w;
    return KERR_OK;
}
