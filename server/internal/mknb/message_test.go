package mknb

import (
	"bytes"
	"encoding/binary"
	"testing"
)

// discoverBytes builds a DISCOVER as the kernel sends it.
func discoverBytes(xid uint32, board BoardID) []byte {
	p := make([]byte, HeaderLen+DiscoverBody)
	encodeHeader(p, TypeDiscover, xid, 0)
	copy(p[HeaderLen:], board.MAC[:])
	copy(p[HeaderLen+MACLen:], board.ID[:])
	return p
}

// readBytes builds a READ as the kernel sends it.
func readBytes(xid, first uint32, count uint16, token uint32) []byte {
	p := make([]byte, HeaderLen+ReadBody)
	encodeHeader(p, TypeRead, xid, first)
	binary.BigEndian.PutUint16(p[HeaderLen:], count)
	binary.BigEndian.PutUint32(p[HeaderLen+4:], token)
	return p
}

func testBoard() BoardID {
	b := BoardID{MAC: [6]byte{2, 0x4d, 0x4b, 1, 2, 3}}
	for i := range b.ID {
		b.ID[i] = byte(i + 1)
	}
	return b
}

func TestParseHeaderAcceptsOurMessages(t *testing.T) {
	h, err := ParseHeader(discoverBytes(0x11223344, testBoard()))
	if err != nil {
		t.Fatalf("ParseHeader: %v", err)
	}
	if h.Type != TypeDiscover || h.XID != 0x11223344 || h.Arg != 0 {
		t.Fatalf("got %+v", h)
	}
}

func TestParseHeaderRejectsForeignMessages(t *testing.T) {
	good := discoverBytes(1, testBoard())
	cases := []struct {
		name string
		edit func([]byte) []byte
		want error
	}{
		{"short", func(p []byte) []byte { return p[:HeaderLen-1] }, ErrShort},
		{"empty", func([]byte) []byte { return nil }, ErrShort},
		{"magic", func(p []byte) []byte { p[0] ^= 0xff; return p }, ErrMagic},
		{"version", func(p []byte) []byte { p[4] = 99; return p }, ErrVersion},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			p := append([]byte(nil), good...)
			if _, err := ParseHeader(c.edit(p)); err != c.want {
				t.Fatalf("got %v, want %v", err, c.want)
			}
		})
	}
}

func TestParseDiscoverReadsTheBoard(t *testing.T) {
	board := testBoard()
	p := discoverBytes(7, board)
	h, _ := ParseHeader(p)
	d, err := ParseDiscover(p, h)
	if err != nil {
		t.Fatalf("ParseDiscover: %v", err)
	}
	if d.Board != board {
		t.Fatalf("got %+v, want %+v", d.Board, board)
	}
	if _, err := ParseDiscover(p[:HeaderLen+4], h); err != ErrBody {
		t.Fatalf("a truncated body gave %v", err)
	}
}

func TestParseReadReadsTheWindow(t *testing.T) {
	p := readBytes(5, 128, 64, 0xdeadbeef)
	h, _ := ParseHeader(p)
	r, err := ParseRead(p, h)
	if err != nil {
		t.Fatalf("ParseRead: %v", err)
	}
	if r.Arg != 128 || r.Count != 64 || r.Token != 0xdeadbeef {
		t.Fatalf("got %+v", r)
	}
	if _, err := ParseRead(p[:HeaderLen+2], h); err != ErrBody {
		t.Fatalf("a truncated body gave %v", err)
	}
}

// The OFFER's layout is the ABI the kernel parses: check it byte by byte.
func TestEncodeOfferLayout(t *testing.T) {
	img := NewImage("root.tar", bytes.Repeat([]byte{0xab}, 3000))
	p, err := EncodeOffer(0x01020304, img, 0xcafef00d)
	if err != nil {
		t.Fatalf("EncodeOffer: %v", err)
	}
	if len(p) != HeaderLen+OfferBody {
		t.Fatalf("offer is %d bytes, want %d", len(p), HeaderLen+OfferBody)
	}
	if binary.BigEndian.Uint32(p) != Magic || p[4] != Version || MessageType(p[5]) != TypeOffer {
		t.Fatal("header is wrong")
	}
	if binary.BigEndian.Uint32(p[8:]) != 0x01020304 || binary.BigEndian.Uint32(p[12:]) != 3000 {
		t.Fatal("xid or length is wrong")
	}
	if binary.BigEndian.Uint16(p[HeaderLen:]) != BlockSize {
		t.Fatal("block size is wrong")
	}
	if !bytes.Equal(p[HeaderLen+4:HeaderLen+36], img.Digest[:]) {
		t.Fatal("digest is wrong")
	}
	if binary.BigEndian.Uint32(p[HeaderLen+36:]) != 0xcafef00d {
		t.Fatal("token is wrong")
	}
}

func TestEncodeDataLayout(t *testing.T) {
	block := bytes.Repeat([]byte{7}, BlockSize)
	p := EncodeData(9, 42, block)
	if len(p) != HeaderLen+BlockSize || MessageType(p[5]) != TypeData {
		t.Fatalf("data is %d bytes, type %d", len(p), p[5])
	}
	if binary.BigEndian.Uint32(p[12:]) != 42 || !bytes.Equal(p[HeaderLen:], block) {
		t.Fatal("index or payload is wrong")
	}
}

func TestEncodeErrorTruncatesItsText(t *testing.T) {
	p := EncodeError(1, ReasonNoImage, string(bytes.Repeat([]byte("x"), 200)))
	if len(p) != HeaderLen+64 {
		t.Fatalf("error message is %d bytes", len(p))
	}
	if binary.BigEndian.Uint32(p[12:]) != ReasonNoImage {
		t.Fatal("reason is wrong")
	}
}

func TestImageBlocks(t *testing.T) {
	cases := []struct {
		size   int
		blocks uint32
		last   int
	}{
		{1, 1, 1},
		{BlockSize, 1, BlockSize},
		{BlockSize + 1, 2, 1},
		{5000, 5, 5000 - 4*BlockSize},
	}
	for _, c := range cases {
		img := NewImage("i", make([]byte, c.size))
		if img.Blocks() != c.blocks {
			t.Fatalf("%d bytes gave %d blocks, want %d", c.size, img.Blocks(), c.blocks)
		}
		last, ok := img.Block(c.blocks - 1)
		if !ok || len(last) != c.last {
			t.Fatalf("%d bytes: last block is %d bytes (ok=%v), want %d", c.size, len(last), ok, c.last)
		}
		if _, ok := img.Block(c.blocks); ok {
			t.Fatalf("%d bytes: block %d should not exist", c.size, c.blocks)
		}
		if _, ok := img.Block(1 << 30); ok {
			t.Fatal("a huge index should not exist")
		}
	}
}
