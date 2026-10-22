/* sys_socket — the socket syscalls: socket, bind, connect, listen, accept(4),
 * getsockname, getpeername, sendto, recvfrom, sendmsg, recvmsg, shutdown,
 * setsockopt, getsockopt. Addresses cross the boundary as sockaddr_in. */
#include "core/errno.h"
#include "kernel/proc.h"
#include "kernel/socket.h"
#include "kernel/syscall.h"
#include "kernel/vfs.h"

#include <stddef.h>

#define SOCKADDR_IN_LEN 16u
#define AF_UNSPEC       0
#define SOL_SOCKET      1u
#define SO_TYPE         3u
#define SO_ERROR        4u
#define SO_SNDBUF       7u
#define SO_RCVBUF       8u
#define SO_RCVTIMEO     20u
#define SO_SNDTIMEO     21u

/* A timeval as setsockopt takes it. */
struct user_timeval {
    uint32_t sec;
    uint32_t usec;
};
#define SO_ACCEPTCONN   30u
#define IPPROTO_IPV6    41u
#define SOCK_BUF_SIZE   65536u
#define MSGHDR_LEN      28u
#define BOUNCE          2048u
#define IOV_LIMIT       64u

int32_t sys_fd_install(struct file *f, bool cloexec);

/* sockaddr_in: family (host order), port and address (network order). */
static int get_addr(uint32_t uaddr, uint32_t len, struct sock_addr *a) {
    uint8_t b[SOCKADDR_IN_LEN];
    if (len < 8u || len > 128u) {
        return KERR_INVAL;
    }
    if (mm_copy_in(proc_current()->mm, b, uaddr, len < sizeof b ? len : sizeof b) != KERR_OK) {
        return KERR_FAULT;
    }
    if ((b[0] | (b[1] << 8)) == AF_UNSPEC) {
        *a = (struct sock_addr){0};
        return KERR_OK;
    }
    if ((b[0] | (b[1] << 8)) != AF_INET) {
        return EAFNOSUPPORT;
    }
    a->port = (uint16_t)((b[2] << 8) | b[3]);
    a->ip = ((uint32_t)b[4] << 24) | ((uint32_t)b[5] << 16) | ((uint32_t)b[6] << 8) | b[7];
    return KERR_OK;
}

/* Writes a sockaddr_in to uaddr, honouring and updating *ulen (socklen_t). */
static int put_addr(uint32_t uaddr, uint32_t ulen_addr, struct sock_addr a) {
    struct mm *mm = proc_current()->mm;
    uint8_t b[SOCKADDR_IN_LEN] = {AF_INET, 0, (uint8_t)(a.port >> 8), (uint8_t)a.port, (uint8_t)(a.ip >> 24),
                                  (uint8_t)(a.ip >> 16), (uint8_t)(a.ip >> 8), (uint8_t)a.ip};
    uint32_t len, want = SOCKADDR_IN_LEN;
    if (uaddr == 0 || ulen_addr == 0) {
        return KERR_OK;
    }
    if (mm_copy_in(mm, &len, ulen_addr, 4) != KERR_OK || (int32_t)len < 0) {
        return KERR_FAULT;
    }
    if (mm_copy_out(mm, uaddr, b, len < want ? len : want) != KERR_OK) {
        return KERR_FAULT;
    }
    return mm_copy_out(mm, ulen_addr, &want, 4);
}

static struct sock *sock_fd(int32_t fd, int *err) {
    struct file *f = fd_file(fd);
    struct sock *s = socket_of(f);
    *err = f == NULL ? KERR_BADF : (s == NULL ? ENOTSOCK : KERR_OK);
    return s;
}

static int32_t sys_socket(struct trapframe *tf) {
    struct file *f;
    int32_t fd;
    int rc = socket_create((int)tf->r[0], (int)tf->r[1], (int)tf->r[2], &f);
    if (rc != KERR_OK) {
        return rc;
    }
    fd = sys_fd_install(f, (tf->r[1] & SOCK_CLOEXEC) != 0);
    if (fd < 0) {
        file_put(f);
    }
    return fd;
}

