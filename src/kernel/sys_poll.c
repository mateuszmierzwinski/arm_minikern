/* sys_poll — epoll and eventfd: what Go's netpoller is built on.
 *
 * An epoll instance is a file holding a fixed table of registrations
 * (fd, file, interest, user data). Waiting scans them; when nothing is
 * ready it sleeps on the VFS poll channel, which every readiness change
 * wakes, until the timeout. EPOLLET is honoured with a generation number
 * per file: an edge registration reports again only after a change.
 * Regular files cannot be registered (EPERM, as Linux): Go then uses
 * blocking reads, which is right for an in-memory filesystem. */
#include "core/errno.h"
#include "kernel/mem.h"
#include "kernel/proc.h"
#include "kernel/syscall.h"
#include "kernel/time.h"
#include "kernel/vfs.h"

#include <stddef.h>

#define EPOLL_MAX_WATCH 32u
#define EPOLL_MAX_EVENTS 128u
#define EPOLL_CTL_ADD 1u
#define EPOLL_CTL_DEL 2u
#define EPOLL_CTL_MOD 3u
#define EPOLLET 0x80000000u
#define EPOLLONESHOT 0x40000000u
#define EFD_SEMAPHORE 1u
#define EFD_NONBLOCK 04000u
#define CLOEXEC_FLAG 02000000u
#define EVENTFD_MAX 0xfffffffffffffffeull

int32_t sys_fd_install(struct file *f, bool cloexec);

/* struct epoll_event on ARM EABI: not packed, 16 bytes. */
struct epoll_event {
    uint32_t events;
    uint32_t pad;
    uint64_t data;
};

struct watch {
    struct file *file;  /* holds a reference */
    int32_t fd;
    uint32_t events;
    uint64_t data;
    uint32_t seen_gen;  /* edge-triggered: the generation last reported */
};

struct epoll {
    struct watch w[EPOLL_MAX_WATCH];
};

static void epoll_release(struct file *f) {
    struct epoll *ep = f->priv;
    for (uint32_t i = 0; i < EPOLL_MAX_WATCH; i++) {
        if (ep->w[i].file != NULL) {
            file_put(ep->w[i].file);
        }
    }
    kfree(ep);
}

static const struct file_ops epoll_ops = {.release = epoll_release};

static int32_t sys_epoll_create1(struct trapframe *tf) {
    struct epoll *ep = kzalloc(sizeof *ep);
    struct file *f = ep != NULL ? file_new_memory(&epoll_ops, NULL, 0, 0600u) : NULL;
    int32_t fd;
    if (f == NULL) {
        kfree(ep);
        return KERR_NOMEM;
    }
    f->priv = ep;
    fd = sys_fd_install(f, (tf->r[0] & CLOEXEC_FLAG) != 0);
    if (fd < 0) {
        file_put(f);
    }
    return fd;
}

static struct epoll *epoll_of(int32_t fd) {
    struct file *f = fd_file(fd);
    return f != NULL && f->ops == &epoll_ops ? f->priv : NULL;
}

static struct watch *find_watch(struct epoll *ep, int32_t fd) {
    for (uint32_t i = 0; i < EPOLL_MAX_WATCH; i++) {
        if (ep->w[i].file != NULL && ep->w[i].fd == fd) {
            return &ep->w[i];
        }
    }
    return NULL;
}

static int watch_add(struct epoll *ep, int32_t fd, struct file *f, const struct epoll_event *ev) {
    struct watch *slot = find_watch(ep, -1);
    if (find_watch(ep, fd) != NULL) {
        return KERR_EXIST;
    }
    for (uint32_t i = 0; slot == NULL && i < EPOLL_MAX_WATCH; i++) {
        slot = ep->w[i].file == NULL ? &ep->w[i] : NULL;
    }
    if (slot == NULL) {
        return -28;  /* ENOSPC */
    }
    *slot = (struct watch){.file = file_get(f), .fd = fd, .events = ev->events, .data = ev->data,
                           .seen_gen = f->poll_gen - 1u};
    return KERR_OK;
}

