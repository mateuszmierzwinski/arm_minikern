#include "kernel/tmpfs.h"

#include <stddef.h>

#include "core/errno.h"
#include "kernel/mem.h"
#include "kernel/vfs.h"

/* One file: a name, a growable buffer, and how many opens are holding it.
 * A file that is unlinked while open keeps its bytes until the last opener
 * lets go, which is what every program expects of a temporary file. */
struct node {
    char name[32];
    uint8_t *data;
    uint32_t len;
    uint32_t cap;
    uint32_t opens;
    bool used;
    bool unlinked;
};

static struct node nodes[TMPFS_MAX_FILES];

/* The record a directory listing emits (Linux's struct linux_dirent64). */
#define DT_DIR 4u
#define DT_REG 8u
#define DIRENT_HEADER 19u

struct dirent_head {
    uint64_t ino;
    int64_t off;
    uint16_t reclen;
    uint8_t type;
} __attribute__((packed));

static bool name_equal(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* A name must be a single component: there are no subdirectories here, and
 * a name with a separator in it would silently become one. */
static bool name_ok(const char *name) {
    uint32_t i = 0;
    for (; name[i] != '\0'; i++) {
        if (name[i] == '/' || i + 1 >= sizeof nodes[0].name) {
            return false;
        }
    }
    return i > 0;
}

static struct node *find(const char *name) {
    for (uint32_t i = 0; i < TMPFS_MAX_FILES; i++) {
        if (nodes[i].used && !nodes[i].unlinked && name_equal(nodes[i].name, name)) {
            return &nodes[i];
        }
    }
    return NULL;
}

/* Lets go of a node, freeing it when nothing holds it and it is gone from
 * the directory. */
static void release_node(struct node *n) {
    if (n->opens > 0) {
        n->opens--;
    }
    if (n->opens == 0 && n->unlinked) {
        kfree(n->data);
        *n = (struct node){0};
    }
}

static struct node *create(const char *name) {
    for (uint32_t i = 0; i < TMPFS_MAX_FILES; i++) {
        if (!nodes[i].used) {
            struct node *n = &nodes[i];
            *n = (struct node){.used = true};
            for (uint32_t k = 0; name[k] != '\0'; k++) {
                n->name[k] = name[k];
            }
            return n;
        }
    }
    return NULL;
}

/* Grows a file to hold at least want bytes, doubling so that writing a
 * byte at a time is not quadratic. */
static int grow(struct node *n, uint32_t want) {
    uint32_t cap = n->cap == 0 ? 4096u : n->cap;
    uint8_t *bigger;
    if (want <= n->cap) {
        return KERR_OK;
    }
    if (want > TMPFS_MAX_BYTES) {
        return KERR_FBIG;
    }
    while (cap < want) {
        cap *= 2u;
    }
    cap = cap > TMPFS_MAX_BYTES ? TMPFS_MAX_BYTES : cap;
    bigger = kmalloc(cap);
    if (bigger == NULL) {
        return KERR_NOSPC;
    }
    for (uint32_t i = 0; i < n->len; i++) {
        bigger[i] = n->data[i];
    }
    for (uint32_t i = n->len; i < cap; i++) {
        bigger[i] = 0;
    }
    kfree(n->data);
    n->data = bigger;
    n->cap = cap;
    return KERR_OK;
}

static struct node *node_of(const struct file *f) {
    return f->priv;
}

static int32_t tmpfs_read(struct file *f, void *buf, uint32_t n) {
    struct node *node = node_of(f);
    uint8_t *out = buf;
    uint32_t at = (uint32_t)f->pos;
    uint32_t have = at < node->len ? node->len - at : 0;
    uint32_t take = have < n ? have : n;
    for (uint32_t i = 0; i < take; i++) {
        out[i] = node->data[at + i];
    }
    f->pos += take;
    return (int32_t)take;
}

static int32_t tmpfs_write(struct file *f, const void *buf, uint32_t n) {
    struct node *node = node_of(f);
    const uint8_t *in = buf;
    uint32_t at = (f->flags & O_APPEND) != 0 ? node->len : (uint32_t)f->pos;
    int rc;
    if (n == 0) {
        return 0;
    }
    if (at > TMPFS_MAX_BYTES - 1u || n > TMPFS_MAX_BYTES - at) {
        return KERR_FBIG;
    }
    if ((rc = grow(node, at + n)) != KERR_OK) {
        return rc;
    }
    for (uint32_t i = 0; i < n; i++) {
        node->data[at + i] = in[i];
    }
    node->len = at + n > node->len ? at + n : node->len;
    f->pos = at + n;
    f->size = node->len;
    return (int32_t)n;
}

static void tmpfs_release(struct file *f) {
    release_node(node_of(f));
}

static const struct file_ops tmpfs_ops = {
    .read = tmpfs_read,
    .write = tmpfs_write,
    .release = tmpfs_release,
};

/* The mount point itself: a directory with nothing behind it but the
 * listing (tmpfs_getdents). */
static const struct file_ops tmpfs_dir_ops = {0};

bool tmpfs_owns(const struct file *f) {
    return f != NULL && (f->ops == &tmpfs_ops || f->ops == &tmpfs_dir_ops);
}

/* Builds the file handle over a node, taking a hold on it. */
static int file_for(struct node *n, uint32_t flags, struct file **out) {
    struct file *f = file_new_memory(&tmpfs_ops, NULL, n->len, S_IFREG | 0644u);
    if (f == NULL) {
        return KERR_NOMEM;
    }
    f->priv = n;
    f->flags = flags;
    n->opens++;
    *out = f;
    return KERR_OK;
}

int tmpfs_open(const char *rest, uint32_t flags, struct file **out) {
    struct node *n;
    if (rest[0] == '\0') {
        *out = file_new_memory(&tmpfs_dir_ops, NULL, 0, S_IFDIR | 0777u);
        return *out == NULL ? KERR_NOMEM : KERR_OK;
    }
    if (!name_ok(rest)) {
        return KERR_INVAL;
    }
    n = find(rest);
    if (n == NULL) {
        if ((flags & O_CREAT) == 0) {
            return KERR_NOENT;
        }
        n = create(rest);
        if (n == NULL) {
            return KERR_NOSPC;
        }
    } else if ((flags & O_TRUNC) != 0) {
        n->len = 0;
    }
    return file_for(n, flags, out);
}

int tmpfs_unlink(const char *rest) {
    struct node *n;
    if (rest[0] == '\0') {
        return KERR_ISDIR;
    }
    n = find(rest);
    if (n == NULL) {
        return KERR_NOENT;
    }
    n->unlinked = true;
    if (n->opens == 0) {
        kfree(n->data);
        *n = (struct node){0};
    }
    return KERR_OK;
}

/* Appends one record; false when the buffer is full. */
static bool emit(uint8_t *buf, uint32_t cap, uint32_t *used, const char *name, uint8_t type,
                 uint64_t next) {
    uint32_t len = 0;
    struct dirent_head h;
    while (name[len] != '\0') {
        len++;
    }
    uint32_t rec = (DIRENT_HEADER + len + 8u) & ~7u;
    if (*used + rec > cap) {
        return false;
    }
    h = (struct dirent_head){.ino = next, .off = (int64_t)next, .reclen = (uint16_t)rec, .type = type};
    for (uint32_t i = 0; i < sizeof h; i++) {
        buf[*used + i] = ((const uint8_t *)&h)[i];
    }
    for (uint32_t i = 0; i < len; i++) {
        buf[*used + sizeof h + i] = (uint8_t)name[i];
    }
    for (uint32_t i = sizeof h + len; i < rec; i++) {
        buf[*used + i] = 0;
    }
    *used += rec;
    return true;
}

int32_t tmpfs_getdents(uint32_t skip, uint8_t *buf, uint32_t cap, uint32_t *emitted) {
    uint32_t used = 0, seen = 0;
    *emitted = 0;
    for (uint32_t i = 0; i < TMPFS_MAX_FILES; i++) {
        if (!nodes[i].used || nodes[i].unlinked) {
            continue;
        }
        if (seen++ < skip) {
            continue;
        }
        if (!emit(buf, cap, &used, nodes[i].name, DT_REG, skip + *emitted + 1u)) {
            break;
        }
        (*emitted)++;
    }
    return (int32_t)used;
}
