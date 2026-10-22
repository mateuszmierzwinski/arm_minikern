#include "kernel/vfs.h"

#include "core/errno.h"
#include "kernel/devfs.h"
#include "kernel/mem.h"
#include "kernel/procfs.h"
#include "kernel/sched.h"
#include "kernel/tarfs.h"

static uint32_t next_ino = 1;
static int poll_channel;

void vfs_poll_notify(struct file *f) {
    if (f != NULL) {
        f->poll_gen++;
    }
    sched_wake(&poll_channel, 0, SCHED_MAX_TASKS);
}

const void *vfs_poll_channel(void) {
    return &poll_channel;
}

/* True when path is prefix itself or lies below it ("/dev", "/dev/x"). */
static int under(const char *path, const char *prefix, const char **rest) {
    uint32_t i = 0;
    while (prefix[i] != '\0' && path[i] == prefix[i]) {
        i++;
    }
    if (prefix[i] != '\0' || (path[i] != '\0' && path[i] != '/')) {
        return 0;
    }
    *rest = path + i + (path[i] == '/');
    return 1;
}

int vfs_open(const char *path, uint32_t flags, struct file **out, int *alias_fd) {
    const char *rest;
    if (path[0] != '/') {
        return KERR_NOENT;
    }
    if (under(path, "/dev", &rest)) {
        return devfs_open(rest, flags, out, alias_fd);
    }
    if (under(path, "/proc", &rest)) {
        return procfs_open(rest, flags, out);
    }
    return tarfs_open(path, flags, out);
}

struct file *file_new_memory(const struct file_ops *ops, const uint8_t *data, uint32_t size, uint32_t mode) {
    struct file *f = kzalloc(sizeof *f);
    if (f != NULL) {
        *f = (struct file){.ops = ops, .refs = 1, .data = data, .size = size, .mode = mode, .ino = next_ino++};
    }
    return f;
}

int32_t file_memory_read(struct file *f, void *buf, uint32_t n) {
    uint8_t *d = buf;
    uint32_t left = f->pos >= f->size ? 0 : f->size - (uint32_t)f->pos;
    uint32_t c = n < left ? n : left;
    for (uint32_t i = 0; i < c; i++) {
        d[i] = f->data[f->pos + i];
    }
    f->pos += c;
    return (int32_t)c;
}

struct file *file_get(struct file *f) {
    f->refs++;
    return f;
}

void file_put(struct file *f) {
    if (f == NULL || --f->refs > 0) {
        return;
    }
    if (f->ops->release != NULL) {
        f->ops->release(f);
    }
    if (f->owned != NULL) {
        kfree(f->owned);
    }
    kfree(f);
}
