#include "kernel/xfsfs.h"

#include <stddef.h>

#include "core/console.h"
#include "core/errno.h"
#include "core/xfs/xfs.h"
#include "kernel/mem.h"
#include "kernel/nbd.h"
#include "kernel/time.h"
#include "kernel/vfs.h"

/* The one mount. A second filesystem would need a mount table, and there
 * is one block device to mount. */
#define XFSFS_PATH_MAX 48u

static struct {
    struct xfs fs;
    char at[XFSFS_PATH_MAX];
    bool up;
    bool read_only;
} mounted;

/* Linux's d_type values, and the directory-entry record a listing emits. */
#define DT_DIR 4u
#define DT_REG 8u
#define DT_LNK 10u
#define DIRENT_HEADER 19u

struct dirent_head {
    uint64_t ino;
    int64_t off;
    uint16_t reclen;
    uint8_t type;
} __attribute__((packed));

bool xfsfs_mounted(void) {
    return mounted.up;
}

const char *xfsfs_prefix(void) {
    return mounted.at;
}

int xfsfs_mount(const char *at, bool read_only) {
    struct bcache *cache = knbd_cache();
    uint32_t i = 0;
    int rc;
    if (at == NULL || at[0] != '/') {
        return KERR_INVAL;
    }
    if (mounted.up) {
        return KERR_EXIST;
    }
    if (cache == NULL) {
        kprintf("xfs: no block device is attached (minikern.nbd=...)\n");
        return KERR_NODEV;
    }
    rc = xfs_mount(&mounted.fs, cache);
    if (rc != KERR_OK) {
        return rc;
    }
    while (at[i] != '\0' && i + 1 < XFSFS_PATH_MAX) {
        mounted.at[i] = at[i];
        i++;
    }
    mounted.at[i] = '\0';
    mounted.read_only = read_only || !xfs_writable(&mounted.fs);
    mounted.up = true;
    kprintf("xfs: mounted at %s, %s\n", mounted.at, mounted.read_only ? "read-only" : "read-write");
    return KERR_OK;
}

/* ── files ─────────────────────────────────────────────────────────────── */

/* A file's inode lives in the file's owned memory, so it is freed with it. */
static struct xfs_inode *inode_of(const struct file *f) {
    return f->owned;
}

static int32_t xfsfs_read(struct file *f, void *buf, uint32_t n) {
    uint32_t got = 0;
    int rc = xfs_file_read(&mounted.fs, inode_of(f), f->pos, buf, n, &got);
    if (rc != KERR_OK) {
        return rc;
    }
    f->pos += got;
    return (int32_t)got;
}

/* Writing file data, which changes no metadata — see xfsfs.h for what is
 * refused and why. */
static int32_t xfsfs_write(struct file *f, const void *buf, uint32_t n) {
    uint32_t put = 0;
    int rc;
    if (mounted.read_only) {
        return KERR_ROFS;
    }
    rc = xfs_file_write(&mounted.fs, inode_of(f), f->pos, buf, n, &put);
    if (rc != KERR_OK && put == 0) {
        return rc;
    }
    f->pos += put;
    /* The open file's size follows the inode's, which the write has just
     * moved: `fstat` reads it from here. */
    f->size = (uint32_t)inode_of(f)->size;
    return (int32_t)put;
}

int xfsfs_flush_all(void) {
    return mounted.up && !mounted.read_only ? xfs_flush(&mounted.fs) : KERR_OK;
}

/* fsync writes the cache back and flushes the device under it. */
static int xfsfs_fsync(struct file *f) {
    (void)f;
    return mounted.up ? xfs_flush(&mounted.fs) : KERR_NODEV;
}

static const struct file_ops xfsfs_ops = {
    .read = xfsfs_read,
    .write = xfsfs_write,
    .fsync = xfsfs_fsync,
};

bool xfsfs_owns(const struct file *f) {
    return f != NULL && f->ops == &xfsfs_ops;
}

/* Builds a file over one inode, with the mode and size a stat must report. */
static int file_for(const struct xfs_inode *ip, uint32_t flags, struct file **out) {
    struct xfs_inode *owned = kmalloc(sizeof *owned);
    struct file *f;
    if (owned == NULL) {
        return KERR_NOMEM;
    }
    *owned = *ip;
    f = file_new_memory(&xfsfs_ops, NULL, (uint32_t)ip->size, ip->mode);
    if (f == NULL) {
        kfree(owned);
        return KERR_NOMEM;
    }
    f->owned = owned;
    f->flags = flags;
    f->ino = (uint32_t)ip->ino;
    *out = f;
    return KERR_OK;
}

/* Splits a path into the directory it names and the last component.
 * Returns the length of the directory part, which is 1 — just the root —
 * when there is only one component. */
static uint32_t split_parent(const char *path, uint32_t len, const char **name, uint32_t *namelen) {
    uint32_t cut = 0;
    for (uint32_t i = 0; i < len; i++) {
        cut = path[i] == '/' ? i : cut;
    }
    *name = path + cut + 1u;
    *namelen = len - cut - 1u;
    return cut == 0 ? 1u : cut;
}

/* Creates the file a failed O_CREAT lookup was looking for.
 *
 * The parent is looked up and the name taken from the path, which is all
 * the filesystem needs: it refuses by itself anything it cannot do — a
 * directory that has outgrown its inode, a chunk with one inode left — and
 * each refusal comes back to the caller as the errno for that reason
 * rather than as "read-only" (core/xfs/xfs.h). */
