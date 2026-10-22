#include "kernel/tarfs.h"

#include "core/errno.h"
#include "core/tar.h"
#include "kernel/vfs.h"

static const uint8_t *root;
static uint32_t root_len;

static const struct file_ops tar_file_ops = {.read = file_memory_read};
static const struct file_ops tar_dir_ops = {0};

void tarfs_mount(const uint8_t *archive, uint32_t len) {
    root = archive;
    root_len = len;
}

void vfs_mount_root(const uint8_t *archive, uint32_t len) {
    tarfs_mount(archive, len);
}

const uint8_t *tarfs_archive(uint32_t *len) {
    *len = root_len;
    return root;
}

const uint8_t *vfs_root_archive(uint32_t *len) {
    return tarfs_archive(len);
}

/* The root directory exists even though no archive entry names it. */
static int is_root(const char *path) {
    char norm[TAR_NAME_MAX];
    return tar_normalise(path, norm) == KERR_OK && norm[0] == '\0';
}

static struct file *open_dir(const char *name) {
    struct file *f = file_new_memory(&tar_dir_ops, NULL, 0, S_IFDIR | 0755u);
    uint32_t i = 0;
    for (; f != NULL && name[i] != '\0' && i + 1 < sizeof f->name; i++) {
        f->name[i] = name[i];
    }
    if (f != NULL) {
        f->name[i] = '\0';
    }
    return f;
}

/* Where a relative symlink target points: the link's own directory, then
 * the target, normalised — so `../lib/x` from `/usr/bin/tool` becomes
 * `usr/lib/x`. Normalising is what resolves the `..`, and it is also what
 * keeps the result inside the root (core/tar.h). */
static int resolve_relative(const char *path, const char *target, char *out) {
    char joined[TAR_NAME_MAX];
    uint32_t at = 0, cut = 0;
    for (uint32_t i = 0; path[i] != '\0'; i++) {
        cut = path[i] == '/' ? i + 1u : cut;
    }
    for (; at < cut && at + 1u < sizeof joined; at++) {
        joined[at] = path[at];
    }
    for (uint32_t i = 0; target[i] != '\0'; i++) {
        if (at + 1u >= sizeof joined) {
            return KERR_NAMETOOLONG;
        }
        joined[at++] = target[i];
    }
    joined[at] = '\0';
    return tar_normalise(joined, out);
}

/* Resolves path to an entry, following symlinks.
 *
 * An absolute target is taken from the root and a relative one from the
 * link's own directory, which is what every other system does and what a
 * relative target is for. More than a few links deep is a loop — a link to
 * itself is the easy case, and two links to each other the next — so the
 * depth is bounded rather than trusted. */
#define SYMLINK_MAX_DEPTH 8u

static int lookup(const char *path, struct tar_entry *e) {
    char resolved[TAR_NAME_MAX];
    int rc = tar_lookup(root, root_len, path, e);
    for (uint32_t depth = 0; rc == KERR_OK && e->type == TAR_SYMLINK; depth++) {
        if (depth >= SYMLINK_MAX_DEPTH) {
            return KERR_LOOP;
        }
        if (e->link[0] == '/') {
            rc = tar_normalise(e->link, resolved);
        } else {
            rc = resolve_relative(path, e->link, resolved);
        }
        if (rc != KERR_OK) {
            return rc;
        }
        path = resolved;
        rc = tar_lookup(root, root_len, resolved, e);
    }
    return rc;
}

int tarfs_open(const char *path, uint32_t flags, struct file **out) {
    struct tar_entry e;
    int rc;
    if ((flags & O_ACCMODE) != O_RDONLY || (flags & (O_CREAT | O_TRUNC)) != 0) {
        return KERR_ACCES;
    }
    if (is_root(path)) {
        *out = open_dir("");
        return *out == NULL ? KERR_NOMEM : KERR_OK;
    }
    rc = lookup(path, &e);
    if (rc != KERR_OK) {
        return rc == KERR_INVAL ? KERR_IO : rc;
    }
    if ((flags & O_DIRECTORY) && e.type != TAR_DIR) {
        return KERR_NOTDIR;
    }
    *out = e.type == TAR_DIR ? open_dir(e.name)
                             : file_new_memory(&tar_file_ops, e.data, e.size, S_IFREG | (e.mode & 07777u));
    return *out == NULL ? KERR_NOMEM : KERR_OK;
}
