/* sys_file — descriptors and files: open/openat, close, read, write, readv,
 * writev, pread64/pwrite64, fsync, lseek/_llseek, ioctl, fcntl(64), fstat64,
 * getdents64, dup/dup2/dup3, readlinkat, getcwd, faccessat. User buffers
 * go through a kernel bounce buffer; file ops never see user pointers. */
#include "core/errno.h"
#include "core/tar.h"
#include "kernel/mem.h"
#include "kernel/pipe.h"
#include "kernel/proc.h"
#include "kernel/syscall.h"

#include <stddef.h>
#include "kernel/vfs.h"
#include "kernel/tmpfs.h"
#include "kernel/xfsfs.h"

#define AT_FDCWD (-100)
#define AT_EMPTY_PATH 0x1000u
#define PATH_CAP 256u
#define BOUNCE 1024u
#define IOV_MAX 1024u
#define F_DUPFD 0u
#define F_GETFD 1u
#define F_SETFD 2u
#define F_GETFL 3u
#define F_SETFL 4u
#define F_DUPFD_CLOEXEC 1030u
#define FD_CLOEXEC 1u
#define MS_RDONLY 1u
#define SEEK_SET 0u
#define SEEK_CUR 1u
#define SEEK_END 2u

static struct proc *cur(void) {
    return proc_current();
}

struct file *fd_file(int32_t fd) {
    struct proc *p = cur();
    return (fd >= 0 && (uint32_t)fd < PROC_MAX_FDS) ? p->fds[fd] : NULL;
}

/* Installs f at the lowest free descriptor >= min. */
static int32_t fd_install(struct file *f, uint32_t min, bool cloexec) {
    struct proc *p = cur();
    for (uint32_t fd = min; fd < PROC_MAX_FDS; fd++) {
        if (p->fds[fd] == NULL) {
            p->fds[fd] = f;
            p->cloexec = cloexec ? (p->cloexec | (1ull << fd)) : (p->cloexec & ~(1ull << fd));
            return (int32_t)fd;
        }
    }
    return KERR_MFILE;
}

/* For the other descriptor-creating domains (epoll, eventfd). */
int32_t sys_fd_install(struct file *f, bool cloexec) {
    return fd_install(f, 0, cloexec);
}

static int32_t fd_close(int32_t fd) {
    struct file *f = fd_file(fd);
    if (f == NULL) {
        return KERR_BADF;
    }
    cur()->fds[fd] = NULL;
    file_put(f);
    return KERR_OK;
}

/* Joins a directory's normalised name and a relative path into raw. */
static int join_dir(const struct file *d, const char *rel, char *raw) {
    uint32_t n = 0;
    for (const char *s = d->name; *s != '\0'; s++) {
        raw[n++] = *s;
    }
    raw[n++] = '/';
    for (const char *s = rel; *s != '\0'; s++) {
        if (n + 1u >= PATH_CAP) {
            return KERR_NAMETOOLONG;
        }
        raw[n++] = *s;
    }
    raw[n] = '\0';
    return KERR_OK;
}

/* Resolves (dirfd, path) to an absolute, normalised path in out (PATH_CAP
 * + 1 bytes). The working directory is always "/". "." and ".." inside a
 * path are not interpreted. */
static int user_path(int32_t dirfd, uint32_t upath, char *out) {
    char rel[PATH_CAP], raw[PATH_CAP];
    int n = mm_copy_string_in(cur()->mm, rel, upath, PATH_CAP - 64u);
    const char *src = rel;
    if (n <= 0) {
        return n == 0 ? KERR_NOENT : n;
    }
    if (rel[0] != '/' && dirfd != AT_FDCWD) {
        struct file *d = fd_file(dirfd);
        if (d == NULL || (d->mode & S_IFMT) != S_IFDIR) {
            return d == NULL ? KERR_BADF : KERR_NOTDIR;
        }
        if (join_dir(d, rel, raw) != KERR_OK) {
            return KERR_NAMETOOLONG;
        }
        src = raw;
    }
    out[0] = '/';
    return tar_normalise(src, out + 1) == KERR_OK ? KERR_OK : KERR_NAMETOOLONG;
}

