/* xfs_internal.h — what the parts of the reader share: the big-endian
 * accessors, the block-number arithmetic, and the one function that gets a
 * metadata block and refuses to hand it over unverified. */
#ifndef MINIKERN_CORE_XFS_XFS_INTERNAL_H
#define MINIKERN_CORE_XFS_XFS_INTERNAL_H

#include "core/xfs/xfs.h"
#include "core/xfs/xfs_format.h"

static inline uint16_t xfs_be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static inline uint32_t xfs_be32(const uint8_t *p) {
    return ((uint32_t)xfs_be16(p) << 16) | xfs_be16(p + 2);
}

static inline uint64_t xfs_be64(const uint8_t *p) {
    return ((uint64_t)xfs_be32(p) << 32) | xfs_be32(p + 4);
}

/* The checksums are the one little-endian field in the format. */
static inline uint32_t xfs_le32(const uint8_t *p) {
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
}

/* A filesystem block number is (ag << agblklog) | agbno, and the group's
 * blocks are agblocks apart on the device — which is not the same thing,
 * because agblocks need not be a power of two. */
static inline uint64_t xfs_fsb_to_block(const struct xfs *fs, uint64_t fsb) {
    uint64_t mask = ((uint64_t)1 << fs->agblklog) - 1u;
    return (fsb >> fs->agblklog) * fs->agblocks + (fsb & mask);
}

static inline uint32_t xfs_fsb_ag(const struct xfs *fs, uint64_t fsb) {
    return (uint32_t)(fsb >> fs->agblklog);
}

/* True when fsb names a block inside the filesystem. */
bool xfs_fsb_valid(const struct xfs *fs, uint64_t fsb, uint64_t count);

/* Verifies the CRC32C stored little-endian at crc_off within len bytes,
 * with the field itself counted as zero (which is how it was computed). */
bool xfs_crc_ok(const uint8_t *buf, uint32_t len, uint32_t crc_off);

/* The filesystem's UUID at uuid, or not. */
bool xfs_uuid_ok(const struct xfs *fs, const uint8_t *uuid);

/* One verified metadata block, pinned: its magic, its CRC and its UUID all
 * checked. KERR_IO when any of them is wrong, KERR_INVAL for a block
 * number outside the filesystem. A second magic may be given (directories
 * have two), or 0 for none. */
int xfs_meta_block(struct xfs *fs, uint64_t fsb, uint32_t magic, uint32_t magic2, uint32_t crc_off,
                   uint32_t uuid_off, struct bbuf **out);

/* The clean-log check (xfs_log.c): KERR_OK when the log's newest record is
 * an unmount record, KERR_NOTSUP when it is anything else. */
int xfs_log_check(struct xfs *fs);

/* Where an inode lives: the device block holding its chunk, and its byte
 * offset within that block. The same arithmetic the reader uses, exposed
 * because a write has to put the inode back where it came from. */
void xfs_inode_place(const struct xfs *fs, uint64_t ino, uint64_t *device_block, uint32_t *boffset);

/* One inode in a transaction: which inode, where on the device its chunk
 * is and where in that chunk it sits — what recovery would need to find it
 * — and its on-disk core. */
struct xfs_log_inode_item {
    uint64_t ino;
    uint64_t chunk_block;
    uint32_t boffset;
    const uint8_t *core; /* the on-disk core, big-endian; converted on the way in */
};

#define XFS_LOG_MAX_ITEMS 2u

/* Writes these inodes to the log as one committed transaction, then an
 * unmount record after it so the log is clean again (xfs_logwrite.c).
 *
 * Several items rather than one because a change usually is several: a
 * created file is a new inode *and* its parent's directory, and the point
 * of a transaction is that they arrive together or not at all. */
int xfs_log_inodes(struct xfs *fs, const struct xfs_log_inode_item *items, uint32_t n);

/* One extent record, unpacked. */
struct xfs_extent {
    uint64_t startoff;   /* logical block within the file */
    uint64_t startblock; /* filesystem block */
    uint32_t blockcount;
    bool unwritten; /* allocated but never written: reads as zeroes */
};

/* Unpacks the sixteen-byte record at p. */
void xfs_extent_unpack(const uint8_t *p, struct xfs_extent *out);

/* The extent covering logical block off, or KERR_NOENT when off is in a
 * hole. Walks the inode's inline extent list. */
int xfs_bmap(const struct xfs *fs, const struct xfs_inode *ip, uint64_t off, struct xfs_extent *out);

#endif
