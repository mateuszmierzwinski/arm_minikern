#include <stddef.h>

#include "core/console.h"
#include "core/errno.h"
#include "core/xfs/xfs_internal.h"

/* Inodes: where one is, what it says, and which block holds a given part
 * of its file. */

void xfs_extent_unpack(const uint8_t *p, struct xfs_extent *out) {
    uint64_t hi = xfs_be64(p);
    uint64_t lo = xfs_be64(p + 8);
    out->unwritten = (hi >> 63) != 0;
    out->startoff = (hi >> 9) & (((uint64_t)1 << 54) - 1u);
    out->startblock = ((hi & 0x1ffu) << 43) | (lo >> 21);
    out->blockcount = (uint32_t)(lo & 0x1fffffu);
}

/* An inode number encodes where the inode is: which group, which block of
 * it, and which slot in that block. */
static void locate(const struct xfs *fs, uint64_t ino, uint64_t *fsb, uint32_t *slot) {
    uint64_t agbno_mask = ((uint64_t)1 << fs->agblklog) - 1u;
    uint64_t agno = ino >> (fs->agblklog + fs->inopblog);
    uint64_t agbno = (ino >> fs->inopblog) & agbno_mask;
    *fsb = (agno << fs->agblklog) | agbno;
    *slot = (uint32_t)(ino & (((uint64_t)1 << fs->inopblog) - 1u));
}

/* The inline fork's extent: it ends where the attribute fork begins, or at
 * the end of the inode when there is no attribute fork. */
static uint32_t fork_bytes(const struct xfs *fs, const uint8_t *di) {
    uint32_t forkoff = di[XFS_DI_FORKOFF];
    uint32_t area = fs->inodesize - XFS_DI_V3_SIZE;
    return forkoff == 0 ? area : forkoff * 8u;
}

/* Everything that must be true of an inode before its contents are used:
 * it says it is an inode, in the layout we read, it knows its own number,
 * and its checksum and UUID tie it to this filesystem. */
static int verify(struct xfs *fs, const uint8_t *di, uint64_t ino) {
    if (xfs_be16(di + XFS_DI_MAGIC) != XFS_DINODE_MAGIC) {
        return KERR_IO;
    }
    if (di[XFS_DI_VERSION] != XFS_DI_VERSION_3 || xfs_be64(di + XFS_DI_INO) != ino) {
        return KERR_IO;
    }
    if (!xfs_crc_ok(di, fs->inodesize, XFS_DI_CRC) || !xfs_uuid_ok(fs, di + XFS_DI_UUID)) {
        return KERR_IO;
    }
    if ((xfs_be64(di + XFS_DI_FLAGS2) & XFS_DIFLAG2_NREXT64) != 0) {
        kprintf("xfs: inode %u uses 64-bit extent counts, which are not supported\n", (uint32_t)ino);
        return KERR_NOTSUP;
    }
    return KERR_OK;
}

/* Copies the core fields and the inline fork out of a verified inode. */
static int take(struct xfs *fs, const uint8_t *di, uint64_t ino, struct xfs_inode *out) {
    uint32_t avail = fork_bytes(fs, di);
    *out = (struct xfs_inode){
        .ino = ino,
        .size = xfs_be64(di + XFS_DI_SIZE),
        .nblocks = xfs_be64(di + XFS_DI_NBLOCKS),
        .mode = xfs_be16(di + XFS_DI_MODE),
        .nlink = xfs_be32(di + XFS_DI_NLINK),
        .nextents = xfs_be32(di + XFS_DI_NEXTENTS),
        .format = di[XFS_DI_FORMAT],
        .fork_len = avail < XFS_FORK_MAX ? avail : XFS_FORK_MAX,
    };
    if (out->format == XFS_DINODE_FMT_BTREE) {
        kprintf("xfs: inode %u has a btree fork, which is not supported\n", (uint32_t)ino);
        return KERR_NOTSUP;
    }
    if (out->format == XFS_DINODE_FMT_LOCAL && out->size > out->fork_len) {
        return KERR_IO; /* says it holds more than fits inside it */
    }
    if (out->format == XFS_DINODE_FMT_EXTENTS &&
        (uint64_t)out->nextents * XFS_EXTENT_BYTES > out->fork_len) {
        return KERR_IO; /* more extents than the inode has room for */
    }
    for (uint32_t i = 0; i < out->fork_len; i++) {
        out->fork[i] = di[XFS_DI_V3_SIZE + i];
    }
    return KERR_OK;
}

int xfs_inode_read(struct xfs *fs, uint64_t ino, struct xfs_inode *out) {
    struct bbuf *b;
    uint64_t fsb;
    uint32_t slot;
    int rc;
    locate(fs, ino, &fsb, &slot);
    if (!xfs_fsb_valid(fs, fsb, 1) || slot >= fs->inopblock) {
        return KERR_NOENT;
    }
    rc = bcache_get(fs->cache, xfs_fsb_to_block(fs, fsb), &b);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = verify(fs, b->data + slot * fs->inodesize, ino);
    if (rc == KERR_OK) {
        rc = take(fs, b->data + slot * fs->inodesize, ino, out);
    }
    bcache_put(fs->cache, b);
    return rc;
}

int xfs_bmap(const struct xfs *fs, const struct xfs_inode *ip, uint64_t off, struct xfs_extent *out) {
    if (ip->format != XFS_DINODE_FMT_EXTENTS) {
        return KERR_NOTSUP;
    }
    for (uint32_t i = 0; i < ip->nextents; i++) {
        struct xfs_extent e;
        xfs_extent_unpack(ip->fork + i * XFS_EXTENT_BYTES, &e);
        if (e.blockcount == 0 || !xfs_fsb_valid(fs, e.startblock, e.blockcount)) {
            return KERR_IO;
        }
        if (off >= e.startoff && off - e.startoff < e.blockcount) {
            *out = e;
            return KERR_OK;
        }
    }
    return KERR_NOENT; /* a hole, or past the last extent */
}

bool xfs_is_dir(const struct xfs_inode *ip) {
    return (ip->mode & 0170000u) == 0040000u;
}

bool xfs_is_reg(const struct xfs_inode *ip) {
    return (ip->mode & 0170000u) == 0100000u;
}

bool xfs_is_link(const struct xfs_inode *ip) {
    return (ip->mode & 0170000u) == 0120000u;
}
