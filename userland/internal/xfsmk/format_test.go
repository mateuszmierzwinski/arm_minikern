package xfsmk

import (
	"bytes"
	"encoding/binary"
	"errors"
	"testing"
)

// memDevice is a device in memory, so Format can be driven without a file.
// It records what it was asked to do, because *where* a formatter writes
// matters as much as what.
type memDevice struct {
	data    []byte
	synced  int
	failAt  int64 // a write at or past this offset fails
	written map[int64]int
}

func newMemDevice(size int) *memDevice {
	return &memDevice{data: make([]byte, size), failAt: -1, written: map[int64]int{}}
}

func (d *memDevice) WriteAt(p []byte, off int64) (int, error) {
	if d.failAt >= 0 && off >= d.failAt {
		return 0, errors.New("the device gave up")
	}
	if off < 0 || off+int64(len(p)) > int64(len(d.data)) {
		return 0, errors.New("write outside the device")
	}
	copy(d.data[off:], p)
	d.written[off] += len(p)
	return len(p), nil
}

func (d *memDevice) ReadAt(p []byte, off int64) (int, error) {
	if off < 0 || off+int64(len(p)) > int64(len(d.data)) {
		return 0, errors.New("read outside the device")
	}
	copy(p, d.data[off:])
	return len(p), nil
}

func (d *memDevice) Size() (int64, error) { return int64(len(d.data)), nil }
func (d *memDevice) Sync() error          { d.synced++; return nil }

// fixedRandom makes the UUID predictable, so a test can assert it reached
// every structure that must carry it.
type fixedRandom struct{ b byte }

func (r fixedRandom) Read(p []byte) (int, error) {
	for i := range p {
		p[i] = r.b
	}
	return len(p), nil
}

func formatted(t *testing.T, size int) (*memDevice, Layout) {
	t.Helper()
	dev := newMemDevice(size)
	l, err := Format(dev, Defaults(), fixedRandom{b: 0x5a})
	if err != nil {
		t.Fatalf("Format: %v", err)
	}
	return dev, l
}

func TestFormatWritesTheSuperblock(t *testing.T) {
	dev, l := formatted(t, 64*mib)
	sb := dev.data[:512]
	if binary.BigEndian.Uint32(sb) != sbMagic {
		t.Fatalf("magic = %#x", binary.BigEndian.Uint32(sb))
	}
	if binary.BigEndian.Uint32(sb[sbOffBlocksize:]) != 4096 {
		t.Error("the block size is wrong")
	}
	if binary.BigEndian.Uint64(sb[sbOffDblocks:]) != l.Blocks {
		t.Error("dblocks does not match the plan")
	}
	if binary.BigEndian.Uint64(sb[sbOffRootino:]) != l.RootIno {
		t.Error("the root inode is wrong")
	}
	if binary.BigEndian.Uint16(sb[sbOffVersionnum:])&0xf != 5 {
		t.Error("this is not a v5 superblock")
	}
	if got := binary.BigEndian.Uint32(sb[sbOffFeatIncompat:]); got != featInFtype {
		t.Errorf("incompat features = %#x, want just ftype", got)
	}
	if got := binary.BigEndian.Uint32(sb[sbOffFeatRo:]); got != 0 {
		t.Errorf("read-only features = %#x, want none", got)
	}
	if sb[sbOffInprogress] != 0 {
		t.Error("inprogress must be clear: the filesystem is finished")
	}
	if dev.synced != 1 {
		t.Errorf("synced %d times, want once", dev.synced)
	}
}

