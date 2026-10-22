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

/* Resolves path to an entry, following one symlink level (absolute targets
 * are taken from the root, relative ones from the link's directory: only
 * absolute targets are supported for now). */
static int lookup(const char *path, struct tar_entry *e) {
    int rc = tar_lookup(root, root_len, path, e);
    if (rc == KERR_OK && e->type == TAR_SYMLINK) {
        if (e->link[0] != '/') {
            return KERR_NOENT;
        }
        rc = tar_lookup(root, root_len, e->link, e);
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
