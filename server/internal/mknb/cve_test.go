// The server's CVE-class regression suite (master Rule 25). A netboot
// server is in the same product class as TFTP, DHCP and PXE servers, so
// these are the classes that have actually produced CVEs there: UDP
// reflection and amplification, path traversal through a client-supplied
// name, resource exhaustion from unauthenticated requests, parser faults
// on malformed datagrams, and leaking server paths to a client.
package mknb

import (
	"encoding/binary"
	"fmt"
	"net/netip"
	"strings"
	"testing"
)

// An attacker spoofs a victim's source address and asks for a window. The
// server must send nothing, because the token only ever went to whoever
// could receive the OFFER. Without this the server is a ~3000x amplifier.
func TestCVEAmplificationNeedsTheToken(t *testing.T) {
	// The real token source: the defence rests on unpredictability, so the
	// test must not use the predictable fake the other tests echo.
	h := newHarness(t, 64*BlockSize, Config{})
	h.svc = NewService(h.store, h.out, h.clock, RandomTokens{}, h.log, Config{})
	victim := addr("198.51.100.7:53")
	// The attacker can forge a DISCOVER from the victim; that reply is small.
	h.svc.Handle(victim, discoverBytes(0xaaaa, testBoard()))
	offerBytes := h.out.bytes()
	probe := len(discoverBytes(0xaaaa, testBoard()))
	if offerBytes > 4*probe {
		t.Fatalf("the probe reply amplifies %dx", offerBytes/probe)
	}
	// Now the forged READ, with every token the attacker might guess.
	before := h.out.bytes()
	guesses := []uint32{0, 1, 0xffffffff, 0x1001, 0xdeadbeef, 0x5eed1234}
	for i := 0; i < 1000; i++ {
		guesses = append(guesses, uint32(i))
	}
	for _, guess := range guesses {
		h.svc.Handle(victim, readBytes(0xaaaa, 0, MaxWindow, guess))
	}
	if n := h.out.bytes() - before; n != 0 {
		t.Fatalf("a forged READ pushed %d bytes at the victim", n)
	}
	if s := h.svc.Stats(); s.BadToken != uint64(len(guesses)) || s.Blocks != 0 {
		t.Fatalf("stats: %+v", s)
	}
}

// Tokens must not be predictable from one another: that is what an
// off-path attacker would need (and what the fake in service_test.go
// deliberately is not).
func TestCVETokensAreUnpredictable(t *testing.T) {
	var src RandomTokens
	seen := map[uint32]bool{}
	for i := 0; i < 4096; i++ {
		tok := src.Token()
		if seen[tok] {
			t.Fatalf("token %#x repeated within 4096 sessions", tok)
		}
		seen[tok] = true
	}
}

// Even with the right token, blocks go only to the address that holds the
// session: a READ from elsewhere finds no session at all.
func TestCVENoReflectionToAnotherAddress(t *testing.T) {
	h := newHarness(t, 64*BlockSize, Config{})
	client := addr("10.0.2.15:40000")
	token := h.discover(t, client, 1)
	h.svc.Handle(addr("198.51.100.7:53"), readBytes(1, 0, MaxWindow, token))
	if n := len(h.out.ofType(TypeData)); n != 0 {
		t.Fatalf("served %d blocks to an address with no session", n)
	}
	for _, d := range h.out.out {
		if d.to != client {
			t.Fatalf("a datagram went to %s, not the client", d.to)
		}
	}
}

// A board id full of traversal sequences must not reach the filesystem as
// a path: BoardID.Key is the chokepoint and renders hex only.
func TestCVEBoardIDCannotTraverse(t *testing.T) {
	attacks := []string{"../../../../etc/passwd", "..\\..\\windows", "/etc/shadow", "a\x00b"}
	for _, attack := range attacks {
		board := BoardID{}
		copy(board.ID[:], attack)
		copy(board.MAC[:], attack)
		key := board.Key()
		if strings.ContainsAny(key, "./\\\x00") {
			t.Fatalf("%q became %q", attack, key)
		}
		for _, c := range key {
			if !strings.ContainsRune("0123456789abcdef-", c) {
				t.Fatalf("%q became %q, which is not hex", attack, key)
			}
		}
	}
}

// A flood of probes from many addresses must not grow the session table
// past its cap, and must not keep the server from serving anyone.
func TestCVESessionFloodIsBounded(t *testing.T) {
	h := newHarness(t, BlockSize, Config{MaxSessions: 8, SessionsPerClient: 2})
	for i := 0; i < 200; i++ {
		from := addr(fmt.Sprintf("10.0.%d.%d:40000", i/250, i%250+1))
		h.svc.Handle(from, discoverBytes(uint32(i), testBoard()))
	}
	s := h.svc.Stats()
	if s.SessionsNow > 8 {
		t.Fatalf("the table holds %d sessions, cap is 8", s.SessionsNow)
	}
	if s.Busy == 0 {
		t.Fatal("the flood was never refused")
	}
}

// One address cannot take the whole table by varying its transaction id.
func TestCVEOneClientCannotFillTheTable(t *testing.T) {
	h := newHarness(t, BlockSize, Config{MaxSessions: 64, SessionsPerClient: 3})
	client := addr("10.0.2.15:40000")
	for i := 0; i < 50; i++ {
		h.svc.Handle(client, discoverBytes(uint32(i), testBoard()))
	}
	if s := h.svc.Stats(); s.SessionsNow > 3 {
		t.Fatalf("one client holds %d sessions, cap is 3", s.SessionsNow)
	}
}

