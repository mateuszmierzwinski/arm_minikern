// xfstest is the init that proves the whole storage path: a Go program on
// minikern formats an XFS filesystem on a volume that lives as a file on a
// server, over this kernel's own TCP and its own NBD client, and then asks
// the kernel to mount it and reads it back.
//
// The host side (test/xfs_qemu_test.sh) then runs **xfs_repair** on that
// file. So the claim this test makes is not "it printed PASS": it is that a
// filesystem this board wrote over the network is one Linux calls valid.
package main

import (
	"bytes"
	"fmt"
	"os"
	"syscall"

	"github.com/mateuszmierzwinski/arm_minikern/userland/internal/xfsmk"
)

const (
	device = "/dev/nbd0"
	mount  = "/mnt"
)

var failed bool

func check(name string, err error) {
	if err != nil {
		failed = true
		fmt.Printf("FAIL %s: %v\n", name, err)
		return
	}
	fmt.Printf("PASS %s\n", name)
}

// The two things this program is asked to do. The test runs it twice: once
// on a volume it formats itself, and once on a filesystem the host made
// with a file already in it, which is the only way to write to anything
// (creating a file needs the log).
const (
	modeEnv    = "MINIKERN_XFS_MODE"
	modeFormat = "format"
	modeWrite  = "write"
)

func main() {
	switch os.Getenv(modeEnv) {
	case modeWrite:
		writePhase()
	default:
		formatPhase()
	}
	if failed {
		fmt.Println("XFS FAILURES")
		os.Exit(1)
	}
	fmt.Println("ALL XFS PASS")
}

// formatPhase makes a filesystem on the volume and reads it back.
func formatPhase() {
	check("format", format())
	check("mount", mountVolume(syscall.MS_RDONLY))
	check("list", list())
	check("read-only", readOnly())
	check("mount-twice", mountTwice())
	check("bad-fstype", badFstype())
}

// writePhase writes into a file the host put there, which is what a board
// with somewhere to put its data actually does.
func writePhase() {
	check("mount-rw", mountVolume(0))
	check("read-existing", readExisting())
	check("write-data", writeData())
	check("read-back", readBack())
	check("create-file", createFile())
	check("list-after-create", listAfterCreate())
	check("create-twice", createTwice())
	check("grow-into-own-block", growIntoOwnBlock())
	check("refuse-grow", refuseGrow())
}

// The file the host made, and what this writes into it.
const (
	target    = mount + "/data.bin"
	writeAt   = int64(4096)
	writeByte = 0x7e
	writeLen  = 40000
	// The host made the file 262000 bytes, which is not a whole number of
	// 4096-byte blocks: its last block has 144 bytes of room that the file
	// already owns. Growing into that room changes the recorded size and
	// nothing else, which is the write the log makes possible.
	grownBy = 100
)

// readExisting proves the file is the one the host made before anything is
// written over it.
func readExisting() error {
	info, err := os.Stat(target)
	if err != nil {
		return err
	}
	fmt.Printf("%s is %d bytes\n", target, info.Size())
	if info.Size() < writeAt+writeLen {
		return fmt.Errorf("the file is too small to write into: %d bytes", info.Size())
	}
	return nil
}

// neighbours holds the bytes on either side of the range about to be
// written, read before the write happens.
//
// They are **compared**, not inspected for the written byte: the file is
// random, so asserting that sixteen random bytes do not happen to contain
// 0x7e is a test that fails about one run in sixteen for no reason — which
// it duly did. What matters is that they did not change.
var neighbours struct {
	before [16]byte
	after  [16]byte
}

func recordNeighbours(f *os.File) error {
	if _, err := f.ReadAt(neighbours.before[:], writeAt-int64(len(neighbours.before))); err != nil {
		return err
	}
	_, err := f.ReadAt(neighbours.after[:], writeAt+writeLen)
	return err
}