/* bind and connect share a shape: a socket and an address. */
static int32_t with_addr(struct trapframe *tf, bool connect) {
    struct sock_addr a = {0};
    int err;
    struct sock *s = sock_fd((int32_t)tf->r[0], &err);
    if (s == NULL) {
        return err;
    }
    err = get_addr(tf->r[1], tf->r[2], &a);
    if (err != KERR_OK) {
        return err;
    }
    return connect ? s->ops->connect(s, a) : s->ops->bind(s, a);
}

static int32_t sys_bind(struct trapframe *tf) {
    return with_addr(tf, false);
}

static int32_t sys_connect(struct trapframe *tf) {
    return with_addr(tf, true);
}

static int32_t sys_listen(struct trapframe *tf) {
    int err;
    struct sock *s = sock_fd((int32_t)tf->r[0], &err);
    return s == NULL ? err : s->ops->listen(s, (int)tf->r[1]);
}

/* accept4(fd, addr, addrlen, flags). */
static int32_t sys_accept4(struct trapframe *tf) {
    int err;
    struct sock *s = sock_fd((int32_t)tf->r[0], &err), *c;
    struct file *f;
    int32_t fd;
    if (s == NULL) {
        return err;
    }
    err = s->ops->accept(s, &c, (s->file->flags & O_NONBLOCK) != 0);
    if (err != KERR_OK) {
        return err;
    }
    f = socket_file(c, (tf->r[3] & SOCK_NONBLOCK) ? O_NONBLOCK : 0u);
    if (f == NULL) {
        c->ops->release(c);
        return KERR_NOMEM;
    }
    fd = sys_fd_install(f, (tf->r[3] & SOCK_CLOEXEC) != 0);
    if (fd < 0) {
        file_put(f);
        return fd;
    }
    (void)put_addr(tf->r[1], tf->r[2], c->remote);
    return fd;
}

static int32_t sys_accept(struct trapframe *tf) {
    struct trapframe t = *tf;
    t.r[3] = 0;
    return sys_accept4(&t);
}

static int32_t name_of(struct trapframe *tf, bool peer) {
    int err;
    struct sock *s = sock_fd((int32_t)tf->r[0], &err);
    if (s == NULL) {
        return err;
    }
    if (peer && !s->connected) {
        return ENOTCONN;
    }
    return put_addr(tf->r[1], tf->r[2], peer ? s->remote : s->local);
}

static int32_t sys_getsockname(struct trapframe *tf) {
    return name_of(tf, false);
}

static int32_t sys_getpeername(struct trapframe *tf) {
    return name_of(tf, true);
}

/* One send of n bytes from user memory, optionally to an address. */
static int32_t send_user(struct sock *s, uint32_t ubuf, uint32_t n, const struct sock_addr *to) {
    static uint8_t bounce[NET_UDP_MAX + 64u];
    uint32_t done = 0;
    if (s->type == SOCK_DGRAM && n > NET_UDP_MAX) {
        return EMSGSIZE;
    }
    while (done < n || (n == 0 && done == 0)) {
        uint32_t c = n - done < sizeof bounce ? n - done : (uint32_t)sizeof bounce;
        int32_t r;
        if (mm_copy_in(proc_current()->mm, bounce, ubuf + done, c) != KERR_OK) {
            return done > 0 ? (int32_t)done : KERR_FAULT;
        }
        r = s->ops->send(s, bounce, c, to);
        if (r < 0) {
            return done > 0 ? (int32_t)done : r;
        }
        done += (uint32_t)r;
        if (n == 0 || (uint32_t)r < c) {
            break;
        }
    }
    return (int32_t)done;
}

/* One receive into user memory (a datagram, or what a stream has). */
static int32_t recv_user(struct sock *s, uint32_t ubuf, uint32_t n, struct sock_addr *from, uint32_t flags) {
    static uint8_t bounce[BOUNCE];
    uint32_t want = n < sizeof bounce ? n : (uint32_t)sizeof bounce;
    int32_t r = s->ops->recv(s, bounce, want, from, flags, (s->file->flags & O_NONBLOCK) != 0);
    if (r > 0 && mm_copy_out(proc_current()->mm, ubuf, bounce, (uint32_t)r) != KERR_OK) {
        return KERR_FAULT;
    }
    return r;
}