static int32_t do_open(int32_t dirfd, uint32_t upath, uint32_t flags) {
    char path[PATH_CAP + 1];
    struct file *f;
    int alias = 0;
    int rc = user_path(dirfd, upath, path);
    int32_t fd;
    if (rc != KERR_OK) {
        return rc;
    }
    rc = vfs_open(path, flags, &f, &alias);
    if (rc == VFS_ALIAS) {
        f = fd_file(alias);
        if (f == NULL) {
            return KERR_NOENT;
        }
        file_get(f);
    } else if (rc != KERR_OK) {
        return rc;
    }
    f->flags = (f->flags & ~O_ACCMODE) | (flags & ~O_CLOEXEC);
    fd = fd_install(f, 0, (flags & O_CLOEXEC) != 0);
    if (fd < 0) {
        file_put(f);
    }
    return fd;
}

static int32_t sys_open(struct trapframe *tf) {
    return do_open(AT_FDCWD, tf->r[0], tf->r[1]);
}

static int32_t sys_openat(struct trapframe *tf) {
    return do_open((int32_t)tf->r[0], tf->r[1], tf->r[2]);
}

static int32_t sys_close(struct trapframe *tf) {
    return fd_close((int32_t)tf->r[0]);
}

/* Reads up to len bytes of f into the user buffer, one bounce at a time;
 * stops at the first short read (EOF, or a console line). */
static int32_t read_into(struct file *f, uint32_t ubuf, uint32_t len) {
    uint8_t bounce[BOUNCE];
    uint32_t done = 0;
    if (f->ops->read == NULL) {
        return f->mode & S_IFDIR ? KERR_ISDIR : KERR_INVAL;
    }
    while (done < len) {
        uint32_t want = len - done < BOUNCE ? len - done : BOUNCE;
        int32_t n = f->ops->read(f, bounce, want);
        if (n <= 0) {
            return done > 0 ? (int32_t)done : n;
        }
        if (mm_copy_out(cur()->mm, ubuf + done, bounce, (uint32_t)n) != KERR_OK) {
            return done > 0 ? (int32_t)done : KERR_FAULT;
        }
        done += (uint32_t)n;
        if ((uint32_t)n < want) {
            break;
        }
    }
    return (int32_t)done;
}

static int32_t write_from(struct file *f, uint32_t ubuf, uint32_t len) {
    uint8_t bounce[BOUNCE];
    uint32_t done = 0;
    if (f->ops->write == NULL) {
        return KERR_BADF;
    }
    while (done < len) {
        uint32_t want = len - done < BOUNCE ? len - done : BOUNCE;
        int32_t n;
        if (mm_copy_in(cur()->mm, bounce, ubuf + done, want) != KERR_OK) {
            return done > 0 ? (int32_t)done : KERR_FAULT;
        }
        n = f->ops->write(f, bounce, want);
        if (n <= 0) {
            return done > 0 ? (int32_t)done : n;
        }
        done += (uint32_t)n;
    }
    return (int32_t)done;
}

static bool readable(const struct file *f) {
    return (f->flags & O_ACCMODE) != O_WRONLY;
}

static bool writable(const struct file *f) {
    return (f->flags & O_ACCMODE) != O_RDONLY;
}

static int32_t sys_read(struct trapframe *tf) {
    struct file *f = fd_file((int32_t)tf->r[0]);
    if (f == NULL || !readable(f)) {
        return KERR_BADF;
    }
    return read_into(f, tf->r[1], tf->r[2]);
}

static int32_t sys_write(struct trapframe *tf) {
    struct file *f = fd_file((int32_t)tf->r[0]);
    if (f == NULL || !writable(f)) {
        return KERR_BADF;
    }
    return write_from(f, tf->r[1], tf->r[2]);
}