/* Not inlined on purpose: an inode here carries its whole inline fork, and
 * this function's plus its caller's together overflow an 8 KiB kernel stack
 * (CLAUDE.md §10). A static buffer is not the answer — a lookup sleeps on
 * network I/O, so another open would clobber it. */
__attribute__((noinline)) static int create_then_open(char *path, uint32_t len, uint32_t flags,
                                                      struct file **out) {
    /* One inode structure for both roles — the parent, then the file that
     * was created in it — because an inode here carries its whole inline
     * fork, and three of them at once overflow an 8 KiB kernel stack
     * (CLAUDE.md §10). The parent is not needed once xfs_create returns. */
    struct xfs_inode node;
    const char *name;
    uint32_t namelen, dirlen = split_parent(path, len, &name, &namelen);
    char keep = path[dirlen];
    uint64_t ino = 0;
    int rc;
    path[dirlen] = '\0';
    rc = xfs_path_lookup(&mounted.fs, path, &node);
    path[dirlen] = keep;
    if (rc != KERR_OK) {
        return rc;
    }
    rc = xfs_create(&mounted.fs, &node, name, namelen, XFSFS_CREATE_MODE,
                    time_realtime_ns() / 1000000000ull, &ino);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = xfs_inode_read(&mounted.fs, ino, &node);
    return rc == KERR_OK ? file_for(&node, flags, out) : rc;
}

int xfsfs_open(const char *rest, uint32_t flags, struct file **out) {
    struct xfs_inode ip;
    char path[XFS_NAME_MAX + 2];
    uint32_t i = 0;
    int rc;
    if (!mounted.up) {
        return KERR_NODEV;
    }
    if ((flags & O_ACCMODE) != O_RDONLY && mounted.read_only) {
        return KERR_ROFS;
    }
    if ((flags & O_TRUNC) != 0) {
        return KERR_NOTSUP; /* shortening a file frees blocks: the allocator's */
    }
    path[i++] = '/';
    for (const char *p = rest; *p != '\0' && i + 1 < sizeof path; p++) {
        path[i++] = *p;
    }
    path[i] = '\0';
    rc = xfs_path_lookup(&mounted.fs, path, &ip);
    if (rc == KERR_NOENT && (flags & O_CREAT) != 0 && !mounted.read_only) {
        return create_then_open(path, i, flags, out);
    }
    if (rc == KERR_OK && (flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) {
        return KERR_EXIST;
    }
    return rc == KERR_OK ? file_for(&ip, flags, out) : rc;
}

/* ── listing ───────────────────────────────────────────────────────────── */

/* What a listing carries while it walks: where to start, where to put the
 * records, and how far it got. */
struct listing {
    uint8_t *buf;
    uint32_t cap;
    uint32_t used;
    uint32_t skip;
    uint32_t seen;
    uint32_t emitted;
    bool full;
};

static uint8_t dtype_of(uint8_t type) {
    return type == XFS_DIR3_FT_DIR ? DT_DIR : type == XFS_DIR3_FT_SYMLINK ? DT_LNK : DT_REG;
}

/* Appends one record, or says the buffer is full. */
static bool emit(struct listing *l, const struct xfs_dirent *e) {
    uint32_t rec = (DIRENT_HEADER + e->name_len + 8u) & ~7u;
    struct dirent_head h = {.ino = e->ino, .reclen = (uint16_t)rec, .type = dtype_of(e->type)};
    uint8_t *at = l->buf + l->used;
    if (l->used + rec > l->cap) {
        return false;
    }
    h.off = (int64_t)(l->skip + l->emitted + 1u);
    for (uint32_t i = 0; i < sizeof h; i++) {
        at[i] = ((const uint8_t *)&h)[i];
    }
    for (uint32_t i = 0; i < e->name_len; i++) {
        at[sizeof h + i] = (uint8_t)e->name[i];
    }
    for (uint32_t i = sizeof h + e->name_len; i < rec; i++) {
        at[i] = 0;
    }
    l->used += rec;
    l->emitted++;
    return true;
}

/* The callback the reader drives: skips what a previous call returned, and
 * stops when the buffer is full. */
static int visit(void *self, const struct xfs_dirent *e) {
    struct listing *l = self;
    if (l->seen++ < l->skip) {
        return KERR_OK;
    }
    if (!emit(l, e)) {
        l->full = true;
        return KERR_EXIST; /* stop the walk: there is no room for more */
    }
    return KERR_OK;
}

int32_t xfsfs_getdents(struct file *f, uint8_t *buf, uint32_t cap, uint32_t *advanced) {
    struct listing l = {.buf = buf, .cap = cap, .skip = (uint32_t)f->pos};
    int rc;
    *advanced = 0;
    if (!mounted.up) {
        return KERR_NODEV;
    }
    rc = xfs_dir_list(&mounted.fs, inode_of(f), visit, &l);
    if (rc != KERR_OK && rc != KERR_EXIST) {
        return rc;
    }
    if (l.full && l.emitted == 0) {
        return KERR_INVAL; /* not even one entry fits */
    }
    *advanced = l.emitted;
    return (int32_t)l.used;
}
