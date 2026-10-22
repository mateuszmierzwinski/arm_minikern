#include <stddef.h>

#include "core/errno.h"
#include "lib/crc32c.h"
#include "core/xfs/xfs_internal.h"

/* Reading a file's bytes.
 *
 * File data is not metadata: its blocks carry no magic, no owner and no
 * checksum, so they are read through the cache without verification. What
 * is verified is the path that led here — the inode and every directory
 * block — and the extent list's arithmetic, which is checked against the
 * filesystem's size before any block number is used. */

static void zero_fill(uint8_t *at, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        at[i] = 0;
    }
}

static void copy_bytes(uint8_t *dst, const uint8_t *src, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        dst[i] = src[i];
    }
}

/* A file held inside its own inode: a symbolic link's target, or a
 * directory's shortform content. */
static int read_local(const struct xfs_inode *ip, uint64_t off, uint8_t *buf, uint32_t len,
                      uint32_t *got) {
    uint32_t have = ip->size < ip->fork_len ? (uint32_t)ip->size : ip->fork_len;
    if (off >= have) {
        return KERR_OK;
    }
    *got = have - (uint32_t)off < len ? have - (uint32_t)off : len;
    copy_bytes(buf, ip->fork + off, *got);
    return KERR_OK;
}

/* One block's worth: from the file's data, from a hole, or from an extent
 * that was allocated but never written — the last two read as zeroes,
 * which is what they are. */
static int read_chunk(struct xfs *fs, const struct xfs_inode *ip, uint64_t at, uint8_t *out,
                      uint32_t within, uint32_t chunk) {
    struct xfs_extent e;
    struct bbuf *b;
    int rc = xfs_bmap(fs, ip, at >> fs->blocklog, &e);
    if (rc == KERR_NOENT || (rc == KERR_OK && e.unwritten)) {
        zero_fill(out, chunk);
        return KERR_OK;
    }
    if (rc != KERR_OK) {
        return rc;
    }
    rc = bcache_get(fs->cache, xfs_fsb_to_block(fs, e.startblock + ((at >> fs->blocklog) - e.startoff)), &b);
    if (rc != KERR_OK) {
        return rc;
    }
    copy_bytes(out, b->data + within, chunk);
    bcache_put(fs->cache, b);
    return KERR_OK;
}

int xfs_file_read(struct xfs *fs, const struct xfs_inode *ip, uint64_t off, void *buf, uint32_t len,
                  uint32_t *got) {
    uint8_t *out = buf;
    uint32_t done = 0;
    *got = 0;
    /* A directory's bytes are structure rather than content: handing them
     * back as a file's contents is how a reader turns a directory into
     * rubbish for whoever asked. */
    if (xfs_is_dir(ip)) {
        return KERR_ISDIR;
    }
    if (!xfs_is_reg(ip) && !xfs_is_link(ip)) {
        return KERR_INVAL;
    }
    if (ip->format == XFS_DINODE_FMT_LOCAL) {
        return read_local(ip, off, out, len, got);
    }
    if (ip->format != XFS_DINODE_FMT_EXTENTS) {
        return KERR_NOTSUP;
    }
    if (off >= ip->size) {
        return KERR_OK; /* at or past the end of the file */
    }
    len = ip->size - off < len ? (uint32_t)(ip->size - off) : len;
    while (done < len) {
        uint64_t at = off + done;
        uint32_t within = (uint32_t)(at & (fs->blocksize - 1u));
        uint32_t chunk = fs->blocksize - within < len - done ? fs->blocksize - within : len - done;
        int rc = read_chunk(fs, ip, at, out + done, within, chunk);
        if (rc != KERR_OK) {
            return rc;
        }
        done += chunk;
    }
    *got = done;
    return KERR_OK;
}

/* ── writing ───────────────────────────────────────────────────────────── */

bool xfs_writable(const struct xfs *fs) {
    return fs->mounted && fs->cache != NULL && !fs->cache->dev->read_only;
}

int xfs_flush(struct xfs *fs) {
    return fs->mounted ? bcache_flush(fs->cache) : KERR_INVAL;
}

/* The one kind of write that changes no metadata: into a block the file
 * already has, and has already been written to. Anything else — a hole, an
 * extent allocated but never written, a block past the end of the file —
 * would mean changing the inode, the free-space trees or the extent list,
 * and all of that has to go through the log first (P9.4). */
static void put_be64(uint8_t *p, uint64_t v) {
    for (uint32_t i = 0; i < 8; i++) {
        p[i] = (uint8_t)(v >> (56u - 8u * i));
    }
}

static void put_le32(uint8_t *p, uint32_t v) {
    for (uint32_t i = 0; i < 4; i++) {
        p[i] = (uint8_t)(v >> (8u * i));
    }
}

static int write_chunk(struct xfs *fs, const struct xfs_inode *ip, uint64_t at, const uint8_t *in,
                       uint32_t within, uint32_t chunk) {
    struct xfs_extent e;
    struct bbuf *b;
    int rc = xfs_bmap(fs, ip, at >> fs->blocklog, &e);
    if (rc == KERR_NOENT || (rc == KERR_OK && e.unwritten)) {
        return KERR_NOTSUP; /* a hole or an unwritten extent: metadata */
    }
    if (rc != KERR_OK) {
        return rc;
    }
    rc = bcache_get(fs->cache, xfs_fsb_to_block(fs, e.startblock + ((at >> fs->blocklog) - e.startoff)),
                    &b);
    if (rc != KERR_OK) {
        return rc;
    }
    copy_bytes(b->data + within, in, chunk);
    bcache_dirty(b);
    bcache_put(fs->cache, b);
    return KERR_OK;
}

