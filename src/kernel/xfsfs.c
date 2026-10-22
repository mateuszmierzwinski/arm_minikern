#include "kernel/xfsfs.h"

#include <stddef.h>

#include "core/console.h"
#include "core/errno.h"
#include "core/xfs/xfs.h"
#include "kernel/mem.h"
#include "kernel/nbd.h"
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
    if ((flags & (O_CREAT | O_TRUNC)) != 0) {
        return KERR_ROFS; /* both would change metadata */
    }
    path[i++] = '/';
    for (const char *p = rest; *p != '\0' && i + 1 < sizeof path; p++) {
        path[i++] = *p;
    }
    path[i] = '\0';
    rc = xfs_path_lookup(&mounted.fs, path, &ip);
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