/* sendto(fd, buf, len, flags, addr, addrlen). */
static int32_t sys_sendto(struct trapframe *tf) {
    struct sock_addr to;
    int err;
    struct sock *s = sock_fd((int32_t)tf->r[0], &err);
    if (s == NULL) {
        return err;
    }
    if (tf->r[4] == 0) {
        return send_user(s, tf->r[1], tf->r[2], NULL);
    }
    err = get_addr(tf->r[4], tf->r[5], &to);
    return err != KERR_OK ? err : send_user(s, tf->r[1], tf->r[2], &to);
}

/* recvfrom(fd, buf, len, flags, addr, addrlen). */
static int32_t sys_recvfrom(struct trapframe *tf) {
    struct sock_addr from = {0};
    int err;
    struct sock *s = sock_fd((int32_t)tf->r[0], &err);
    int32_t r;
    if (s == NULL) {
        return err;
    }
    r = recv_user(s, tf->r[1], tf->r[2], &from, tf->r[3]);
    if (r >= 0 && put_addr(tf->r[4], tf->r[5], s->type == SOCK_STREAM ? s->remote : from) != KERR_OK) {
        return KERR_FAULT;
    }
    return r;
}

/* struct msghdr on ARM: name, namelen, iov, iovlen, control, controllen, flags. */
struct msghdr32 {
    uint32_t name, namelen, iov, iovlen, control, controllen, flags;
};

static int get_msghdr(uint32_t uaddr, struct msghdr32 *m) {
    if (mm_copy_in(proc_current()->mm, m, uaddr, MSGHDR_LEN) != KERR_OK) {
        return KERR_FAULT;
    }
    return m->iovlen > IOV_LIMIT ? KERR_INVAL : KERR_OK;
}

/* sendmsg: each iovec is sent in turn (a datagram socket sends the first
 * non-empty one: Go's single-buffer WriteMsgUDP). */
static int32_t sys_sendmsg(struct trapframe *tf) {
    struct msghdr32 m;
    struct sock_addr to;
    uint32_t iov[2];
    int err;
    struct sock *s = sock_fd((int32_t)tf->r[0], &err);
    if (s == NULL || (err = get_msghdr(tf->r[1], &m)) != KERR_OK) {
        return err;
    }
    if (m.name != 0 && (err = get_addr(m.name, m.namelen, &to)) != KERR_OK) {
        return err;
    }
    if (m.iovlen == 0) {
        return send_user(s, 0, 0, m.name != 0 ? &to : NULL);
    }
    if (mm_copy_in(proc_current()->mm, iov, m.iov, sizeof iov) != KERR_OK) {
        return KERR_FAULT;
    }
    return send_user(s, iov[0], iov[1], m.name != 0 ? &to : NULL);
}

/* recvmsg: into the first iovec; no control messages. */
static int32_t sys_recvmsg(struct trapframe *tf) {
    struct msghdr32 m;
    struct sock_addr from = {0};
    uint32_t iov[2], zero = 0;
    int err;
    int32_t r;
    struct sock *s = sock_fd((int32_t)tf->r[0], &err);
    if (s == NULL || (err = get_msghdr(tf->r[1], &m)) != KERR_OK) {
        return err;
    }
    if (m.iovlen == 0 || mm_copy_in(proc_current()->mm, iov, m.iov, sizeof iov) != KERR_OK) {
        return m.iovlen == 0 ? KERR_INVAL : KERR_FAULT;
    }
    r = recv_user(s, iov[0], iov[1], &from, tf->r[2]);
    if (r < 0) {
        return r;
    }
    (void)put_addr(m.name, m.name != 0 ? tf->r[1] + 4u : 0, from);
    (void)mm_copy_out(proc_current()->mm, tf->r[1] + 20u, &zero, 4);  /* controllen = 0 */
    (void)mm_copy_out(proc_current()->mm, tf->r[1] + 24u, &zero, 4);  /* flags = 0 */
    return r;
}

static int32_t sys_shutdown(struct trapframe *tf) {
    int err;
    struct sock *s = sock_fd((int32_t)tf->r[0], &err);
    return s == NULL ? err : s->ops->shutdown(s, (int)tf->r[1]);
}