// Every structure must carry the filesystem's UUID, or a reader will refuse
// it as belonging to another filesystem.
func TestEveryStructureCarriesTheUUID(t *testing.T) {
	dev, l := formatted(t, 64*mib)
	uuid := bytes.Repeat([]byte{0x5a}, 16)
	uuid[6] = 0x5a&0x0f | 0x40
	uuid[8] = 0x5a&0x3f | 0x80
	places := map[string]int64{
		"superblock": sbOffUUID,
		"agf":        512 + agfOffUUID,
		"agi":        1024 + agiOffUUID,
		"agfl":       1536 + agflOffUUID,
		"bnobt":      int64(l.BnobtBlock)*4096 + btOffUUID,
		"cntbt":      int64(l.CntbtBlock)*4096 + btOffUUID,
		"inobt":      int64(l.InobtBlock)*4096 + btOffUUID,
		"root inode": int64(l.InodeChunk)*4096 + diOffUUID,
	}
	for name, at := range places {
		if !bytes.Equal(dev.data[at:at+16], uuid) {
			t.Errorf("%s does not carry the filesystem's uuid", name)
		}
	}
}

func TestFormatWritesTheAGHeaders(t *testing.T) {
	dev, l := formatted(t, 64*mib)
	agf, agi, agfl := dev.data[512:1024], dev.data[1024:1536], dev.data[1536:2048]
	if binary.BigEndian.Uint32(agf) != agfMagic || binary.BigEndian.Uint32(agi) != agiMagic ||
		binary.BigEndian.Uint32(agfl) != agflMagic {
		t.Fatal("an AG header has the wrong magic")
	}
	if binary.BigEndian.Uint32(agf[agfOffLength:]) != l.AgBlocks {
		t.Error("the group's length is wrong")
	}
	g := &l.Groups[0]
	if binary.BigEndian.Uint32(agf[agfOffFreeblks:]) != g.FreeBlocks() ||
		binary.BigEndian.Uint32(agf[agfOffLongest:]) != g.Longest() {
		t.Error("the free-space counters do not match the group's free extents")
	}
	if binary.BigEndian.Uint32(agf[agfOffFlcount:]) != 0 {
		t.Error("a new filesystem has freed nothing, so the free list is empty")
	}
	if binary.BigEndian.Uint32(agi[agiOffCount:]) != uint32(l.Icount()) ||
		binary.BigEndian.Uint32(agi[agiOffFreecount:]) != uint32(l.Ifree()) {
		t.Error("the inode counters are wrong")
	}
	if binary.BigEndian.Uint32(agi[agiOffRoot:]) != l.InobtBlock {
		t.Error("the inode btree root is wrong")
	}
	for i := 0; i < 64; i++ {
		if binary.BigEndian.Uint32(agi[agiOffUnlinked+i*4:]) != ^uint32(0) {
			t.Fatalf("unlinked[%d] must be null", i)
		}
	}
}

func TestFormatWritesTheBtrees(t *testing.T) {
	dev, l := formatted(t, 64*mib)
	for _, bt := range []struct {
		name  string
		block uint32
		magic uint32
	}{
		{"bnobt", l.BnobtBlock, bnobtMagic},
		{"cntbt", l.CntbtBlock, cntbtMagic},
		{"inobt", l.InobtBlock, inobtMagic},
	} {
		blk := dev.data[int(bt.block)*4096 : (int(bt.block)+1)*4096]
		if binary.BigEndian.Uint32(blk) != bt.magic {
			t.Errorf("%s has the wrong magic", bt.name)
		}
		if binary.BigEndian.Uint16(blk[btOffLevel:]) != 0 {
			t.Errorf("%s's root should be a leaf", bt.name)
		}
		if binary.BigEndian.Uint16(blk[btOffNumrecs:]) == 0 {
			t.Errorf("%s holds no records", bt.name)
		}
		if binary.BigEndian.Uint32(blk[btOffLeftsib:]) != ^uint32(0) ||
			binary.BigEndian.Uint32(blk[btOffRightsib:]) != ^uint32(0) {
			t.Errorf("%s's root has siblings", bt.name)
		}
		if binary.BigEndian.Uint64(blk[btOffBlkno:]) != uint64(bt.block)*8 {
			t.Errorf("%s does not know its own block", bt.name)
		}
		if binary.BigEndian.Uint32(blk[btOffOwner:]) != 0 {
			t.Errorf("%s names the wrong group", bt.name)
		}
	}
	// The free-space records are the group's free extents, in both trees,
	// and they must add up to what the AGF claims.
	g := &l.Groups[0]
	for _, block := range []uint32{l.BnobtBlock, l.CntbtBlock} {
		blk := dev.data[int(block)*4096:]
		var total uint32
		for i := 0; i < int(binary.BigEndian.Uint16(blk[btOffNumrecs:])); i++ {
			total += binary.BigEndian.Uint32(blk[btRecords+i*8+4:])
		}
		if total != g.FreeBlocks() {
			t.Errorf("block %d holds %d free blocks, the group has %d", block, total,
				g.FreeBlocks())
		}
	}
	// The inode record covers the chunk, with three inodes in use.
	rec := dev.data[int(l.InobtBlock)*4096+btRecords:]
	if binary.BigEndian.Uint32(rec) != uint32(l.RootIno) {
		t.Error("the inode record does not start at the root inode")
	}
	if binary.BigEndian.Uint32(rec[4:]) != uint32(l.Ifree()) {
		t.Error("the inode record's free count is wrong")
	}
	if binary.BigEndian.Uint64(rec[8:]) != FreeInodeMask() {
		t.Errorf("the free bitmap is %#x, want the first three clear", binary.BigEndian.Uint64(rec[8:]))
	}
}

