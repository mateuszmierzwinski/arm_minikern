package xfsmk

// Planning a filesystem: everything the writers need, computed from the
// device's size alone and nothing written yet. It is pure, so the
// arithmetic that decides where every structure goes is tested without a
// device (layout_test.go).
//
// The shape follows mkfs.xfs's, because that is what `xfs_repair` expects
// to find and it says so when it does not — "sb root inode value 32 valid
// but in unaligned location (expected 131136)" is how the numbers below
// were pinned down. Measured from a two-group filesystem mkfs.xfs made:
//
//	group 0:  0-3 headers and btree roots, 4-7 spare, 8-15 the inode
//	          chunk, then free to the end
//	group 1:  0-3 headers and btree roots, 4.. the log, then free
//
// mkfs.xfs refuses to make a single-group filesystem at all ("must have at
// least two superblocks for redundancy"), so neither does this.

// The log's bounds, in filesystem blocks.
const (
	minLogBlocks = 2048  // 8 MiB with 4 KiB blocks
	maxLogBlocks = 65536 // 256 MiB
	logFraction  = 8     // a log of about an eighth of the filesystem
)

// The fixed blocks at the start of every allocation group.
const (
	headerBlock    = 0 // the superblock, AGF, AGI and AGFL share it
	bnobtBlock     = 1
	cntbtBlock     = 2
	inobtBlock     = 3
	preallocBlocks = 4 // what the format reserves before anything is usable

	// What XFS keeps in reserve for a btree split. The blocks are free,
	// but the first inode chunk starts after them, which is where
	// xfs_repair expects to find the root.
	freelistReserve = 4

	// Groups, and the device size at which another one is added.
	minAgCount     = 2
	blocksPerAgMax = 256 * 1024 // 1 GiB with 4 KiB blocks
	maxAgCount     = 32
)

// Extent is a run of free blocks within one group.
type Extent struct {
	Start uint32
	Count uint32
}

// Group is one allocation group's plan.
type Group struct {
	Index       uint32
	Blocks      uint32
	Free        []Extent
	InodeChunk  uint32 // 0 when this group holds no inodes
	ChunkBlocks uint32
	LogStart    uint32 // 0 when the log is in another group
	LogBlocks   uint32
	Icount      uint32
	Ifree       uint32
}

// Defaults returns the geometry this formatter and the kernel's reader
// agree on: 4 KiB blocks, 512-byte sectors, 512-byte inodes.
func Defaults() Options {
	return Options{BlockSize: 4096, SectorSize: 512, InodeSize: 512}
}

// Plan works out where everything goes on a device of size bytes.
func Plan(size int64, opts Options) (Layout, error) {
	l := Layout{Options: opts}
	if err := l.takeGeometry(); err != nil {
		return Layout{}, err
	}
	l.Blocks = uint64(size) / uint64(opts.BlockSize)
	if err := l.divide(); err != nil {
		return Layout{}, err
	}
	l.place()
	if err := l.check(); err != nil {
		return Layout{}, err
	}
	return l, nil
}

// takeGeometry validates the sizes and derives the shifts the format is
// written in terms of.
func (l *Layout) takeGeometry() error {
	blockLog, blockOK := log2exact(uint64(l.BlockSize))
	sectorLog, sectorOK := log2exact(uint64(l.SectorSize))
	inodeLog, inodeOK := log2exact(uint64(l.InodeSize))
	if !blockOK || !sectorOK || !inodeOK || l.BlockSize != 4096 {
		return ErrGeometry
	}
	if l.SectorSize < 512 || l.SectorSize > l.BlockSize {
		return ErrGeometry
	}
	if l.InodeSize != 256 && l.InodeSize != 512 {
		return ErrGeometry
	}
	l.BlockLog, l.SectorLog, l.InodeLog = blockLog, sectorLog, inodeLog
	l.InopBlock = l.BlockSize / l.InodeSize
	l.InopBlog, _ = log2exact(uint64(l.InopBlock))
	return nil
}

// divide splits the device into groups of equal size, the last one taking
// whatever is left over.
func (l *Layout) divide() error {
	count := clamp(l.Blocks/blocksPerAgMax, minAgCount, maxAgCount)
	l.AgCount = uint32(count)
	l.AgBlocks = uint32(l.Blocks / count)
	if l.Blocks > 1<<31-1 {
		return ErrTooLarge
	}
	l.AgBlkLog = log2ceil(uint64(l.AgBlocks))
	l.LastAgBlocks = uint32(l.Blocks - uint64(l.AgCount-1)*uint64(l.AgBlocks))
	if l.AgBlocks < preallocBlocks+freelistReserve+1 {
		return ErrTooSmall
	}
	return nil
}

// place lays out each group: the inode chunk in the first, the log in the
// second, and free space wherever neither of them is.
func (l *Layout) place() {
	l.BnobtBlock, l.CntbtBlock, l.InobtBlock = bnobtBlock, cntbtBlock, inobtBlock
	l.ChunkBlocks = uint32(uint64(inodesPerChunk) * uint64(l.InodeSize) / uint64(l.BlockSize))
	l.InodeChunk = uint32(roundUp(preallocBlocks+freelistReserve, uint64(l.InodeAlignment())))
	l.LogBlocks = uint32(clamp(l.Blocks/logFraction, minLogBlocks, maxLogBlocks))
	l.LogAg = 1
	l.LogStart = preallocBlocks
	l.shrinkLogToFit()
	l.RootIno = l.InoFor(0, l.InodeChunk, 0)
	l.RbmIno = l.RootIno + 1
	l.RsumIno = l.RootIno + 2
	l.groups()
}

