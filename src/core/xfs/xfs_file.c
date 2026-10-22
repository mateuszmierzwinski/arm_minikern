#include <stddef.h>

#include "core/errno.h"
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