// Every inode of the chunk must be initialised, not just the three in use:
// a repair tool checks all of them.
func TestFormatWritesEveryInodeOfTheChunk(t *testing.T) {
	dev, l := formatted(t, 64*mib)
	for i := uint64(0); i < inodesPerChunk; i++ {
		ino := l.RootIno + i
		block, slot := l.InodeBlock(ino)
		at := int(block)*4096 + int(slot)*int(l.InodeSize)
		di := dev.data[at : at+int(l.InodeSize)]
		if binary.BigEndian.Uint16(di) != inodeMagic {
			t.Fatalf("inode %d has no magic", ino)
		}
		if di[diOffVersion] != inodeVersion {
			t.Fatalf("inode %d is version %d", ino, di[diOffVersion])
		}
		if binary.BigEndian.Uint64(di[diOffIno:]) != ino {
			t.Fatalf("inode %d does not know its own number", ino)
		}
		if binary.BigEndian.Uint32(di[diOffNextUnlinked:]) != ^uint32(0) {
			t.Fatalf("inode %d has a next-unlinked pointer", ino)
		}
		// An attribute format of zero means "a device", and xfs_repair
		// says so about every inode that has one.
		if di[diOffAformat] != fmtExtents {
			t.Fatalf("inode %d has attribute format %d", ino, di[diOffAformat])
		}
	}
}

func TestTheRootIsAnEmptyDirectory(t *testing.T) {
	dev, l := formatted(t, 64*mib)
	block, slot := l.InodeBlock(l.RootIno)
	at := int(block)*4096 + int(slot)*int(l.InodeSize)
	di := dev.data[at : at+int(l.InodeSize)]
	if binary.BigEndian.Uint16(di[diOffMode:]) != modeDir {
		t.Errorf("the root's mode is %o", binary.BigEndian.Uint16(di[diOffMode:]))
	}
	if di[diOffFormat] != fmtLocal {
		t.Error("an empty directory is held inside its own inode")
	}
	if binary.BigEndian.Uint64(di[diOffSize:]) != 6 {
		t.Errorf("the root's size is %d, want 6 (a header and no entries)",
			binary.BigEndian.Uint64(di[diOffSize:]))
	}
	if binary.BigEndian.Uint32(di[diOffNlink:]) != 2 {
		t.Error("a directory's link count starts at two")
	}
	sf := di[diCoreSize:]
	if sf[0] != 0 || sf[1] != 0 {
		t.Error("the root should have no entries")
	}
	if binary.BigEndian.Uint32(sf[2:]) != uint32(l.RootIno) {
		t.Error("the root's parent should be itself")
	}
	// The two realtime inodes exist as empty files.
	for _, ino := range []uint64{l.RbmIno, l.RsumIno} {
		b, s := l.InodeBlock(ino)
		rt := dev.data[int(b)*4096+int(s)*int(l.InodeSize):]
		if binary.BigEndian.Uint16(rt[diOffMode:]) != modeFile {
			t.Errorf("inode %d should be a regular file", ino)
		}
	}
}