/* EPOLL_CTL_DEL and EPOLL_CTL_MOD on an existing watch. */
static int watch_change(struct epoll *ep, uint32_t op, int32_t fd, const struct epoll_event *ev) {
    struct watch *w = find_watch(ep, fd);
    if (w == NULL) {
        return KERR_NOENT;
    }
    if (op == EPOLL_CTL_DEL) {
        file_put(w->file);
        *w = (struct watch){0};
        return KERR_OK;
    }
    if (op != EPOLL_CTL_MOD) {
        return KERR_INVAL;
    }
    w->events = ev->events;
    w->data = ev->data;
    w->seen_gen = w->file->poll_gen - 1u;
    return KERR_OK;
}

/* epoll_ctl(epfd, op, fd, event). */
static int32_t sys_epoll_ctl(struct trapframe *tf) {
    struct epoll *ep = epoll_of((int32_t)tf->r[0]);
    struct file *f = fd_file((int32_t)tf->r[2]);
    struct epoll_event ev = {0};
    if (ep == NULL || f == NULL) {
        return KERR_BADF;
    }
    if (f->ops->poll == NULL) {
        return KERR_PERM;
    }
    if (tf->r[1] != EPOLL_CTL_DEL && mm_copy_in(proc_current()->mm, &ev, tf->r[3], sizeof ev) != KERR_OK) {
        return KERR_FAULT;
    }
    if (tf->r[1] == EPOLL_CTL_ADD) {
        return watch_add(ep, (int32_t)tf->r[2], f, &ev);
    }
    return watch_change(ep, tf->r[1], (int32_t)tf->r[2], &ev);
}

/* What a watch should report now (0: nothing). */
static uint32_t watch_ready(struct watch *w) {
    uint32_t ready = w->file->ops->poll(w->file) & (w->events | EPOLLERR | EPOLLHUP);
    if (ready == 0 || ((w->events & EPOLLET) && w->seen_gen == w->file->poll_gen)) {
        return 0;
    }
    w->seen_gen = w->file->poll_gen;
    return ready;
}

/* One scan: copies ready events out; returns their count or an error. */
static int32_t collect(struct epoll *ep, uint32_t uevents, uint32_t max) {
    int32_t n = 0;
    for (uint32_t i = 0; i < EPOLL_MAX_WATCH && (uint32_t)n < max; i++) {
        struct watch *w = &ep->w[i];
        struct epoll_event ev;
        if (w->file == NULL || (ev.events = watch_ready(w)) == 0) {
            continue;
        }
        ev.pad = 0;
        ev.data = w->data;
        if (mm_copy_out(proc_current()->mm, uevents + (uint32_t)n * sizeof ev, &ev, sizeof ev) != KERR_OK) {
            return KERR_FAULT;
        }
        n++;
    }
    return n;
}

/* epoll_wait / epoll_pwait(epfd, events, maxevents, timeout_ms[, sigmask]). */
static int32_t sys_epoll_wait(struct trapframe *tf) {
    struct epoll *ep = epoll_of((int32_t)tf->r[0]);
    int32_t timeout = (int32_t)tf->r[3];
    uint64_t deadline = timeout > 0 ? time_monotonic_ns() + (uint64_t)timeout * 1000000u : 0;
    if (ep == NULL) {
        return KERR_BADF;
    }
    if (tf->r[2] == 0 || tf->r[2] > EPOLL_MAX_EVENTS * 8u) {
        return KERR_INVAL;
    }
    for (;;) {
        int32_t n = collect(ep, tf->r[1], tf->r[2]);
        int rc;
        if (n != 0 || timeout == 0) {
            return n;
        }
        rc = sched_sleep(vfs_poll_channel(), 0, deadline, true);
        if (rc == KERR_TIMEDOUT || rc == KERR_INTR) {
            return rc == KERR_INTR ? KERR_INTR : collect(ep, tf->r[1], tf->r[2]);
        }
    }
}