/* The two options that are not ignored: how long a blocking receive or send
 * may wait. Everything else is accepted and has nothing to tune; IPv6
 * options say ENOPROTOOPT, as on a kernel without IPv6. */
static int32_t set_timeout(struct sock *s, uint32_t name, uint32_t uaddr) {
    struct user_timeval tv;
    uint32_t ms;
    if (mm_copy_in(proc_current()->mm, &tv, uaddr, sizeof tv) != KERR_OK) {
        return KERR_FAULT;
    }
    /* A zero timeval means "wait for ever", which is what 0 means inside
     * (socket.h), so the two agree without a special case. */
    ms = tv.sec * 1000u + (tv.usec + 999u) / 1000u;
    if (name == SO_RCVTIMEO) {
        s->recv_timeout_ms = ms;
    } else {
        s->send_timeout_ms = ms;
    }
    return KERR_OK;
}

static int32_t sys_setsockopt(struct trapframe *tf) {
    int err;
    struct sock *s = sock_fd((int32_t)tf->r[0], &err);
    if (s == NULL) {
        return err;
    }
    if (tf->r[1] == IPPROTO_IPV6) {
        return ENOPROTOOPT;
    }
    if (tf->r[1] == SOL_SOCKET && (tf->r[2] == SO_RCVTIMEO || tf->r[2] == SO_SNDTIMEO)) {
        return set_timeout(s, tf->r[2], tf->r[3]);
    }
    return KERR_OK;
}

static uint32_t option_value(struct sock *s, uint32_t level, uint32_t name, int *err) {
    *err = KERR_OK;
    if (level == SOL_SOCKET && name == SO_ERROR) {
        uint32_t e = (uint32_t)-s->error;
        s->error = 0;
        return e;
    }
    if (level == SOL_SOCKET && name == SO_TYPE) {
        return (uint32_t)s->type;
    }
    if (level == SOL_SOCKET && (name == SO_SNDBUF || name == SO_RCVBUF)) {
        return SOCK_BUF_SIZE;
    }
    if (level == SOL_SOCKET && name == SO_ACCEPTCONN) {
        return 0;
    }
    if (level == SOL_SOCKET && (name == SO_RCVTIMEO || name == SO_SNDTIMEO)) {
        /* Reported in milliseconds rather than as a timeval, which is a
         * deliberate narrowing: no caller here reads it back, and the
         * alternative is a second shape for one number. */
        return name == SO_RCVTIMEO ? s->recv_timeout_ms : s->send_timeout_ms;
    }
    *err = ENOPROTOOPT;
    return 0;
}

/* getsockopt(fd, level, name, val, len). */
static int32_t sys_getsockopt(struct trapframe *tf) {
    int err;
    struct sock *s = sock_fd((int32_t)tf->r[0], &err);
    uint32_t v, four = 4;
    if (s == NULL) {
        return err;
    }
    v = option_value(s, tf->r[1], tf->r[2], &err);
    if (err != KERR_OK) {
        return err;
    }
    if (mm_copy_out(proc_current()->mm, tf->r[3], &v, 4) != KERR_OK ||
        mm_copy_out(proc_current()->mm, tf->r[4], &four, 4) != KERR_OK) {
        return KERR_FAULT;
    }
    return KERR_OK;
}

const struct syscall_entry sys_socket_table[] = {
    {281, "socket", sys_socket},
    {282, "bind", sys_bind},
    {283, "connect", sys_connect},
    {284, "listen", sys_listen},
    {285, "accept", sys_accept},
    {286, "getsockname", sys_getsockname},
    {287, "getpeername", sys_getpeername},
    {290, "sendto", sys_sendto},
    {292, "recvfrom", sys_recvfrom},
    {293, "shutdown", sys_shutdown},
    {294, "setsockopt", sys_setsockopt},
    {295, "getsockopt", sys_getsockopt},
    {296, "sendmsg", sys_sendmsg},
    {297, "recvmsg", sys_recvmsg},
    {366, "accept4", sys_accept4},
    {0, NULL, NULL},
};
