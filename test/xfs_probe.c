/* xfs_probe — the kernel's XFS reader (core/xfs), on the build machine,
 * against an image file.
 *
 * It exists so the reader can be pointed at filesystems made by Linux's own
 * mkfs.xfs and checked against the files that went into them. The reader is
 * the same source the kernel links; only the block device differs, which is
 * the point of that being a port.
 *
 *   xfs_probe <image> info
 *   xfs_probe <image> ls <path>
 *   xfs_probe <image> cat <path> <outfile>
 *   xfs_probe <image> stat <path>
 *   xfs_probe <image> write <path> <offset> <file>   (opens the image rw)
 *
 * Exit status is 0 for success, 1 for a refusal or a failure — which is
 * what the negative cases in xfs_live_test.sh check.
 */
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/bcache.h"
#include "core/console.h"
#include "core/errno.h"
#include "core/xfs/xfs.h"

#define BLOCK  4096u
#define SLOTS  64u

/* The reader explains its refusals with kprintf, which goes to a console
 * port — so the port here is stderr. Without it the kernel's own reasons
 * ("the log is dirty", "unsupported features") would be invisible, and a
 * test could only see the error number. */
static void stderr_write(void *self, const char *buf, size_t len) {
    (void)self;
    fwrite(buf, 1, len, stderr);
}

static int fd;
static uint8_t cache_mem[SLOTS * BLOCK];
static struct bcache cache;
static struct blockdev dev;
static struct xfs fs;

static int file_read(void *self, uint64_t lba, uint32_t count, void *buf) {
    (void)self;
    if (!blockdev_in_range(&dev, lba, count)) {
        return KERR_INVAL;
    }
    return pread(fd, buf, count * BLOCK, (off_t)(lba * BLOCK)) == (ssize_t)(count * BLOCK) ? KERR_OK
                                                                                          : KERR_IO;
}

static int file_write(void *self, uint64_t lba, uint32_t count, const void *buf) {
    (void)self;
    if (!blockdev_in_range(&dev, lba, count)) {
        return KERR_INVAL;
    }
    return pwrite(fd, buf, count * BLOCK, (off_t)(lba * BLOCK)) == (ssize_t)(count * BLOCK) ? KERR_OK
                                                                                            : KERR_IO;
}

static int file_flush(void *self) {
    (void)self;
    return fsync(fd) == 0 ? KERR_OK : KERR_IO;
}

/* Opens the image and mounts it, read-write only when asked: a test that
 * means to read must not be able to change anything. */
static int mount_image(const char *path, bool writable) {
    struct stat st;
    int rc;
    fd = open(path, writable ? O_RDWR : O_RDONLY);
    if (fd < 0 || fstat(fd, &st) != 0) {
        fprintf(stderr, "xfs_probe: cannot open %s\n", path);
        return KERR_IO;
    }
    dev = (struct blockdev){.read = file_read,
                            .write = file_write,
                            .flush = file_flush,
                            .block_size = BLOCK,
                            .blocks = (uint64_t)st.st_size / BLOCK,
                            .read_only = !writable};
    rc = bcache_init(&cache, &dev, cache_mem, SLOTS);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = xfs_mount(&fs, &cache);
    if (rc != KERR_OK) {
        fprintf(stderr, "xfs_probe: mount refused: %d\n", rc);
    }
    return rc;
}

static const char *type_name(uint8_t type) {
    switch (type) {
    case XFS_DIR3_FT_REG: return "file";
    case XFS_DIR3_FT_DIR: return "dir";
    case XFS_DIR3_FT_SYMLINK: return "link";
    default: return "?";
    }
}

/* One line per entry: the listing a test compares against the protofile. */
static int print_entry(void *self, const struct xfs_dirent *e) {
    (void)self;
    printf("%s %llu %.*s\n", type_name(e->type), (unsigned long long)e->ino, (int)e->name_len, e->name);
    return KERR_OK;
}

static int do_ls(const char *path) {
    struct xfs_inode ip;
    int rc = xfs_path_lookup(&fs, path, &ip);
    if (rc != KERR_OK) {
        fprintf(stderr, "xfs_probe: %s: %d\n", path, rc);
        return rc;
    }
    return xfs_dir_list(&fs, &ip, print_entry, NULL);
}