/* readv/writev: struct iovec {base, len}, gathered one entry at a time. */
static int32_t vector_io(struct trapframe *tf, bool write) {
    struct file *f = fd_file((int32_t)tf->r[0]);
    uint32_t iov[2];
    int32_t total = 0;
    if (f == NULL || (write ? !writable(f) : !readable(f))) {
        return KERR_BADF;
    }
    if (tf->r[2] > IOV_MAX) {
        return KERR_INVAL;
    }
    for (uint32_t i = 0; i < tf->r[2]; i++) {
        int32_t n;
        if (mm_copy_in(cur()->mm, iov, tf->r[1] + i * 8u, sizeof iov) != KERR_OK) {
            return total > 0 ? total : KERR_FAULT;
        }
        n = write ? write_from(f, iov[0], iov[1]) : read_into(f, iov[0], iov[1]);
        if (n < 0) {
            return total > 0 ? total : n;
        }
        total += n;
        if ((uint32_t)n < iov[1]) {
            break;
        }
    }
    return total;
}

static int32_t sys_readv(struct trapframe *tf) {
    return vector_io(tf, false);
}

static int32_t sys_writev(struct trapframe *tf) {
    return vector_io(tf, true);
}

/* Files and block devices have positions; a character device, a socket and
 * a pipe are streams. */
static bool seekable(const struct file *f) {
    uint32_t type = f->mode & S_IFMT;
    return type == S_IFREG || type == S_IFBLK;
}

/* pread64(fd, buf, count, pad, off_lo, off_hi): the position is kept. */
static int32_t sys_pread64(struct trapframe *tf) {
    struct file *f = fd_file((int32_t)tf->r[0]);
    uint64_t saved;
    int32_t n;
    if (f == NULL || !readable(f)) {
        return KERR_BADF;
    }
    if (!seekable(f)) {
        return KERR_SPIPE;
    }
    saved = f->pos;
    f->pos = ((uint64_t)tf->r[5] << 32) | tf->r[4];
    n = read_into(f, tf->r[1], tf->r[2]);
    f->pos = saved;
    return n;
}

/* pwrite64(fd, buf, count, pad, off_lo, off_hi): the position is kept. It
 * is what Go's WriteAt uses, so a program can write a disk image without
 * seeking (mkfs does exactly this). */
static int32_t sys_pwrite64(struct trapframe *tf) {
    struct file *f = fd_file((int32_t)tf->r[0]);
    uint64_t saved;
    int32_t n;
    if (f == NULL || !writable(f)) {
        return KERR_BADF;
    }
    if (!seekable(f)) {
        return KERR_SPIPE;
    }
    saved = f->pos;
    f->pos = ((uint64_t)tf->r[5] << 32) | tf->r[4];
    n = write_from(f, tf->r[1], tf->r[2]);
    f->pos = saved;
    return n;
}

/* fsync and fdatasync: make what was written durable. A file with nothing
 * behind it (a tty, a memory file) has nothing to do and says so, as Linux
 * does for a device that cannot fail.  */
/* pipe2: a pipe and two descriptors for it. Go's exec needs one to carry a
 * failed execve back from the child, so this is part of running a program.
 *
 * Either descriptor failing must leave nothing behind: a half-installed pipe
 * would be a descriptor leak reachable from user space. */
static int32_t sys_pipe2(struct trapframe *tf) {
    struct file *ends[2];
    uint32_t fds[2];
    int32_t rc = (int32_t)pipe_create(tf->r[1], &ends[0], &ends[1]);
    bool cloexec = (tf->r[1] & O_CLOEXEC) != 0;
    if (rc != KERR_OK) {
        return rc;
    }
    for (uint32_t i = 0; i < 2; i++) {
        rc = fd_install(ends[i], 0, cloexec);
        if (rc < 0) {
            while (i-- > 0) {
                (void)fd_close((int32_t)fds[i]);
            }
            file_put(ends[0]);
            file_put(ends[1]);
            return rc;
        }
        fds[i] = (uint32_t)rc;
    }
    if (mm_copy_out(cur()->mm, tf->r[0], fds, sizeof fds) != KERR_OK) {
        (void)fd_close((int32_t)fds[0]);
        (void)fd_close((int32_t)fds[1]);
        return KERR_FAULT;
    }
    return 0;
}

static int32_t sys_fsync(struct trapframe *tf) {
    struct file *f = fd_file((int32_t)tf->r[0]);
    if (f == NULL) {
        return KERR_BADF;
    }
    return f->ops->fsync != NULL ? f->ops->fsync(f) : KERR_OK;
}

