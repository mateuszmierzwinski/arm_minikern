package xfsmk

import (
	"errors"
	"testing"
)

const mib = 1 << 20

func TestPlanA512MiBDevice(t *testing.T) {
	l, err := Plan(512*mib, Defaults())
	if err != nil {
		t.Fatal(err)
	}
	if l.Blocks != 131072 || l.AgBlocks != 65536 {
		t.Errorf("blocks = %d, agblocks = %d", l.Blocks, l.AgBlocks)
	}
	if l.AgBlkLog != 16 {
		t.Errorf("agblklog = %d, want 16 (enough bits for 65536 blocks)", l.AgBlkLog)
	}
	if l.InopBlock != 8 || l.InopBlog != 3 {
		t.Errorf("inopblock = %d, inopblog = %d", l.InopBlock, l.InopBlog)
	}
	if l.ChunkBlocks != 8 {
		t.Errorf("a chunk of 64 inodes of 512 bytes is %d blocks, want 8", l.ChunkBlocks)
	}
	if l.RootIno != 64 || l.RbmIno != 65 || l.RsumIno != 66 {
		t.Errorf("root = %d, rbm = %d, rsum = %d; want 64, 65, 66 as mkfs.xfs makes them",
			l.RootIno, l.RbmIno, l.RsumIno)
	}
	// mkfs.xfs's own two-group layout, which is what xfs_repair expects:
	// the inode chunk at block 8 of group 0, the log at block 4 of group 1.
	if l.AgCount != 2 || l.AgBlocks != 65536 {
		t.Errorf("agcount = %d, agblocks = %d; want 2 groups of 65536", l.AgCount, l.AgBlocks)
	}
	if l.InodeChunk != 8 || l.RootIno != 64 {
		t.Errorf("the chunk is at block %d and the root is inode %d; want 8 and 64", l.InodeChunk,
			l.RootIno)
	}
	if l.LogAg != 1 || l.LogStart != 4 || l.LogBlocks != 16384 {
		t.Errorf("log in group %d at block %d for %d blocks", l.LogAg, l.LogStart, l.LogBlocks)
	}
	if l.InodeAlignment() != 4 {
		t.Errorf("inoalignmt = %d, want 4", l.InodeAlignment())
	}
	// 114672 is what mkfs.xfs reports for the same device, to the block.
	if l.Fdblocks() != 114672 {
		t.Errorf("fdblocks = %d, want 114672 (what mkfs.xfs makes)", l.Fdblocks())
	}
}

// Everything the layout claims must add up: metadata plus log plus free
// space is the whole device, and nothing overlaps.
// Everything the plan claims must add up, at every size: each group is
// accounted for block by block, nothing overlaps, and the inode numbers
// resolve back to where the inodes are.
func TestThePlanAddsUp(t *testing.T) {
	for _, size := range []int64{32 * mib, 64 * mib, 512 * mib, 4096 * mib, 16384 * mib} {
		l, err := Plan(size, Defaults())
		if err != nil {
			t.Fatalf("%d MiB: %v", size/mib, err)
		}
		checkGroups(t, size, l)
		block, slot := l.InodeBlock(l.RootIno)
		if block != l.DeviceBlock(0, l.InodeChunk) || slot != 0 {
			t.Errorf("%d MiB: the root inode is at block %d slot %d, want %d/0", size/mib, block,
				slot, l.DeviceBlock(0, l.InodeChunk))
		}
		// The last group reaches exactly the end of the device.
		last := l.Groups[len(l.Groups)-1]
		if l.DeviceBlock(last.Index, 0)+uint64(last.Blocks) != l.Blocks {
			t.Errorf("%d MiB: the groups do not cover the device", size/mib)
		}
	}
}