static int do_cat(const char *path, const char *out_path) {
    struct xfs_inode ip;
    FILE *out;
    uint64_t off = 0;
    int rc = xfs_path_lookup(&fs, path, &ip);
    if (rc != KERR_OK) {
        fprintf(stderr, "xfs_probe: %s: %d\n", path, rc);
        return rc;
    }
    out = fopen(out_path, "wb");
    if (out == NULL) {
        return KERR_IO;
    }
    while (off < ip.size && rc == KERR_OK) {
        static uint8_t buf[65536];
        uint32_t got = 0;
        rc = xfs_file_read(&fs, &ip, off, buf, sizeof buf, &got);
        if (rc != KERR_OK || got == 0) {
            break;
        }
        rc = fwrite(buf, 1, got, out) == got ? rc : KERR_IO;
        off += got;
    }
    fclose(out);
    return rc;
}

/* write <path> <offset> <file>: puts a file's bytes into the filesystem,
 * which must already have the blocks for them. */
static int do_write(const char *path, const char *offset, const char *source) {
    struct xfs_inode ip;
    uint64_t off = strtoull(offset, NULL, 0);
    uint8_t *buf;
    FILE *in = fopen(source, "rb");
    size_t len;
    uint32_t put = 0;
    int rc = xfs_path_lookup(&fs, path, &ip);
    if (rc != KERR_OK || in == NULL) {
        fprintf(stderr, "xfs_probe: %s: %d\n", path, rc);
        return rc == KERR_OK ? KERR_NOENT : rc;
    }
    buf = malloc(1u << 20);
    if (buf == NULL) {
        return KERR_NOMEM;
    }
    len = fread(buf, 1, 1u << 20, in);
    fclose(in);
    rc = xfs_file_write(&fs, &ip, off, buf, (uint32_t)len, &put);
    free(buf);
    if (rc != KERR_OK) {
        fprintf(stderr, "xfs_probe: writing %zu bytes at %s: %d\n", len, offset, rc);
        return rc;
    }
    printf("wrote %u bytes at %llu\n", put, (unsigned long long)off);
    return xfs_flush(&fs);
}

static int do_stat(const char *path) {
    struct xfs_inode ip;
    int rc = xfs_path_lookup(&fs, path, &ip);
    if (rc != KERR_OK) {
        fprintf(stderr, "xfs_probe: %s: %d\n", path, rc);
        return rc;
    }
    printf("ino %llu mode %06o size %llu nlink %u format %u nextents %u\n", (unsigned long long)ip.ino,
           ip.mode, (unsigned long long)ip.size, ip.nlink, ip.format, ip.nextents);
    return KERR_OK;
}

int main(int argc, char **argv) {
    struct console_port console = {.write = stderr_write};
    int rc;
    console_attach(&console);
    if (argc < 3) {
        fprintf(stderr, "usage: %s <image> info|ls <path>|cat <path> <out>|stat <path>\n", argv[0]);
        return 2;
    }
    rc = mount_image(argv[1], strcmp(argv[2], "write") == 0);
    if (rc != KERR_OK) {
        return 1;
    }
    if (strcmp(argv[2], "info") == 0) {
        printf("blocks %llu blocksize %u agcount %u inodesize %u root %llu ftype %d\n",
               (unsigned long long)fs.dblocks, fs.blocksize, fs.agcount, fs.inodesize,
               (unsigned long long)fs.rootino, fs.ftype ? 1 : 0);
        rc = KERR_OK;
    } else if (strcmp(argv[2], "ls") == 0 && argc >= 4) {
        rc = do_ls(argv[3]);
    } else if (strcmp(argv[2], "cat") == 0 && argc >= 5) {
        rc = do_cat(argv[3], argv[4]);
    } else if (strcmp(argv[2], "write") == 0 && argc >= 5) {
        rc = do_write(argv[3], argv[4], argv[5]);
    } else if (strcmp(argv[2], "stat") == 0 && argc >= 4) {
        rc = do_stat(argv[3]);
    } else {
        fprintf(stderr, "xfs_probe: bad operation\n");
        rc = KERR_INVAL;
    }
    close(fd);
    return rc == KERR_OK ? 0 : 1;
}