func writeData() error {
	f, err := os.OpenFile(target, os.O_RDWR, 0)
	if err != nil {
		return err
	}
	defer func() { _ = f.Close() }()
	if err = recordNeighbours(f); err != nil {
		return err
	}
	block := bytes.Repeat([]byte{writeByte}, writeLen)
	if _, err := f.WriteAt(block, writeAt); err != nil {
		return err
	}
	return f.Sync()
}

// readBack checks what was written is there, and that its neighbours are
// not: a write that spilled would be worse than one that failed.
func readBack() error {
	f, err := os.Open(target)
	if err != nil {
		return err
	}
	defer func() { _ = f.Close() }()
	const edge = 16
	buf := make([]byte, writeLen+2*edge)
	if _, err := f.ReadAt(buf, writeAt-edge); err != nil {
		return err
	}
	if !bytes.Equal(buf[:edge], neighbours.before[:]) {
		return fmt.Errorf("the write changed the bytes before its start")
	}
	for i := edge; i < edge+writeLen; i++ {
		if buf[i] != writeByte {
			return fmt.Errorf("byte %d of the write is %#02x", i-edge, buf[i])
		}
	}
	if !bytes.Equal(buf[edge+writeLen:], neighbours.after[:]) {
		return fmt.Errorf("the write changed the bytes past its end")
	}
	return nil
}

// refuseGrow checks the line this kernel still draws: a write may reach the
// end of the blocks a file already owns, and no further, because growing
// past them needs the block allocator.
// The file this creates, and what it writes into it.
const created = mount + "/made-on-the-board"

// createFile makes a file in the mounted filesystem. It is the allocator's
// whole claim: an inode comes out of the inode btrees and an entry appears
// in the parent's directory, and the host's xfs_repair is what judges it.
func createFile() error {
	f, err := os.OpenFile(created, os.O_RDWR|os.O_CREATE, 0o644)
	if err != nil {
		return err
	}
	defer func() { _ = f.Close() }()
	info, err := f.Stat()
	if err != nil {
		return err
	}
	if info.Size() != 0 {
		return fmt.Errorf("a new file is %d bytes", info.Size())
	}
	return f.Sync()
}

// listAfterCreate proves the entry is really in the directory, by reading
// it back the ordinary way rather than by trusting the create.
func listAfterCreate() error {
	names, err := os.ReadDir(mount)
	if err != nil {
		return err
	}
	for _, e := range names {
		if e.Name() == "made-on-the-board" {
			return nil
		}
	}
	return fmt.Errorf("the new file is not in the listing")
}

// createTwice must fail: O_EXCL means what it says, and a directory may not
// hold two entries of one name.
func createTwice() error {
	f, err := os.OpenFile(created, os.O_RDWR|os.O_CREATE|os.O_EXCL, 0o644)
	if err == nil {
		_ = f.Close()
		return fmt.Errorf("creating the same name twice succeeded")
	}
	return nil
}

// growIntoOwnBlock writes past the end of the file but inside the last
// block it already owns. The size must grow, and the bytes must read back —
// this is the one metadata change a data write makes, and it goes through
// the log (core/xfs/xfs_logwrite.c).
func growIntoOwnBlock() error {
	f, err := os.OpenFile(target, os.O_RDWR, 0)
	if err != nil {
		return err
	}
	defer func() { _ = f.Close() }()
	info, err := f.Stat()
	if err != nil {
		return err
	}
	was := info.Size()
	want := make([]byte, grownBy)
	for i := range want {
		want[i] = byte('g')
	}
	if _, err = f.WriteAt(want, was); err != nil {
		return fmt.Errorf("growing into the file's own last block: %w", err)
	}
	if info, err = f.Stat(); err != nil {
		return err
	}
	if info.Size() != was+grownBy {
		return fmt.Errorf("size is %d, want %d", info.Size(), was+grownBy)
	}
	got := make([]byte, grownBy)
	if _, err = f.ReadAt(got, was); err != nil {
		return err
	}
	if string(got) != string(want) {
		return fmt.Errorf("the grown part reads back wrong")
	}
	return nil
}

