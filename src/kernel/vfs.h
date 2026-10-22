/* vfs — open files and the namespace.
 *
 * The namespace is fixed: "/dev" is devfs (consoles, null, zero, random),
 * "/proc" is procfs (generated, the little Go runtimes read), and
 * everything else is the read-only TAR root (core/tar.h). A struct file is
 * an open instance with an ops table, shared by reference between file
 * descriptors (dup) and threads. File ops work on kernel buffers; the
 * syscall layer copies to and from user space. */
#ifndef MINIKERN_KERNEL_VFS_H
#define MINIKERN_KERNEL_VFS_H

#include <stdint.h>

/* Open flags and file-type bits: Linux's values (ARM EABI). */
#define O_ACCMODE   0003u
#define O_RDONLY    0u
#define O_WRONLY    1u
#define O_RDWR      2u
#define O_CREAT     0100u
#define O_TRUNC     01000u
#define O_APPEND    02000u
#define O_NONBLOCK  04000u
#define O_DIRECTORY 040000u  /* ARM's value, not x86's */
#define O_CLOEXEC   02000000u

#define S_IFMT   0170000u
#define S_IFDIR  0040000u
#define S_IFCHR  0020000u
#define S_IFBLK  0060000u
#define S_IFREG  0100000u
#define S_IFLNK  0120000u

struct file;

struct file_ops {
    int32_t (*read)(struct file *f, void *buf, uint32_t n);   /* at f->pos, advances it */
    int32_t (*write)(struct file *f, const void *buf, uint32_t n);
    int (*ioctl)(struct file *f, uint32_t cmd, uint32_t arg);
    int (*fsync)(struct file *f);                             /* NULL: nothing to make durable */
    void (*release)(struct file *f);
    uint32_t (*poll)(struct file *f);  /* EPOLLIN/EPOLLOUT now; NULL: not pollable */
};

/* epoll's readiness bits (Linux values). */
#define EPOLLIN  0x001u
#define EPOLLOUT 0x004u
#define EPOLLERR 0x008u
#define EPOLLHUP 0x010u

struct file {
    const struct file_ops *ops;
    uint32_t refs;
    uint32_t flags;        /* O_* status flags */
    uint64_t pos;
    uint32_t mode;         /* S_IF* | permissions */
    uint32_t ino;
    uint32_t rdev;
    const uint8_t *data;   /* memory-backed files: the bytes */
    uint32_t size;
    void *owned;           /* freed on release (generated contents) */
    void *priv;            /* the filesystem's per-open state */
    uint32_t poll_gen;     /* bumped on every readiness change (edge-triggered epoll) */
    char name[64];         /* directories: the normalised path, for listing */
};

/* What a path resolved to when it is an alias for an open descriptor
 * (/dev/stdout is fd 1): vfs_open returns VFS_ALIAS and sets *alias_fd. */
#define VFS_ALIAS 1

/* Makes the TAR archive at archive (len bytes, kernel address) the root. */
void vfs_mount_root(const uint8_t *archive, uint32_t len);

/* Opens path (absolute, already copied in). KERR_OK with *out, VFS_ALIAS,
 * or a negative errno. */
int vfs_open(const char *path, uint32_t flags, struct file **out, int *alias_fd);

/* A new memory-backed file over data (not copied), for the filesystems. */
struct file *file_new_memory(const struct file_ops *ops, const uint8_t *data, uint32_t size, uint32_t mode);

/* Reads memory-backed files: the shared read op. */
int32_t file_memory_read(struct file *f, void *buf, uint32_t n);

struct file *file_get(struct file *f);
void file_put(struct file *f);

/* A pollable file's readiness changed: bumps its generation and wakes
 * every epoll waiter to look again. */
void vfs_poll_notify(struct file *f);

/* The channel epoll waiters sleep on. */
const void *vfs_poll_channel(void);

/* The root archive, for directory listing (getdents64). */
const uint8_t *vfs_root_archive(uint32_t *len);

#endif
