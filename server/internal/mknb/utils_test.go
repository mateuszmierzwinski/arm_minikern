package mknb

import (
	"strings"
	"testing"
)

func TestBoardKeyIsHexOnly(t *testing.T) {
	board := BoardID{MAC: [6]byte{0x2e, 0x2e, 0x2f, 0, 0, 0}}
	copy(board.ID[:], "../../etc/passwd")
	key := board.Key()
	if strings.ContainsAny(key, "./\\") {
		t.Fatalf("Key() passed through path characters: %q", key)
	}
	if len(key) != 2*MACLen+1+2*BoardIDLen {
		t.Fatalf("Key() is %d characters: %q", len(key), key)
	}
}

func TestMACString(t *testing.T) {
	board := BoardID{MAC: [6]byte{0x02, 0x64, 0xb7, 0x00, 0x81, 0x11}}
	if got := board.MACString(); got != "02:64:b7:00:81:11" {
		t.Fatalf("got %q", got)
	}
}

func TestNewImageHashesItsData(t *testing.T) {
	img := NewImage("a.tar", []byte("abc"))
	// The SHA-256 of "abc", from FIPS 180-4.
	const want = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
	if img.DigestString() != want {
		t.Fatalf("got %s", img.DigestString())
	}
	if img.Name != "a.tar" || len(img.Data) != 3 {
		t.Fatalf("got %+v", img)
	}
}

func TestClampWindow(t *testing.T) {
	cases := map[uint16]uint32{0: 1, 1: 1, 64: 64, 65: MaxWindow, 65535: MaxWindow}
	for in, want := range cases {
		if got := clampWindow(in); got != want {
			t.Fatalf("clampWindow(%d) = %d, want %d", in, got, want)
		}
	}
}

func TestBlockRange(t *testing.T) {
	img := NewImage("i", make([]byte, 5000)) // 5 blocks
	cases := []struct {
		first      uint32
		count      uint16
		from, upto uint32
	}{
		{0, 64, 0, 5},
		{2, 64, 2, 5},
		{0, 2, 0, 2},
		{4, 1, 4, 5},
		{5, 64, 0, 0},      // past the end
		{1 << 20, 1, 0, 0}, // far past the end
	}
	for _, c := range cases {
		from, upto := blockRange(img, c.first, c.count)
		if from != c.from || upto != c.upto {
			t.Fatalf("blockRange(%d, %d) = (%d, %d), want (%d, %d)", c.first, c.count, from, upto,
				c.from, c.upto)
		}
	}
}

func BenchmarkBoardKey(b *testing.B) {
	board := testBoard()
	for b.Loop() {
		_ = board.Key()
	}
}

func BenchmarkMACString(b *testing.B) {
	board := testBoard()
	for b.Loop() {
		_ = board.MACString()
	}
}

func BenchmarkNewImage(b *testing.B) {
	data := make([]byte, 1<<20)
	b.SetBytes(int64(len(data)))
	for b.Loop() {
		_ = NewImage("i", data)
	}
}

func BenchmarkClampWindow(b *testing.B) {
	for b.Loop() {
		_ = clampWindow(100)
	}
}

func BenchmarkBlockRange(b *testing.B) {
	img := NewImage("i", make([]byte, 1<<20))
	for b.Loop() {
		_, _ = blockRange(img, 7, 64)
	}
}

func BenchmarkDigestString(b *testing.B) {
	img := NewImage("i", []byte("abc"))
	for b.Loop() {
		_ = img.DigestString()
	}
}