// The log must say the filesystem was put down cleanly, or nothing will
// mount it.
func TestTheLogIsClean(t *testing.T) {
	dev, l := formatted(t, 64*mib)
	log := dev.data[l.DeviceBlock(l.LogAg, l.LogStart)*4096:]
	if binary.BigEndian.Uint32(log) != logMagic {
		t.Fatal("the log has no record")
	}
	if binary.BigEndian.Uint32(log[logRecCycle:]) != 1 {
		t.Error("the log's cycle should be 1")
	}
	if binary.BigEndian.Uint32(log[logRecNumOps:]) != 1 {
		t.Error("the unmount record carries one operation")
	}
	op := log[logSector:]
	if op[logOpClientID] != logClientLog {
		t.Error("the operation is the filesystem's own")
	}
	if op[logOpFlags]&logUnmountTrans == 0 {
		t.Error("the operation is not the unmount transaction")
	}
	if binary.BigEndian.Uint16(op[logOpHdrSize:]) != logUnmountMagic {
		t.Error("the unmount payload's magic is wrong")
	}
	// The record's second block holds the cycle in its first word, which is
	// how a reader finds the head of the log.
	if binary.BigEndian.Uint32(op) != 1 {
		t.Error("the data block does not carry the cycle number")
	}
	if binary.BigEndian.Uint32(log[logRecCycleData:]) != logUnmountTID {
		t.Error("the word the cycle displaced was not kept in the header")
	}
	if binary.BigEndian.Uint32(log[logRecSize:]) != logIclogSize {
		t.Error("the record's declared size is wrong")
	}
	// The rest of the log is zeroed, so nothing newer can be found in it.
	start := l.DeviceBlock(l.LogAg, l.LogStart) * 4096
	rest := dev.data[start+2*logSector : start+uint64(l.LogBlocks)*4096]
	for i, b := range rest {
		if b != 0 {
			t.Fatalf("the log is not zeroed at byte %d", i+2*logSector)
		}
	}
}

func TestFormatRefusesADeviceItCannotUse(t *testing.T) {
	if _, err := Format(newMemDevice(mib), Defaults(), fixedRandom{}); !errors.Is(err, ErrTooSmall) {
		t.Errorf("a 1 MiB device: %v", err)
	}
	bad := Options{BlockSize: 1024, SectorSize: 512, InodeSize: 512}
	if _, err := Format(newMemDevice(64*mib), bad, fixedRandom{}); !errors.Is(err, ErrGeometry) {
		t.Errorf("a 1 KiB block size: %v", err)
	}
}

// A device that fails part way through must report it, not leave a
// half-formatted filesystem looking finished.
func TestAFailingDeviceIsReported(t *testing.T) {
	dev := newMemDevice(64 * mib)
	dev.failAt = 4096 * 20
	if _, err := Format(dev, Defaults(), fixedRandom{}); err == nil {
		t.Fatal("a failing write was not reported")
	}
	if binary.BigEndian.Uint32(dev.data[:4]) == sbMagic {
		t.Error("the superblock was written although the format failed")
	}
}

// Formatting twice with the same UUID must give the same bytes: nothing
// here may depend on the time or on where it ran.
func TestFormattingIsDeterministic(t *testing.T) {
	first, _ := formatted(t, 64*mib)
	second, _ := formatted(t, 64*mib)
	if !bytes.Equal(first.data, second.data) {
		t.Error("two formats of the same device differ")
	}
}

func BenchmarkFormat(b *testing.B) {
	for b.Loop() {
		dev := newMemDevice(64 * mib)
		if _, err := Format(dev, Defaults(), fixedRandom{}); err != nil {
			b.Fatal(err)
		}
	}
}
