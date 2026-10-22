/* sys_dir — getdents64: lists a TAR directory (plus "dev" and "proc" in the
 * root, which are mounts, not archive entries). The file position is the
 * archive offset of the next header to look at; root's two mounts come
 * first, at positions DIR_POS_DEV and DIR_POS_PROC above the archive. */
#include "core/errno.h"
#include "core/tar.h"
#include "kernel/proc.h"
#include "kernel/syscall.h"

#include <stddef.h>
#include "kernel/vfs.h"
#include "kernel/tmpfs.h"
#include "kernel/xfsfs.h"

#define DT_DIR 4u
#define DT_REG 8u
#define DT_LNK 10u
#define DIRENT_HEADER 19u        /* d_ino 8, d_off 8, d_reclen 2, d_type 1 */
#define DIR_POS_START 0xfffffff0ull
#define DIR_POS_PROC  0xfffffff1ull
#define DIR_POS_ARCHIVE 0xfffffff2ull

/* struct linux_dirent64's fixed part; the name follows, NUL-terminated,
 * the record padded to 8 bytes. */
struct dirent_head {
    uint64_t ino;
    int64_t off;
    uint16_t reclen;
    uint8_t type;
} __attribute__((packed));

/* The entry's name relative to dir when it is a direct child, else NULL. */
static const char *child_name(const char *dir, const char *name) {
    uint32_t i = 0;
    if (dir[0] != '\0') {
        while (dir[i] != '\0' && name[i] == dir[i]) {
            i++;
        }
        if (dir[i] != '\0' || name[i] != '/') {
            return NULL;
        }
        i++;
    }
    for (uint32_t k = i; name[k] != '\0'; k++) {
        if (name[k] == '/') {
            return NULL;
        }
    }
    return name[i] != '\0' ? name + i : NULL;
}

static uint8_t dtype(char tar_type) {
    return tar_type == TAR_DIR ? DT_DIR : tar_type == TAR_SYMLINK ? DT_LNK : DT_REG;
}

/* Appends one record; returns false when the buffer is full. */
static bool emit(uint8_t *buf, uint32_t cap, uint32_t *used, const char *name, uint8_t type, uint64_t next) {
    uint32_t len = 0, rec;
    struct dirent_head h;
    while (name[len] != '\0') {
        len++;
    }
    rec = (DIRENT_HEADER + len + 1u + 7u) & ~7u;
    if (*used + rec > cap) {
        return false;
    }
    h = (struct dirent_head){.ino = 1, .off = (int64_t)next, .reclen = (uint16_t)rec, .type = type};
    for (uint32_t i = 0; i < rec; i++) {
        buf[*used + i] = 0;
    }
    for (uint32_t i = 0; i < DIRENT_HEADER; i++) {
        buf[*used + i] = ((const uint8_t *)&h)[i];
    }
    for (uint32_t i = 0; i < len; i++) {
        buf[*used + DIRENT_HEADER + i] = (uint8_t)name[i];
    }
    *used += rec;
    return true;
}

/* The root's two mounts, before any archive entry. */
static bool emit_mounts(struct file *f, uint8_t *buf, uint32_t cap, uint32_t *used) {
    if (f->name[0] != '\0') {
        return true;
    }
    if (f->pos == 0) {
        f->pos = DIR_POS_START;
    }
    if (f->pos == DIR_POS_START && emit(buf, cap, used, "dev", DT_DIR, DIR_POS_PROC)) {
        f->pos = DIR_POS_PROC;
    }
    if (f->pos == DIR_POS_PROC && emit(buf, cap, used, "proc", DT_DIR, DIR_POS_ARCHIVE)) {
        f->pos = DIR_POS_ARCHIVE;
    }
    return f->pos == DIR_POS_ARCHIVE || f->pos < DIR_POS_START;
}

/* Fills buf from the archive starting at the file position. */
static void emit_entries(struct file *f, uint8_t *buf, uint32_t cap, uint32_t *used) {
    uint32_t len;
    const uint8_t *ar = vfs_root_archive(&len);
    size_t off = f->pos >= DIR_POS_START ? 0 : (size_t)f->pos;
    struct tar_entry e;
    for (size_t at = off; tar_next(ar, len, &off, &e) == KERR_OK; at = off) {
        const char *name = child_name(f->name, e.name);
        if (name != NULL && !emit(buf, cap, used, name, dtype(e.type), off)) {
            f->pos = at;
            return;
        }
        f->pos = off;
    }
    f->pos = len;
}

/* A mounted filesystem lists its own directories. The position means
 * different things to each one — an archive offset here, an entry count
 * there — which is why the file's owner decides. */
static int32_t list_mounted(struct file *f, uint8_t *buf, uint32_t cap) {
    uint32_t advanced = 0;
    int32_t used = xfsfs_getdents(f, buf, cap, &advanced);
    if (used > 0) {
        f->pos += advanced;
    }
    return used;
}

/* getdents64(fd, dirp, count). */
static int32_t sys_getdents64(struct trapframe *tf) {
    struct file *f = fd_file((int32_t)tf->r[0]);
    uint8_t buf[512];
    uint32_t cap = tf->r[2] < sizeof buf ? tf->r[2] : (uint32_t)sizeof buf;
    uint32_t used = 0;
    int32_t rc;
    if (f == NULL) {
        return KERR_BADF;
    }
    if ((f->mode & S_IFMT) != S_IFDIR) {
        return KERR_NOTDIR;
    }
    if (xfsfs_owns(f)) {
        rc = list_mounted(f, buf, cap);
        if (rc <= 0) {
            return rc;
        }
        used = (uint32_t)rc;
    } else if (tmpfs_owns(f)) {
        uint32_t emitted = 0;
        rc = tmpfs_getdents((uint32_t)f->pos, buf, cap, &emitted);
        if (rc < 0) {
            return rc;
        }
        f->pos += emitted;
        used = (uint32_t)rc;
    } else {
        if (!emit_mounts(f, buf, cap, &used)) {
            return (int32_t)used;  /* the mounts filled the buffer */
        }
        emit_entries(f, buf, cap, &used);
        if (used == 0 && f->pos < DIR_POS_START && cap < 32u) {
            return KERR_INVAL;
        }
    }
    if (used == 0) {
        return 0;
    }
    return mm_copy_out(proc_current()->mm, tf->r[1], buf, used) == KERR_OK ? (int32_t)used : KERR_FAULT;
}

const struct syscall_entry sys_dir_table[] = {
    {217, "getdents64", sys_getdents64},
    {0, NULL, NULL},
};