static int64_t seek(struct file *f, int64_t off, uint32_t whence) {
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (int64_t)f->pos : (int64_t)f->size;
    if (!seekable(f)) {
        return KERR_SPIPE;
    }
    if (whence > SEEK_END || base + off < 0) {
        return KERR_INVAL;
    }
    f->pos = (uint64_t)(base + off);
    return (int64_t)f->pos;
}

static int32_t sys_lseek(struct trapframe *tf) {
    struct file *f = fd_file((int32_t)tf->r[0]);
    int64_t r = f == NULL ? KERR_BADF : seek(f, (int32_t)tf->r[1], tf->r[2]);
    return r > 0x7fffffff ? -75 : (int32_t)r;  /* EOVERFLOW */
}

/* _llseek(fd, off_hi, off_lo, result*, whence). */
static int32_t sys_llseek(struct trapframe *tf) {
    struct file *f = fd_file((int32_t)tf->r[0]);
    int64_t r = f == NULL ? KERR_BADF : seek(f, (int64_t)(((uint64_t)tf->r[1] << 32) | tf->r[2]), tf->r[4]);
    if (r < 0) {
        return (int32_t)r;
    }
    return mm_copy_out(cur()->mm, tf->r[3], &r, sizeof r);
}

/* mount(source, target, fstype, flags, data).
 *
 * Narrow by design: the only filesystem this kernel can mount is the XFS
 * on its network block device, and it mounts it read-only. Everything else
 * is refused by name rather than half-attempted — Go's syscall.Mount
 * reaches this, so a program gets the ordinary errors it expects. */
/* True when path is prefix or lies below it, with the remainder in rest. */
static bool path_under(const char *path, const char *prefix, const char **rest) {
    uint32_t i = 0;
    while (prefix[i] != '\0' && path[i] == prefix[i]) {
        i++;
    }
    if (prefix[i] != '\0' || (path[i] != '\0' && path[i] != '/')) {
        return false;
    }
    *rest = path + i + (path[i] == '/');
    return true;
}