// shrinkLogToFit keeps the log inside its group, with room for the group's
// own reserve and a block of free space.
func (l *Layout) shrinkLogToFit() {
	room := l.groupBlocks(l.LogAg) - preallocBlocks - freelistReserve - 1
	if l.LogBlocks > room {
		l.LogBlocks = room
	}
}

// groups fills in each group's plan, including its free extents.
func (l *Layout) groups() {
	l.Groups = make([]Group, l.AgCount)
	for i := uint32(0); i < l.AgCount; i++ {
		g := Group{Index: i, Blocks: l.groupBlocks(i)}
		at := uint32(preallocBlocks)
		if i == l.LogAg {
			g.LogStart, g.LogBlocks = l.LogStart, l.LogBlocks
			at = l.LogStart + l.LogBlocks
		}
		if i == 0 {
			g.InodeChunk, g.ChunkBlocks = l.InodeChunk, l.ChunkBlocks
			g.Icount, g.Ifree = inodesPerChunk, inodesPerChunk-usedInodes
			if g.InodeChunk > at {
				g.Free = append(g.Free, Extent{Start: at, Count: g.InodeChunk - at})
			}
			at = g.InodeChunk + g.ChunkBlocks
		}
		if at < g.Blocks {
			g.Free = append(g.Free, Extent{Start: at, Count: g.Blocks - at})
		}
		l.Groups[i] = g
	}
}

// groupBlocks is how long one group is: they are equal but the last.
func (l *Layout) groupBlocks(index uint32) uint32 {
	if index == l.AgCount-1 {
		return l.LastAgBlocks
	}
	return l.AgBlocks
}

// check makes sure the plan leaves room to put a file somewhere.
func (l *Layout) check() error {
	if l.LogBlocks < minLogBlocks || l.Fdblocks() == 0 {
		return ErrTooSmall
	}
	if l.InopBlock == 0 || l.ChunkBlocks == 0 {
		return ErrGeometry
	}
	for _, g := range l.Groups {
		if g.InodeChunk+g.ChunkBlocks > g.Blocks || g.LogStart+g.LogBlocks > g.Blocks {
			return ErrTooSmall
		}
	}
	return nil
}

// InodeAlignment is sb_inoalignmt: the inode cluster size in blocks, which
// is what an inode chunk must be aligned to.
//
// The cluster is 8 KiB for the smallest inodes and scales with them, so
// 512-byte inodes give 16 KiB — four blocks. **Measured**: that is what
// mkfs.xfs writes, and xfs_repair refuses the filesystem outright if this
// field disagrees with its own idea of it ("inconsistent inode alignment
// value"), which is how the number was found.
func (l *Layout) InodeAlignment() uint32 {
	return 8192 * (l.InodeSize / 256) / l.BlockSize
}

// InoFor is the inode number of a slot in a block of a group.
func (l *Layout) InoFor(ag, block, slot uint32) uint64 {
	return uint64(ag)<<(l.AgBlkLog+l.InopBlog) | uint64(block)<<l.InopBlog | uint64(slot)
}

// Icount, Ifree and Fdblocks are what the superblock reports.
func (l *Layout) Icount() uint64 { return inodesPerChunk }
func (l *Layout) Ifree() uint64  { return inodesPerChunk - usedInodes }

// Fdblocks is every free block in every group.
func (l *Layout) Fdblocks() uint64 {
	var total uint64
	for _, g := range l.Groups {
		for _, e := range g.Free {
			total += uint64(e.Count)
		}
	}
	return total
}

// FreeBlocks is one group's free total, and Longest its biggest run: the
// two counters the AGF carries.
func (g *Group) FreeBlocks() uint32 {
	var total uint32
	for _, e := range g.Free {
		total += e.Count
	}
	return total
}

func (g *Group) Longest() uint32 {
	var longest uint32
	for _, e := range g.Free {
		if e.Count > longest {
			longest = e.Count
		}
	}
	return longest
}

// LogFsBlock is the log's start as a filesystem block number, which is what
// the superblock carries.
func (l *Layout) LogFsBlock() uint64 {
	return uint64(l.LogAg)<<l.AgBlkLog | uint64(l.LogStart)
}

// DeviceBlock turns a group-relative block into one on the device.
func (l *Layout) DeviceBlock(ag, block uint32) uint64 {
	return uint64(ag)*uint64(l.AgBlocks) + uint64(block)
}

// InodeBlock is the device block holding an inode, and the slot in it.
func (l *Layout) InodeBlock(ino uint64) (block uint64, slot uint32) {
	mask := (uint64(1) << l.AgBlkLog) - 1
	ag := uint32(ino >> (l.AgBlkLog + l.InopBlog))
	agbno := uint32((ino >> l.InopBlog) & mask)
	return l.DeviceBlock(ag, agbno), uint32(ino & (uint64(l.InopBlock) - 1))
}
