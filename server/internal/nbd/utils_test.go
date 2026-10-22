package nbd

import (
	"errors"
	"fmt"
	"io"
	"testing"
)

// A volume that is nothing but its geometry, for the transforms that only
// read it.
type sizedVolume struct {
	ramVolume
}

func TestVerb(t *testing.T) {
	if got := verb(ExportInfo{}); got != "served" {
		t.Errorf("a writable export: got %q, want %q", got, "served")
	}
	if got := verb(ExportInfo{ReadOnly: true}); got != "read" {
		t.Errorf("a read-only export: got %q, want %q", got, "read")
	}
}

func TestNbdError(t *testing.T) {
	cases := []struct {
		name string
		err  error
		want uint32
	}{
		{"no error", nil, errNone},
		{"out of range", ErrRange, errInvalid},
		{"wrapped range", fmt.Errorf("context: %w", ErrRange), errInvalid},
		{"read-only", ErrReadOnly, errReadOnly},
		{"wrapped read-only", fmt.Errorf("context: %w", ErrReadOnly), errReadOnly},
		{"short write", io.ErrShortWrite, errNoSpace},
		{"anything else", errors.New("disk on fire"), errIO},
		{"protocol", ErrProtocol, errIO},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			if got := nbdError(c.err); got != c.want {
				t.Errorf("nbdError(%v) = %d, want %d", c.err, got, c.want)
			}
		})
	}
}

func TestInfoFor(t *testing.T) {
	vol := newRAMVolume(4096, false)
	got := infoFor("vol", vol)
	if got.Name != "vol" || got.Size != 4096 || got.ReadOnly {
		t.Errorf("infoFor: got %+v", got)
	}
	ro := infoFor("", newRAMVolume(512, true))
	if !ro.ReadOnly || ro.Size != 512 {
		t.Errorf("infoFor read-only: got %+v", ro)
	}
	if flags := ro.TransmissionFlags(); flags&FlagReadOnly == 0 || flags&FlagHasFlags == 0 {
		t.Errorf("read-only flags: got %04x", flags)
	}
	if flags := got.TransmissionFlags(); flags&FlagReadOnly != 0 || flags&FlagSendFlush == 0 {
		t.Errorf("writable flags: got %04x", flags)
	}
}

func TestChunkOf(t *testing.T) {
	buf := make([]byte, 100)
	cases := []struct {
		left, want uint32
	}{
		{0, 0},
		{1, 1},
		{99, 99},
		{100, 100},
		{101, 100},
		{1 << 20, 100},
	}
	for _, c := range cases {
		if got := chunkOf(c.left, buf); got != c.want {
			t.Errorf("chunkOf(%d, [100]) = %d, want %d", c.left, got, c.want)
		}
	}
	if got := chunkOf(5, nil); got != 0 {
		t.Errorf("chunkOf with no buffer = %d, want 0", got)
	}
}

func BenchmarkVerb(b *testing.B) {
	info := ExportInfo{Name: "vol"}
	for b.Loop() {
		_ = verb(info)
	}
}

func BenchmarkNbdError(b *testing.B) {
	err := fmt.Errorf("wrapped: %w", ErrRange)
	for b.Loop() {
		_ = nbdError(err)
	}
}

func BenchmarkInfoFor(b *testing.B) {
	vol := newRAMVolume(1<<20, false)
	for b.Loop() {
		_ = infoFor("vol", vol)
	}
}

func BenchmarkChunkOf(b *testing.B) {
	buf := make([]byte, chunkBytes)
	for b.Loop() {
		_ = chunkOf(1<<20, buf)
	}
}
