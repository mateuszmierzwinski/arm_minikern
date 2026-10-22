// nbdtest is the init that proves /dev/nbd0 works: a Go program on minikern
// reading and writing a volume that lives as a file on a server, over this
// kernel's own TCP and its own NBD client.
//
// The host side (test/nbd_qemu_test.sh) puts a known pattern in the image
// before the boot and checks what this program wrote after it, so the proof
// is byte-exact at both ends rather than "it printed PASS".
package main

import (
	"bytes"
	"fmt"
	"io"
	"os"
)

const (
	device = "/dev/nbd0"

	// What the host puts there before the boot.
	header      = "MINIKERN-NBD-VOLUME-V1"
	patternOff  = 1 << 20
	patternLen  = 64 << 10
	patternByte = 0xa5

	// What this program writes, for the host to find afterwards.
	writeOff  = 2 << 20
	writeLen  = 128 << 10
	writeByte = 0x5c
	markOff   = 3 << 20
	mark      = "written-by-minikern"
)

// check reports one named result and remembers the first failure.
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
	f, err := os.OpenFile(device, os.O_RDWR, 0)
	if err != nil {
		fmt.Printf("FAIL open: %v\n", err)
		os.Exit(1)
	}
	defer func() { _ = f.Close() }()

	size, err := f.Seek(0, io.SeekEnd)
	check("size", err)
	fmt.Printf("volume: %d bytes\n", size)

	check("header", readHeader(f))
	check("pattern", readPattern(f))
	check("cached-reread", readPattern(f))
	check("unaligned-span", unalignedSpan(f))
	check("write", writeRegion(f))
	check("read-back", readBack(f))
	check("past-the-end", pastTheEnd(f, size))
	check("sync", f.Sync())

	if failed {
		fmt.Println("NBD FAILURES")
		os.Exit(1)
	}
	fmt.Println("ALL NBD PASS")
}

// readHeader reads what the host wrote at offset 0.
func readHeader(f *os.File) error {
	buf := make([]byte, len(header))
	if _, err := f.ReadAt(buf, 0); err != nil {
		return err
	}
	if string(buf) != header {
		return fmt.Errorf("got %q, want %q", buf, header)
	}
	return nil
}

// readPattern checks the region the host filled with one byte. Reading it
// twice is how the buffer cache gets exercised: the second read must give
// the same bytes without going back to the server.
func readPattern(f *os.File) error {
	buf := make([]byte, patternLen)
	if _, err := f.ReadAt(buf, patternOff); err != nil {
		return err
	}
	for i, b := range buf {
		if b != patternByte {
			return fmt.Errorf("byte %d of the pattern is %#02x, want %#02x", i, b, patternByte)
		}
	}
	return nil
}

// unalignedSpan reads across a block boundary at an offset that is not a
// multiple of anything, which is where a read-modify-write bug shows up.
func unalignedSpan(f *os.File) error {
	const off = patternOff - 3
	buf := make([]byte, 4096+7)
	if _, err := f.ReadAt(buf, off); err != nil {
		return err
	}
	// The three bytes before the pattern are not the pattern byte, and
	// everything from the pattern's start is.
	for i := 3; i < len(buf); i++ {
		if buf[i] != patternByte {
			return fmt.Errorf("byte %d (offset %d) is %#02x, want the pattern", i, off+int64(i), buf[i])
		}
	}
	return nil
}

// neighbours holds the bytes either side of the write, read before it
// happens.
//
// Checking that they are *unchanged* is the only sound way to prove a write
// stayed inside its range: looking for "not the byte we wrote" fails once
// in eight runs on a volume full of random bytes, because some of those
// bytes are the one we wrote.
var neighbours struct {
	before [32]byte
	after  [32]byte
}

// writeRegion notes what surrounds the range, writes a block of one byte
// and a string, then syncs, so the host can find both in its file.
func writeRegion(f *os.File) error {
	if _, err := f.ReadAt(neighbours.before[:], writeOff-int64(len(neighbours.before))); err != nil {
		return err
	}
	if _, err := f.ReadAt(neighbours.after[:], writeOff+writeLen); err != nil {
		return err
	}
	block := bytes.Repeat([]byte{writeByte}, writeLen)
	if _, err := f.WriteAt(block, writeOff); err != nil {
		return err
	}
	if _, err := f.WriteAt([]byte(mark), markOff); err != nil {
		return err
	}
	return f.Sync()
}

// readBack reads what writeRegion wrote, which must come back the same
// whether it is still in the cache or not.
func readBack(f *os.File) error {
	tail := make([]byte, 32)
	if _, err := f.ReadAt(tail, writeOff+writeLen-int64(len(tail))); err != nil {
		return err
	}
	for i, b := range tail {
		if b != writeByte {
			return fmt.Errorf("the tail of the write is %#02x at %d", b, i)
		}
	}
	// Either side of the write, the volume must be exactly as it was.
	if err := unchanged(f, writeOff-int64(len(neighbours.before)), neighbours.before[:]); err != nil {
		return fmt.Errorf("before the write: %w", err)
	}
	if err := unchanged(f, writeOff+writeLen, neighbours.after[:]); err != nil {
		return fmt.Errorf("after the write: %w", err)
	}
	marked := make([]byte, len(mark))
	if _, err := f.ReadAt(marked, markOff); err != nil {
		return err
	}
	if string(marked) != mark {
		return fmt.Errorf("the mark reads back as %q", marked)
	}
	return nil
}

// unchanged reads a range and compares it with what was there before.
func unchanged(f *os.File, off int64, want []byte) error {
	got := make([]byte, len(want))
	if _, err := f.ReadAt(got, off); err != nil {
		return err
	}
	for i := range want {
		if got[i] != want[i] {
			return fmt.Errorf("byte %d of %d changed from %#02x to %#02x", i, len(want), want[i], got[i])
		}
	}
	return nil
}

// pastTheEnd checks a block device does not pretend to grow.
func pastTheEnd(f *os.File, size int64) error {
	buf := make([]byte, 16)
	if n, err := f.ReadAt(buf, size); err == nil || n != 0 {
		return fmt.Errorf("a read at the end gave %d bytes, %v", n, err)
	}
	if _, err := f.WriteAt(buf, size); err == nil {
		return fmt.Errorf("a write past the end succeeded")
	}
	return nil
}
