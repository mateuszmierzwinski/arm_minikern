package xfsmk

import "hash/crc32"

// XFS's self-describing metadata carries CRC-32C, computed with the
// checksum field itself counted as zero and stored little-endian. The
// kernel's reader does the same thing in C (src/lib/crc32c.c); here the
// standard library's Castagnoli table does it.
var castagnoli = crc32.MakeTable(crc32.Castagnoli)

// seal computes the checksum of buf and writes it at crcOff. buf must be
// the whole structure the checksum covers — the sector for a superblock or
// an AG header, the block for a btree or directory block, the inode for an
// inode (XFS.md §2).
func seal(buf []byte, crcOff int) {
	putLE32(buf[crcOff:], 0)
	putLE32(buf[crcOff:], crc32.Checksum(buf, castagnoli))
}
