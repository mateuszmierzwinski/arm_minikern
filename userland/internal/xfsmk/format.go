package xfsmk

import (
	"crypto/rand"
	"fmt"
)

// Format writes a filesystem over the whole device.
//
// The order is the order a reader would need it in if it were interrupted:
// the log and the inodes first, the metadata that points at them next, and
// the superblock last, because a superblock is what makes the rest
// findable. Then one Sync, which on /dev/nbd0 is what sends the buffer
// cache to the server.
func Format(dev Device, opts Options, rng Random) (Layout, error) {
	size, err := dev.Size()
	if err != nil {
		return Layout{}, fmt.Errorf("device size: %w", err)
	}
	l, err := Plan(size, opts)
	if err != nil {
		return Layout{}, err
	}
	if err := l.pickUUID(rng); err != nil {
		return Layout{}, err
	}
	for _, step := range []struct {
		what string
		run  func(Device, *Layout) error
	}{
		{"wiping the start", wipe},
		{"the log", writeLog},
		{"the inodes", writeInodes},
		{"the btrees", writeBtrees},
		{"the headers", writeHeaders},
	} {
		if err := step.run(dev, &l); err != nil {
			return Layout{}, fmt.Errorf("%s: %w", step.what, err)
		}
	}
	if err := dev.Sync(); err != nil {
		return Layout{}, fmt.Errorf("sync: %w", err)
	}
	return l, nil
}

// pickUUID fills the filesystem's identity when the caller gave none.
func (l *Layout) pickUUID(rng Random) error {
	if l.UUID != [16]byte{} {
		return nil
	}
	if rng == nil {
		rng = rand.Reader
	}
	if _, err := rng.Read(l.UUID[:]); err != nil {
		return fmt.Errorf("uuid: %w", err)
	}
	// The variant and version bits of a random (version 4) UUID.
	l.UUID[6] = l.UUID[6]&0x0f | 0x40
	l.UUID[8] = l.UUID[8]&0x3f | 0x80
	return nil
}

// wipeBytes is how much of the start is zeroed before anything is written.
// It is not about this filesystem — it is about the one that may have been
// here before, whose secondary superblocks would otherwise still be found
// by a repair tool and believed.
const wipeBytes = 1 << 20

func wipe(dev Device, l *Layout) error {
	zero := make([]byte, l.BlockSize)
	end := uint64(wipeBytes) / uint64(l.BlockSize)
	if end > l.Blocks {
		end = l.Blocks
	}
	for block := uint64(0); block < end; block++ {
		if err := writeBlock(dev, l, block, zero); err != nil {
			return err
		}
	}
	// And the start of every group, where a previous filesystem with a
	// different group size would have left a superblock of its own.
	for i := uint32(0); i < l.AgCount; i++ {
		if err := writeBlock(dev, l, l.DeviceBlock(i, headerBlock), zero); err != nil {
			return err
		}
	}
	return nil
}

// writeBlock puts one whole block at its place on the device.
func writeBlock(dev Device, l *Layout, block uint64, buf []byte) error {
	off := int64(block) * int64(l.BlockSize)
	n, err := dev.WriteAt(buf, off)
	if err != nil {
		return err
	}
	if n != len(buf) {
		return fmt.Errorf("short write of %d bytes at block %d", n, block)
	}
	return nil
}

// blockOf is a zeroed buffer of one block, for a writer to fill.
func (l *Layout) blockOf() []byte {
	return make([]byte, l.BlockSize)
}
