// Package xfsmk formats an XFS v5 filesystem.
//
// It is in Go, in user space, on purpose: formatting is a lot of bit
// twiddling over structures that must come out exactly right, and Go is a
// far better place to do that than a freestanding kernel — while
// `xfs_repair` is a better judge of the result than any test of ours.
// The kernel only has to *read* what this writes (src/core/xfs), and the
// facts both ends work from are in XFS.md.
//
// What it makes is deliberately the simplest filesystem the kernel's reader
// supports: **one allocation group**, an internal log, one chunk of inodes,
// and one free extent. No finobt, no rmapbt, no reflink, no sparse inodes.
package xfsmk

// Wire constants, from XFS.md §2. These are an ABI.
const (
	sbMagic    = 0x58465342 // "XFSB"
	agfMagic   = 0x58414746 // "XAGF"
	agiMagic   = 0x58414749 // "XAGI"
	agflMagic  = 0x5841464c // "XAFL"
	bnobtMagic = 0x41423342 // "AB3B": free space by block
	cntbtMagic = 0x41423343 // "AB3C": free space by count
	inobtMagic = 0x49414233 // "IAB3": the inode btree
	inodeMagic = 0x494e     // "IN"
	logMagic   = 0xfeedbabe
)

// Offsets within the superblock sector.
const (
	sbOffBlocksize       = 4
	sbOffDblocks         = 8
	sbOffUUID            = 32
	sbOffLogstart        = 48
	sbOffRootino         = 56
	sbOffRbmino          = 64
	sbOffRsumino         = 72
	sbOffRextsize        = 80
	sbOffAgblocks        = 84
	sbOffAgcount         = 88
	sbOffLogblocks       = 96
	sbOffVersionnum      = 100
	sbOffSectsize        = 102
	sbOffInodesize       = 104
	sbOffInopblock       = 106
	sbOffBlocklog        = 120
	sbOffSectlog         = 121
	sbOffInodelog        = 122
	sbOffInopblog        = 123
	sbOffAgblklog        = 124
	sbOffRextslog        = 125
	sbOffInprogress      = 126
	sbOffImaxPct         = 127
	sbOffIcount          = 128
	sbOffIfree           = 136
	sbOffFdblocks        = 144
	sbOffFrextents       = 152
	sbOffQflags          = 176
	sbOffFlags           = 178
	sbOffInoalignmt      = 180
	sbOffDirblklog       = 192
	sbOffLogsunit        = 196
	sbOffFeatures2       = 200
	sbOffBadFeat2        = 204
	sbOffFeatCompat      = 208
	sbOffFeatRo          = 212
	sbOffFeatIncompat    = 216
	sbOffFeatLogIncompat = 220
	sbOffCRC             = 224
	sbOffSpinoAlign      = 228
	sbOffPquotino        = 232
	sbOffLSN             = 240
	sbOffMetaUUID        = 248
)

// Offsets within the AGF sector.
const (
	agfOffVersion   = 4
	agfOffSeqno     = 8
	agfOffLength    = 12
	agfOffRoots     = 16 // three of them: bno, cnt, rmap
	agfOffLevels    = 28
	agfOffFlfirst   = 40
	agfOffFllast    = 44
	agfOffFlcount   = 48
	agfOffFreeblks  = 52
	agfOffLongest   = 56
	agfOffBtreeblks = 60
	agfOffUUID      = 64
	agfOffCRC       = 216
)

// Offsets within the AGI sector.
const (
	agiOffVersion   = 4
	agiOffSeqno     = 8
	agiOffLength    = 12
	agiOffCount     = 16
	agiOffRoot      = 20
	agiOffLevel     = 24
	agiOffFreecount = 28
	agiOffNewino    = 32
	agiOffDirino    = 36
	agiOffUnlinked  = 40 // 64 entries
	agiOffUUID      = 296
	agiOffCRC       = 312
	agiOffFreeRoot  = 328
	agiOffFreeLevel = 332
)

// Offsets within the AGFL sector, and within a short-form btree block.
const (
	agflOffSeqno = 4
	agflOffUUID  = 8
	agflOffCRC   = 32
	agflOffBnos  = 36

	btOffLevel    = 4
	btOffNumrecs  = 6
	btOffLeftsib  = 8
	btOffRightsib = 12
	btOffBlkno    = 16
	btOffLSN      = 24
	btOffUUID     = 32
	btOffOwner    = 48
	btOffCRC      = 52
	btRecords     = 56
)