static bool same_string(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* unlink(path) and unlinkat(dirfd, path, flags).
 *
 * Only /tmp can lose a name: the root is a read-only archive and the
 * network volume is mounted read-only, so everywhere else this is EROFS
 * rather than a silent success. */
static int32_t unlink_path(const char *path) {
    const char *rest;
    if (!path_under(path, "/tmp", &rest)) {
        return KERR_ROFS;
    }
    return tmpfs_unlink(rest);
}

static int32_t sys_unlink(struct trapframe *tf) {
    char path[PATH_CAP + 1];
    int rc = user_path(AT_FDCWD, tf->r[0], path);
    return rc != KERR_OK ? rc : unlink_path(path);
}

static int32_t sys_unlinkat(struct trapframe *tf) {
    char path[PATH_CAP + 1];
    int rc = user_path((int32_t)tf->r[0], tf->r[1], path);
    return rc != KERR_OK ? rc : unlink_path(path);
}

static int32_t sys_mount(struct trapframe *tf) {
    char source[32], target[48], fstype[16];
    if (mm_copy_string_in(cur()->mm, source, tf->r[0], sizeof source) <= 0 ||
        mm_copy_string_in(cur()->mm, target, tf->r[1], sizeof target) <= 0 ||
        mm_copy_string_in(cur()->mm, fstype, tf->r[2], sizeof fstype) <= 0) {
        return KERR_FAULT;
    }
    if (!same_string(fstype, "xfs") || !same_string(source, "/dev/nbd0")) {
        return KERR_NODEV; /* no such filesystem type, or no such device */
    }
    return xfsfs_mount(target, (tf->r[3] & MS_RDONLY) != 0);
}

static int32_t sys_ioctl(struct trapframe *tf) {
    struct file *f = fd_file((int32_t)tf->r[0]);
    if (f == NULL) {
        return KERR_BADF;
    }
    return f->ops->ioctl != NULL ? f->ops->ioctl(f, tf->r[1], tf->r[2]) : KERR_NOTTY;
}

static int32_t sys_fcntl(struct trapframe *tf) {
    int32_t fd = (int32_t)tf->r[0];
    struct file *f = fd_file(fd);
    struct proc *p = cur();
    uint32_t cmd = tf->r[1], arg = tf->r[2];
    if (f == NULL) {
        return KERR_BADF;
    }
    switch (cmd) {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC: {
        int32_t nfd = fd_install(file_get(f), arg, cmd == F_DUPFD_CLOEXEC);
        if (nfd < 0) {
            file_put(f);
        }
        return nfd;
    }
    case F_GETFD: return (p->cloexec >> fd) & 1u ? FD_CLOEXEC : 0;
    case F_SETFD:
        p->cloexec = (arg & FD_CLOEXEC) ? (p->cloexec | (1ull << fd)) : (p->cloexec & ~(1ull << fd));
        return KERR_OK;
    case F_GETFL: return (int32_t)f->flags;
    case F_SETFL: f->flags = (f->flags & O_ACCMODE) | (arg & (O_APPEND | O_NONBLOCK)); return KERR_OK;
    default: return KERR_INVAL;
    }
}

/* struct stat64 for ARM EABI, 104 bytes (asm/stat.h). */
struct stat64 {
    uint64_t dev;
    uint32_t pad0;
    uint32_t ino32;
    uint32_t mode;
    uint32_t nlink;
    uint32_t uid;
    uint32_t gid;
    uint64_t rdev;
    uint32_t pad3[2];
    int64_t size;
    uint32_t blksize;
    uint32_t pad4;
    uint64_t blocks;
    uint32_t atime, atime_ns, mtime, mtime_ns, ctime, ctime_ns;
    uint64_t ino;
};

_Static_assert(sizeof(struct stat64) == 104, "stat64 is 104 bytes on ARM EABI");
_Static_assert(__builtin_offsetof(struct stat64, size) == 48, "st_size at 48");

static int put_stat(const struct file *f, uint32_t ubuf) {
    struct stat64 st = {.dev = 1, .ino32 = f->ino, .mode = f->mode, .nlink = 1, .rdev = f->rdev,
                        .size = (int64_t)f->size, .blksize = 4096, .blocks = (f->size + 511u) / 512u,
                        .ino = f->ino};
    return mm_copy_out(cur()->mm, ubuf, &st, sizeof st);
}

static int32_t sys_fstat64(struct trapframe *tf) {
    struct file *f = fd_file((int32_t)tf->r[0]);
    return f == NULL ? KERR_BADF : put_stat(f, tf->r[1]);
}

/* fstatat64(dirfd, path, buf, flags): opens, stats and closes. */
static int32_t sys_fstatat64(struct trapframe *tf) {
    char path[PATH_CAP + 1];
    struct file *f;
    int alias = 0;
    int rc;
    if ((tf->r[3] & AT_EMPTY_PATH) != 0) {
        f = fd_file((int32_t)tf->r[0]);
        return f == NULL ? KERR_BADF : put_stat(f, tf->r[2]);
    }
    rc = user_path((int32_t)tf->r[0], tf->r[1], path);
    if (rc == KERR_OK) {
        rc = vfs_open(path, O_RDONLY, &f, &alias);
    }
    if (rc == VFS_ALIAS) {
        f = fd_file(alias);
        return f == NULL ? KERR_NOENT : put_stat(f, tf->r[2]);
    }
    if (rc != KERR_OK) {
        return rc;
    }
    rc = put_stat(f, tf->r[2]);
    file_put(f);
    return rc;
}

static int32_t sys_faccessat(struct trapframe *tf) {
    char path[PATH_CAP + 1];
    struct file *f;
    int alias = 0;
    int rc = user_path((int32_t)tf->r[0], tf->r[1], path);
    if (rc == KERR_OK) {
        rc = vfs_open(path, O_RDONLY, &f, &alias);
    }
    if (rc == KERR_OK) {
        file_put(f);
    }
    return rc == VFS_ALIAS ? KERR_OK : rc;
}

static int32_t sys_access(struct trapframe *tf) {
    struct trapframe at = *tf;
    at.r[0] = (uint32_t)AT_FDCWD;
    at.r[1] = tf->r[0];
    return sys_faccessat(&at);
}

static int32_t dup_to(int32_t oldfd, int32_t newfd, bool cloexec) {
    struct file *f = fd_file(oldfd);
    if (f == NULL || newfd < 0 || (uint32_t)newfd >= PROC_MAX_FDS) {
        return KERR_BADF;
    }
    if (oldfd == newfd) {
        return newfd;
    }
    (void)fd_close(newfd);
    return fd_install(file_get(f), (uint32_t)newfd, cloexec);
}

static int32_t sys_dup(struct trapframe *tf) {
    struct file *f = fd_file((int32_t)tf->r[0]);
    int32_t fd;
    if (f == NULL) {
        return KERR_BADF;
    }
    fd = fd_install(file_get(f), 0, false);
    if (fd < 0) {
        file_put(f);
    }
    return fd;
}

static int32_t sys_dup2(struct trapframe *tf) {
    return dup_to((int32_t)tf->r[0], (int32_t)tf->r[1], false);
}

static int32_t sys_dup3(struct trapframe *tf) {
    if (tf->r[0] == tf->r[1]) {
        return KERR_INVAL;
    }
    return dup_to((int32_t)tf->r[0], (int32_t)tf->r[1], (tf->r[2] & O_CLOEXEC) != 0);
}

static int32_t sys_getcwd(struct trapframe *tf) {
    if (tf->r[1] < 2) {
        return KERR_RANGE;
    }
    return mm_copy_out(cur()->mm, tf->r[0], "/", 2) == KERR_OK ? 2 : KERR_FAULT;
}

static bool str_eq(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* readlinkat: /proc/self/exe names the init image; nothing else is a link
 * we resolve (the TAR's symlinks are followed by open, not read). */
static int32_t sys_readlinkat(struct trapframe *tf) {
    char path[PATH_CAP + 1];
    static const char exe[] = "/init";
    uint32_t n = sizeof exe - 1u < tf->r[3] ? sizeof exe - 1u : tf->r[3];
    int rc = user_path((int32_t)tf->r[0], tf->r[1], path);
    if (rc != KERR_OK) {
        return rc;
    }
    if (!str_eq(path, "/proc/self/exe")) {
        return KERR_INVAL;
    }
    return mm_copy_out(cur()->mm, tf->r[2], exe, n) == KERR_OK ? (int32_t)n : KERR_FAULT;
}

/* readlink(path, buf, len): readlinkat from the working directory. */
static int32_t sys_readlink(struct trapframe *tf) {
    struct trapframe at = *tf;
    at.r[0] = (uint32_t)AT_FDCWD;
    at.r[1] = tf->r[0];
    at.r[2] = tf->r[1];
    at.r[3] = tf->r[2];
    return sys_readlinkat(&at);
}

const struct syscall_entry sys_file_table[] = {
    {3, "read", sys_read},
    {4, "write", sys_write},
    {5, "open", sys_open},
    {6, "close", sys_close},
    {19, "lseek", sys_lseek},
    {33, "access", sys_access},
    {41, "dup", sys_dup},
    {54, "ioctl", sys_ioctl},
    {55, "fcntl", sys_fcntl},
    {63, "dup2", sys_dup2},
    {85, "readlink", sys_readlink},
    {140, "_llseek", sys_llseek},
    {145, "readv", sys_readv},
    {146, "writev", sys_writev},
    {10, "unlink", sys_unlink},
    {21, "mount", sys_mount},
    {118, "fsync", sys_fsync},
    {359, "pipe2", sys_pipe2},
    {148, "fdatasync", sys_fsync},
    {180, "pread64", sys_pread64},
    {181, "pwrite64", sys_pwrite64},
    {183, "getcwd", sys_getcwd},
    {197, "fstat64", sys_fstat64},
    {221, "fcntl64", sys_fcntl},
    {322, "openat", sys_openat},
    {327, "fstatat64", sys_fstatat64},
    {332, "readlinkat", sys_readlinkat},
    {334, "faccessat", sys_faccessat},
    {328, "unlinkat", sys_unlinkat},
    {358, "dup3", sys_dup3},
    {0, NULL, NULL},
};