/* ── poll and select ──────────────────────────────────────────────────────
 *
 * Go does not use either: its netpoller is epoll, which is why epoll came
 * first. Everything else does — a shell, a C program, anything ported — and
 * the two of them are the same question asked twice, so they share the
 * waiting, the readiness and the deadline below.
 */

#define POLL_MAX_FDS 64u /* what one call may ask about */

/* poll's bits, which are not epoll's. */
#define POLLIN_BIT   0x001u
#define POLLOUT_BIT  0x004u
#define POLLERR_BIT  0x008u
#define POLLHUP_BIT  0x010u
#define POLLNVAL_BIT 0x020u

/* One pollfd as user space lays it out: fd, what to watch, what happened. */
struct user_pollfd {
    int32_t fd;
    uint16_t events;
    uint16_t revents;
};

/* What a descriptor can say right now, in poll's bits.
 *
 * A descriptor that is not open is POLLNVAL rather than an error for the
 * whole call: one bad entry in a list of sixty must not blind the caller to
 * the other fifty-nine. */
static uint16_t poll_state(struct proc *p, int32_t fd, uint16_t want) {
    uint32_t ready;
    struct file *f = fd >= 0 && (uint32_t)fd < PROC_MAX_FDS ? p->fds[fd] : NULL;
    if (fd < 0) {
        return 0; /* a negative fd is ignored, as poll(2) says */
    }
    if (f == NULL) {
        return POLLNVAL_BIT;
    }
    ready = f->ops->poll != NULL ? f->ops->poll(f) : (EPOLLIN | EPOLLOUT);
    uint16_t out = 0;
    out |= (ready & EPOLLIN) != 0 && (want & POLLIN_BIT) != 0 ? POLLIN_BIT : 0;
    out |= (ready & EPOLLOUT) != 0 && (want & POLLOUT_BIT) != 0 ? POLLOUT_BIT : 0;
    /* Errors and hangups are reported whether they were asked for or not. */
    out |= (ready & EPOLLERR) != 0 ? POLLERR_BIT : 0;
    out |= (ready & EPOLLHUP) != 0 ? POLLHUP_BIT : 0;
    return out;
}

/* Fills in revents for every entry and counts the ready ones. */
static int32_t poll_scan(struct proc *p, struct user_pollfd *fds, uint32_t n) {
    int32_t ready = 0;
    for (uint32_t i = 0; i < n; i++) {
        fds[i].revents = poll_state(p, fds[i].fd, fds[i].events);
        ready += fds[i].revents != 0 ? 1 : 0;
    }
    return ready;
}

/* The shared body of poll and select: scan, and sleep until something
 * changes or the deadline passes. `deadline` of 0 means wait for ever, and
 * a negative timeout is how both calls spell that. */
static int32_t poll_wait(struct proc *p, struct user_pollfd *fds, uint32_t n, int32_t timeout_ms) {
    uint64_t deadline = timeout_ms > 0 ? time_monotonic_ns() + (uint64_t)timeout_ms * 1000000ull : 0;
    for (;;) {
        int32_t ready = poll_scan(p, fds, n);
        int rc;
        if (ready != 0 || timeout_ms == 0) {
            return ready;
        }
        rc = sched_sleep(vfs_poll_channel(), 0, deadline, true);
        if (rc == KERR_INTR) {
            return KERR_INTR;
        }
        if (rc == KERR_TIMEDOUT) {
            return poll_scan(p, fds, n);
        }
    }
}

