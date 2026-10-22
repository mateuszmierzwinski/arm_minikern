package main

import (
	"fmt"
	"os"
)

// fileDevice is the adapter from a file descriptor to what xfsmk needs. It
// is the same for an image file on the build machine and for /dev/nbd0 on
// the board, which is why the formatter can be tested without a board.
type fileDevice struct {
	f *os.File
}

func openDevice(path string) (*fileDevice, error) {
	f, err := os.OpenFile(path, os.O_RDWR, 0)
	if err != nil {
		return nil, fmt.Errorf("open %s: %w", path, err)
	}
	return &fileDevice{f: f}, nil
}

func (d *fileDevice) WriteAt(p []byte, off int64) (int, error) { return d.f.WriteAt(p, off) }
func (d *fileDevice) ReadAt(p []byte, off int64) (int, error)  { return d.f.ReadAt(p, off) }
func (d *fileDevice) Sync() error                              { return d.f.Sync() }
func (d *fileDevice) Close() error                             { return d.f.Close() }

// Size asks the file how big it is, which works for both a file and
// /dev/nbd0: the kernel reports a block device's size through fstat
// (kernel/devfs.c).
func (d *fileDevice) Size() (int64, error) {
	info, err := d.f.Stat()
	if err != nil {
		return 0, err
	}
	return info.Size(), nil
}
