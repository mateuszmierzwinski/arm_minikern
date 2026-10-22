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

func main() {
	check("format", format())
	check("mount", mountVolume())
	check("list", list())
	check("read-only", readOnly())
	check("mount-twice", mountTwice())
	check("bad-fstype", badFstype())
	if failed {
		fmt.Println("XFS FAILURES")
		os.Exit(1)
	}
	fmt.Println("ALL XFS PASS")
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
func mountVolume() error {
	if err := os.Mkdir(mount, 0o755); err != nil && !os.IsExist(err) {
		// The root filesystem is read-only, so the mount point is
		// expected to be there already; a missing one is the real error.
		_ = err
	}
	return syscall.Mount(device, mount, "xfs", syscall.MS_RDONLY, "")
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