// Offsets within an inode.
const (
	diOffMode         = 2
	diOffVersion      = 4
	diOffFormat       = 5
	diOffNlink        = 16
	diOffSize         = 56
	diOffNblocks      = 64
	diOffExtsize      = 72
	diOffNextents     = 76
	diOffForkoff      = 82
	diOffAformat      = 83
	diOffGen          = 92
	diOffNextUnlinked = 96
	diOffCRC          = 100
	diOffChangecount  = 104
	diOffFlags2       = 120
	diOffCrtime       = 148
	diOffIno          = 152
	diOffUUID         = 160
	diCoreSize        = 176
)

// Offsets within a log record header and its first operation.
const (
	logRecCycle     = 4
	logRecVersion   = 8
	logRecLen       = 12
	logRecLSN       = 16
	logRecTailLSN   = 24
	logRecCRC       = 32
	logRecPrevBlock = 36
	logRecNumOps    = 40
	logRecCycleData = 44  // 64 entries, one per 512-byte block of a record
	logRecFmt       = 300 // measured: 44 + 64*4, not a byte earlier
	logRecFsUUID    = 304
	logRecSize      = 320
	logSector       = 512

	// The iclog size a record declares, which is what mkfs.xfs writes.
	logIclogSize = 32768

	// The transaction identifier mkfs.xfs uses for the unmount record.
	// Any value would do; this one makes our log byte-comparable with
	// theirs, which is how two offsets in this header were found.
	logUnmountTID = 0xb0c0d0d0

	logOpTID      = 0
	logOpLen      = 4
	logOpClientID = 8
	logOpFlags    = 9
	logOpHdrSize  = 12

	logClientLog    = 0xaa
	logUnmountTrans = 0x20
	logUnmountMagic = 0x556e
	logFmtLinux     = 1 // XLOG_FMT_LINUX_LE
)

// Feature flags we set, and the ones we deliberately leave clear (XFS.md §4).
const (
	versionNum = 0xb4a5 // v5, with the bits mkfs sets alongside it

	featRoFinobt = 0x01

	featInFtype = 0x01

	features2 = 0x18a // lazy superblock counters, attr2, projid32bit
)

// Inode formats and modes.
const (
	fmtLocal   = 1
	fmtExtents = 2

	modeDir  = 0o040755
	modeFile = 0o0100000

	inodeVersion = 3
)

// How many inodes XFS allocates at a time, and how many of them this
// formatter fills in: the root directory, and the realtime bitmap and
// summary that the superblock points at.
const (
	inodesPerChunk = 64
	usedInodes     = 3
)

// Options are what a caller may choose. Everything else is computed.
type Options struct {
	// BlockSize is the filesystem's block size; 4096 is the only size the
	// kernel's reader supports today.
	BlockSize uint32

	// SectorSize is the smallest unit the superblock and AG headers are
	// checksummed over.
	SectorSize uint32

	// InodeSize is 256 or 512.
	InodeSize uint32

	// UUID is the filesystem's identity. Zero means "generate one".
	UUID [16]byte
}

// Layout is where everything goes: the result of planning a filesystem on a
// device of a given size, and the only thing the writers consult.
type Layout struct {
	Options
	Blocks       uint64 // the whole filesystem, in blocks
	AgCount      uint32
	AgBlocks     uint32 // every allocation group but the last
	LastAgBlocks uint32 // the last one, which takes what is left over
	BlockLog     uint8
	SectorLog    uint8
	InodeLog     uint8
	InopBlog     uint8
	AgBlkLog     uint8
	InopBlock    uint32

	BnobtBlock  uint32 // the free-space-by-block btree root
	CntbtBlock  uint32 // the free-space-by-count btree root
	InobtBlock  uint32 // the inode btree root
	InodeChunk  uint32 // the first block of the inode chunk
	ChunkBlocks uint32

	LogAg     uint32 // which group holds the log
	LogStart  uint32 // its first block within that group
	LogBlocks uint32

	Groups []Group

	RootIno uint64
	RbmIno  uint64
	RsumIno uint64
}