static int32_t sys_poll(struct trapframe *tf) {
    struct user_pollfd fds[POLL_MAX_FDS];
    uint32_t n = tf->r[1];
    struct proc *p = proc_current();
    int32_t ready;
    if (n > POLL_MAX_FDS) {
        return KERR_INVAL;
    }
    if (n > 0 && mm_copy_in(p->mm, fds, tf->r[0], n * (uint32_t)sizeof fds[0]) != KERR_OK) {
        return KERR_FAULT;
    }
    ready = poll_wait(p, fds, n, (int32_t)tf->r[2]);
    if (ready < 0) {
        return ready;
    }
    if (n > 0 && mm_copy_out(p->mm, tf->r[0], fds, n * (uint32_t)sizeof fds[0]) != KERR_OK) {
        return KERR_FAULT;
    }
    return ready;
}

/* select's three bitmaps, which are the same question in an older shape.
 *
 * It is built on poll rather than beside it: one implementation of "what is
 * ready, and how long will you wait" is enough, and two would drift. */
#define SELECT_WORDS ((POLL_MAX_FDS + 31u) / 32u)

/* Turns the bitmaps into pollfds, and says how many there are. */
static uint32_t select_to_poll(const uint32_t *r, const uint32_t *w, const uint32_t *e, uint32_t nfds,
                               struct user_pollfd *fds) {
    uint32_t n = 0;
    for (uint32_t fd = 0; fd < nfds; fd++) {
        uint16_t want = 0;
        want |= (r[fd / 32u] >> (fd % 32u)) & 1u ? POLLIN_BIT : 0;
        want |= (w[fd / 32u] >> (fd % 32u)) & 1u ? POLLOUT_BIT : 0;
        want |= (e[fd / 32u] >> (fd % 32u)) & 1u ? POLLERR_BIT : 0;
        if (want != 0) {
            fds[n].fd = (int32_t)fd;
            fds[n].events = want;
            fds[n].revents = 0;
            n++;
        }
    }
    return n;
}

/* And back again. select counts a descriptor once per bitmap it is ready
 * in, which is not how poll counts, so the total is recomputed here. */
static int32_t poll_to_select(const struct user_pollfd *fds, uint32_t n, uint32_t *r, uint32_t *w,
                              uint32_t *e) {
    int32_t ready = 0;
    for (uint32_t i = 0; i < SELECT_WORDS; i++) {
        r[i] = 0;
        w[i] = 0;
        e[i] = 0;
    }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t fd = (uint32_t)fds[i].fd, bit = 1u << (fd % 32u);
        if ((fds[i].revents & (POLLIN_BIT | POLLHUP_BIT)) != 0) {
            r[fd / 32u] |= bit;
            ready++;
        }
        if ((fds[i].revents & POLLOUT_BIT) != 0) {
            w[fd / 32u] |= bit;
            ready++;
        }
        if ((fds[i].revents & (POLLERR_BIT | POLLNVAL_BIT)) != 0) {
            e[fd / 32u] |= bit;
            ready++;
        }
    }
    return ready;
}

/* A timeval, which is what select measures its patience in. */
struct user_timeval {
    uint32_t sec;
    uint32_t usec;
};

static int32_t sys_select(struct trapframe *tf) {
    uint32_t sets[3][SELECT_WORDS];
    struct user_pollfd fds[POLL_MAX_FDS];
    struct user_timeval tv = {0, 0};
    struct proc *p = proc_current();
    uint32_t nfds = tf->r[0], n;
    int32_t timeout = -1, ready;
    if (nfds > POLL_MAX_FDS) {
        return KERR_INVAL;
    }
    for (uint32_t i = 0; i < 3u; i++) {
        uint32_t uaddr = tf->r[1u + i];
        for (uint32_t w = 0; w < SELECT_WORDS; w++) {
            sets[i][w] = 0;
        }
        if (uaddr != 0 && mm_copy_in(p->mm, sets[i], uaddr, SELECT_WORDS * 4u) != KERR_OK) {
            return KERR_FAULT;
        }
    }
    if (tf->r[4] != 0) {
        if (mm_copy_in(p->mm, &tv, tf->r[4], sizeof tv) != KERR_OK) {
            return KERR_FAULT;
        }
        timeout = (int32_t)(tv.sec * 1000u + tv.usec / 1000u);
    }
    n = select_to_poll(sets[0], sets[1], sets[2], nfds, fds);
    ready = poll_wait(p, fds, n, timeout);
    if (ready < 0) {
        return ready;
    }
    ready = poll_to_select(fds, n, sets[0], sets[1], sets[2]);
    for (uint32_t i = 0; i < 3u; i++) {
        if (tf->r[1u + i] != 0 &&
            mm_copy_out(p->mm, tf->r[1u + i], sets[i], SELECT_WORDS * 4u) != KERR_OK) {
            return KERR_FAULT;
        }
    }
    return ready;
}