// checkGroups accounts for every block of every group.
func checkGroups(t *testing.T, size int64, l Layout) {
	t.Helper()
	if uint32(len(l.Groups)) != l.AgCount {
		t.Fatalf("%d MiB: %d groups planned, agcount says %d", size/mib, len(l.Groups), l.AgCount)
	}
	for _, g := range l.Groups {
		used := uint32(preallocBlocks) + g.ChunkBlocks + g.LogBlocks
		if used+g.FreeBlocks() != g.Blocks {
			t.Errorf("%d MiB group %d: %d used + %d free != %d blocks", size/mib, g.Index, used,
				g.FreeBlocks(), g.Blocks)
		}
		var last uint32
		for _, e := range g.Free {
			if e.Start < last || e.Count == 0 || e.Start+e.Count > g.Blocks {
				t.Errorf("%d MiB group %d: a free extent %d+%d is out of order or out of range",
					size/mib, g.Index, e.Start, e.Count)
			}
			last = e.Start + e.Count
		}
		if g.LogBlocks > 0 && g.ChunkBlocks > 0 && g.LogStart < g.InodeChunk+g.ChunkBlocks &&
			g.InodeChunk < g.LogStart+g.LogBlocks {
			t.Errorf("%d MiB group %d: the log and the inode chunk overlap", size/mib, g.Index)
		}
	}
}

// The log is bounded at both ends: big enough for a transaction, and not
// most of a small volume.
func TestTheLogIsBounded(t *testing.T) {
	small, err := Plan(32*mib, Defaults())
	if err != nil {
		t.Fatal(err)
	}
	if small.LogBlocks != minLogBlocks {
		t.Errorf("a small volume's log is %d blocks, want the minimum %d", small.LogBlocks,
			minLogBlocks)
	}
	big, err := Plan(64*1024*mib, Defaults())
	if err != nil {
		t.Fatal(err)
	}
	if big.LogBlocks != maxLogBlocks {
		t.Errorf("a large volume's log is %d blocks, want the maximum %d", big.LogBlocks,
			maxLogBlocks)
	}
}

func TestPlanRefusesWhatItCannotMake(t *testing.T) {
	cases := []struct {
		name string
		size int64
		opts Options
		want error
	}{
		{"nothing at all", 0, Defaults(), ErrTooSmall},
		{"no room for a log", 4 * mib, Defaults(), ErrTooSmall},
		{"no room for a log in its own group", 16 * mib, Defaults(), ErrTooSmall},
		{"a block size that is not a power of two", 512 * mib,
			Options{BlockSize: 5000, SectorSize: 512, InodeSize: 512}, ErrGeometry},
		{"a block size we do not support", 512 * mib,
			Options{BlockSize: 8192, SectorSize: 512, InodeSize: 512}, ErrGeometry},
		{"a sector larger than a block", 512 * mib,
			Options{BlockSize: 4096, SectorSize: 8192, InodeSize: 512}, ErrGeometry},
		{"a sector that is not a power of two", 512 * mib,
			Options{BlockSize: 4096, SectorSize: 500, InodeSize: 512}, ErrGeometry},
		{"an inode size we do not support", 512 * mib,
			Options{BlockSize: 4096, SectorSize: 512, InodeSize: 1024}, ErrGeometry},
		{"a device needing two groups", int64(1<<31) * 4096, Defaults(), ErrTooLarge},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			if _, err := Plan(c.size, c.opts); !errors.Is(err, c.want) {
				t.Errorf("Plan = %v, want %v", err, c.want)
			}
		})
	}
}

// The smallest filesystem this will make: two groups, each big enough for
// its own reserve, with the log fitting in the second.
func TestTheSmallestFilesystem(t *testing.T) {
	size := int64(2) * int64(preallocBlocks+freelistReserve+minLogBlocks+1) * 4096
	l, err := Plan(size, Defaults())
	if err != nil {
		t.Fatalf("%d bytes: %v", size, err)
	}
	if l.LogBlocks < minLogBlocks {
		t.Errorf("the log shrank below its minimum: %d", l.LogBlocks)
	}
	if l.Fdblocks() == 0 {
		t.Error("there is nowhere to put a file")
	}
}

func BenchmarkPlan(b *testing.B) {
	for b.Loop() {
		if _, err := Plan(512*mib, Defaults()); err != nil {
			b.Fatal(err)
		}
	}
}