func refuseGrow() error {
	f, err := os.OpenFile(target, os.O_RDWR, 0)
	if err != nil {
		return err
	}
	defer func() { _ = f.Close() }()
	info, err := f.Stat()
	if err != nil {
		return err
	}
	// Past the end of the blocks the file owns, which needs the allocator
	// and must still be refused.
	if _, err := f.WriteAt(make([]byte, 4096), info.Size()); err == nil {
		return fmt.Errorf("a write past the file's own blocks succeeded")
	}
	return nil
}

// format writes a filesystem on the network block device, with the same
// code the build machine's mkfs uses.
func format() error {
	f, err := os.OpenFile(device, os.O_RDWR, 0)
	if err != nil {
		return err
	}
	defer func() { _ = f.Close() }()
	layout, err := xfsmk.Format(&fileDevice{f: f}, xfsmk.Defaults(), nil)
	if err != nil {
		return err
	}
	fmt.Printf("formatted: %d blocks, %d groups, root inode %d, log %d blocks in group %d\n",
		layout.Blocks, layout.AgCount, layout.RootIno, layout.LogBlocks, layout.LogAg)
	return nil
}

// mountVolume asks the kernel to mount what was just written. A kernel that
// could not read its own filesystem would fail here.
func mountVolume(flags uintptr) error {
	if err := os.Mkdir(mount, 0o755); err != nil && !os.IsExist(err) {
		// The root filesystem is read-only, so the mount point is
		// expected to be there already; a missing one is the real error.
		_ = err
	}
	return syscall.Mount(device, mount, "xfs", flags, "")
}

// list reads the new filesystem's root, which must be empty.
func list() error {
	entries, err := os.ReadDir(mount)
	if err != nil {
		return err
	}
	for _, e := range entries {
		fmt.Printf("entry: %s dir=%v\n", e.Name(), e.IsDir())
	}
	if len(entries) != 0 {
		return fmt.Errorf("a new filesystem has %d entries, want none", len(entries))
	}
	info, err := os.Stat(mount)
	if err != nil {
		return err
	}
	if !info.IsDir() {
		return fmt.Errorf("%s is not a directory", mount)
	}
	return nil
}

// readOnly checks the mount refuses to be written: this kernel cannot
// journal, so it must not change a filesystem.
func readOnly() error {
	f, err := os.OpenFile(mount+"/newfile", os.O_CREATE|os.O_WRONLY, 0o644)
	if err == nil {
		_ = f.Close()
		return fmt.Errorf("a read-only filesystem accepted a new file")
	}
	return nil
}

// mountTwice checks the second mount is refused rather than replacing the
// first one underneath whoever is using it.
func mountTwice() error {
	err := syscall.Mount(device, mount, "xfs", syscall.MS_RDONLY, "")
	if err == nil {
		return fmt.Errorf("mounting twice succeeded")
	}
	fmt.Printf("the second mount was refused: %v\n", err)
	return nil
}

// badFstype checks the kernel refuses what it cannot mount by name, rather
// than trying and failing somewhere deeper.
func badFstype() error {
	if err := syscall.Mount(device, "/mnt2", "ext4", syscall.MS_RDONLY, ""); err == nil {
		return fmt.Errorf("ext4 was accepted")
	}
	if err := syscall.Mount("/dev/null", "/mnt2", "xfs", syscall.MS_RDONLY, ""); err == nil {
		return fmt.Errorf("mounting /dev/null as xfs was accepted")
	}
	return nil
}

// fileDevice adapts an open file to what the formatter needs.
type fileDevice struct {
	f *os.File
}

func (d *fileDevice) WriteAt(p []byte, off int64) (int, error) { return d.f.WriteAt(p, off) }
func (d *fileDevice) ReadAt(p []byte, off int64) (int, error)  { return d.f.ReadAt(p, off) }
func (d *fileDevice) Sync() error                              { return d.f.Sync() }

func (d *fileDevice) Size() (int64, error) {
	info, err := d.f.Stat()
	if err != nil {
		return 0, err
	}
	return info.Size(), nil
}