/* eventfd: a 64-bit counter; read takes it (or 1 in semaphore mode). */
struct eventfd {
    uint64_t count;
    uint32_t flags;
};

static int32_t eventfd_read(struct file *f, void *buf, uint32_t n) {
    struct eventfd *e = f->priv;
    uint64_t v;
    if (n < 8) {
        return KERR_INVAL;
    }
    while (e->count == 0) {
        int rc;
        if (f->flags & EFD_NONBLOCK) {
            return KERR_AGAIN;
        }
        rc = sched_sleep(vfs_poll_channel(), 0, 0, true);
        if (rc == KERR_INTR) {
            return rc;
        }
    }
    v = (e->flags & EFD_SEMAPHORE) ? 1u : e->count;
    e->count -= v;
    *(uint64_t *)buf = v;
    vfs_poll_notify(f);
    return 8;
}

static int32_t eventfd_write(struct file *f, const void *buf, uint32_t n) {
    struct eventfd *e = f->priv;
    uint64_t v;
    if (n < 8) {
        return KERR_INVAL;
    }
    v = *(const uint64_t *)buf;
    if (v == ~0ull || e->count + v > EVENTFD_MAX) {
        return (f->flags & EFD_NONBLOCK) ? KERR_AGAIN : KERR_INVAL;
    }
    e->count += v;
    vfs_poll_notify(f);
    return 8;
}

static uint32_t eventfd_poll(struct file *f) {
    struct eventfd *e = f->priv;
    return (e->count > 0 ? EPOLLIN : 0u) | (e->count < EVENTFD_MAX ? EPOLLOUT : 0u);
}

static void eventfd_release(struct file *f) {
    kfree(f->priv);
}

static const struct file_ops eventfd_ops = {
    .read = eventfd_read, .write = eventfd_write, .poll = eventfd_poll, .release = eventfd_release,
};

/* eventfd2(initval, flags). */
static int32_t sys_eventfd2(struct trapframe *tf) {
    struct eventfd *e = kzalloc(sizeof *e);
    struct file *f = e != NULL ? file_new_memory(&eventfd_ops, NULL, 0, 0600u) : NULL;
    int32_t fd;
    if (f == NULL) {
        kfree(e);
        return KERR_NOMEM;
    }
    *e = (struct eventfd){.count = tf->r[0], .flags = tf->r[1]};
    f->priv = e;
    f->flags = O_RDWR | (tf->r[1] & EFD_NONBLOCK);
    fd = sys_fd_install(f, (tf->r[1] & CLOEXEC_FLAG) != 0);
    if (fd < 0) {
        file_put(f);
    }
    return fd;
}

const struct syscall_entry sys_poll_table[] = {
    {142, "select", sys_select},
    {168, "poll", sys_poll},
    {250, "epoll_create", sys_epoll_create1},
    {251, "epoll_ctl", sys_epoll_ctl},
    {252, "epoll_wait", sys_epoll_wait},
    {346, "epoll_pwait", sys_epoll_wait},
    {356, "eventfd2", sys_eventfd2},
    {357, "epoll_create1", sys_epoll_create1},
    {0, NULL, NULL},
};