/* How many bytes the file's existing blocks could hold, which is where a
 * write may reach without anything being allocated. A file's last block is
 * usually only partly used — a 10-byte file still owns 4096 — and growing
 * into that room is a metadata change (the recorded size) but not an
 * allocation. */
static uint64_t allocated_bytes(const struct xfs *fs, const struct xfs_inode *ip) {
    uint64_t end = 0;
    for (uint64_t block = 0;; block++) {
        struct xfs_extent e;
        if (xfs_bmap(fs, ip, block, &e) != KERR_OK) {
            break;
        }
        end = (e.startoff + e.blockcount) << fs->blocklog;
        block = e.startoff + e.blockcount - 1u;
    }
    return end > ip->size ? end : ip->size;
}

/* Checks a write before any of it happens: all of it must be possible, or
 * none of it is done. A half-written range would be worse than a refusal,
 * because the caller would have no way to know where it stopped. */
static int write_is_possible(struct xfs *fs, const struct xfs_inode *ip, uint64_t off, uint32_t len) {
    if (!xfs_writable(fs)) {
        return KERR_ROFS;
    }
    if (xfs_is_dir(ip)) {
        return KERR_ISDIR;
    }
    if (!xfs_is_reg(ip)) {
        return KERR_INVAL;
    }
    if (ip->format != XFS_DINODE_FMT_EXTENTS) {
        return KERR_NOTSUP; /* a file held inside its inode would change it */
    }
    if (off > ip->size) {
        return KERR_NOSPC; /* a hole past the end: that is the allocator's */
    }
    if (len > allocated_bytes(fs, ip) - off) {
        return KERR_NOSPC; /* past the blocks the file already has */
    }
    for (uint64_t block = off >> fs->blocklog; block <= (off + len - 1u) >> fs->blocklog; block++) {
        struct xfs_extent e;
        int rc = xfs_bmap(fs, ip, block, &e);
        if (rc == KERR_NOENT || (rc == KERR_OK && e.unwritten)) {
            return KERR_NOTSUP;
        }
        if (rc != KERR_OK) {
            return rc;
        }
    }
    return KERR_OK;
}

/* Records a file's new size: in the log first, then in its place.
 *
 * This is the only metadata a data write changes, and it is where the log
 * earns its place in the order of things — a crash between the two leaves
 * the change in the log rather than half done. What this kernel will not do
 * is **replay** one (xfs_logwrite.c), so it also writes an unmount record
 * afterwards and the reader goes on refusing a dirty log: Linux's
 * `xfs_repair` stays the recovery tool.
 *
 * The inode's own checksum is recomputed, because a v5 inode carries one
 * and an inode whose sum does not match is an inode nothing will read —
 * including this reader, which is how it would be noticed. */
static int record_new_size(struct xfs *fs, const struct xfs_inode *ip, uint64_t size) {
    uint64_t block;
    uint32_t boffset;
    struct bbuf *b;
    uint8_t *di;
    int rc;
    xfs_inode_place(fs, ip->ino, &block, &boffset);
    rc = bcache_get(fs->cache, block, &b);
    if (rc != KERR_OK) {
        return rc;
    }
    di = b->data + boffset;
    put_be64(di + XFS_DI_SIZE, size);
    put_le32(di + XFS_DI_CRC, 0);
    put_le32(di + XFS_DI_CRC, crc32c(di, fs->inodesize));
    struct xfs_log_inode_item item = {.ino = ip->ino, .chunk_block = block, .boffset = boffset, .core = di};
    rc = xfs_log_inodes(fs, &item, 1);
    if (rc == KERR_OK) {
        bcache_dirty(b);
    }
    bcache_put(fs->cache, b);
    return rc;
}

int xfs_file_write(struct xfs *fs, struct xfs_inode *ip, uint64_t off, const void *buf,
                   uint32_t len, uint32_t *put) {
    const uint8_t *in = buf;
    uint32_t done = 0;
    int rc;
    *put = 0;
    if (len == 0) {
        return KERR_OK;
    }
    if ((rc = write_is_possible(fs, ip, off, len)) != KERR_OK) {
        return rc;
    }
    while (done < len) {
        uint64_t at = off + done;
        uint32_t within = (uint32_t)(at & (fs->blocksize - 1u));
        uint32_t chunk = fs->blocksize - within < len - done ? fs->blocksize - within : len - done;
        if ((rc = write_chunk(fs, ip, at, in + done, within, chunk)) != KERR_OK) {
            *put = done; /* the range was checked, so this is a device error */
            return rc;
        }
        done += chunk;
    }
    *put = done;
    if (off + done <= ip->size) {
        return KERR_OK;
    }
    rc = record_new_size(fs, ip, off + done);
    if (rc == KERR_OK) {
        /* The in-memory inode follows the disk, or nothing above this ever
         * learns the file grew: a stat would keep reporting the old size
         * and a read would stop at it. */
        ip->size = off + done;
    }
    return rc;
}