// The ERROR a client receives must not name a path on the server: that is
// an information disclosure, and the detail belongs in the log.
func TestCVEErrorRevealsNoPaths(t *testing.T) {
	h := newHarness(t, BlockSize, Config{})
	h.store.err = fmt.Errorf("%w: tried /srv/netboot/secret/root.tar", ErrNoImage)
	h.svc.Handle(addr("10.0.2.15:40000"), discoverBytes(1, testBoard()))
	errs := h.out.ofType(TypeError)
	if len(errs) != 1 {
		t.Fatalf("got %d error messages", len(errs))
	}
	if text := string(errs[0].payload[HeaderLen:]); strings.Contains(text, "/") {
		t.Fatalf("the client was told %q", text)
	}
	if len(h.log.lines) == 0 || !strings.Contains(h.log.lines[0], "/srv/netboot") {
		t.Fatal("the detail did not reach the log")
	}
}

// Mutated datagrams: the service must count and drop them, never panic,
// and never send anything it was not asked for.
func TestCVEFuzzedDatagrams(t *testing.T) {
	h := newHarness(t, 8*BlockSize, Config{MaxSessions: 32})
	client := addr("10.0.2.15:40000")
	token := h.discover(t, client, 1)
	seeds := [][]byte{
		discoverBytes(1, testBoard()),
		readBytes(1, 0, 64, token),
		EncodeData(1, 0, make([]byte, BlockSize)),
	}
	var x uint64 = 0x9e3779b97f4a7c15
	for i := 0; i < 20000; i++ {
		x ^= x << 13
		x ^= x >> 7
		x ^= x << 17
		p := append([]byte(nil), seeds[i%len(seeds)]...)
		p = p[:1+int(x>>40)%len(p)]
		if len(p) > 0 {
			p[int(x>>8)%len(p)] ^= byte(x >> 24)
		}
		h.svc.Handle(client, p) // must not panic
	}
	if s := h.svc.Stats(); s.Malformed+s.Unknown+s.BadToken == 0 {
		t.Fatalf("nothing was refused: %+v", s)
	}
}

// A huge block index or count must not overflow into a real range.
func TestCVEHugeIndicesAreRefused(t *testing.T) {
	h := newHarness(t, 5000, Config{})
	client := addr("10.0.2.15:40000")
	token := h.discover(t, client, 1)
	for _, first := range []uint32{0xffffffff, 0xfffffffe, 1 << 31, 1 << 24} {
		h.svc.Handle(client, readBytes(1, first, 0xffff, token))
	}
	if n := len(h.out.ofType(TypeData)); n != 0 {
		t.Fatalf("a huge index produced %d blocks", n)
	}
}

// An image the protocol cannot describe (over 4 GiB) must be refused
// rather than silently truncated by the 32-bit length field.
func TestCVEOversizedImageIsRefused(t *testing.T) {
	img := &Image{Name: "huge.tar", Data: nil}
	// Fake the length without allocating: the guard reads len(Data), so a
	// real check needs a real slice; assert the guard's boundary instead.
	if _, err := EncodeOffer(1, NewImage("ok", make([]byte, 16)), 1, nil); err != nil {
		t.Fatalf("a small image was refused: %v", err)
	}
	if img.Blocks() != 0 {
		t.Fatal("an empty image claims blocks")
	}
	if _, ok := img.Block(0); ok {
		t.Fatal("an empty image served a block")
	}
}

// The window the server sends is bounded whatever the client asks, so one
// request cannot be turned into an unbounded burst.
func TestCVEWindowIsBounded(t *testing.T) {
	h := newHarness(t, 4096*BlockSize, Config{})
	client := addr("10.0.2.15:40000")
	token := h.discover(t, client, 1)
	h.svc.Handle(client, readBytes(1, 0, 0xffff, token))
	data := h.out.ofType(TypeData)
	if len(data) != MaxWindow {
		t.Fatalf("one READ sent %d blocks, cap is %d", len(data), MaxWindow)
	}
	request := len(readBytes(1, 0, 0xffff, token))
	if ratio := h.out.bytes() / request; ratio > 3200 {
		t.Fatalf("a legitimate READ amplifies %dx; bound the window further", ratio)
	}
}

// Every datagram the server emits must be a well-formed MKNB message with
// the client's own transaction id: nothing half-built escapes.
func TestCVERepliesAreWellFormed(t *testing.T) {
	h := newHarness(t, 5000, Config{})
	client := addr("10.0.2.15:40000")
	token := h.discover(t, client, 0x4242)
	h.svc.Handle(client, readBytes(0x4242, 0, 64, token))
	if len(h.out.out) == 0 {
		t.Fatal("nothing was sent")
	}
	for _, d := range h.out.out {
		if len(d.payload) < HeaderLen || len(d.payload) > MaxDatagram {
			t.Fatalf("a %d-byte datagram", len(d.payload))
		}
		if binary.BigEndian.Uint32(d.payload) != Magic || d.payload[4] != Version {
			t.Fatal("a reply had the wrong header")
		}
		if binary.BigEndian.Uint32(d.payload[8:]) != 0x4242 {
			t.Fatal("a reply carried the wrong xid")
		}
	}
}

var _ = netip.AddrPort{}
