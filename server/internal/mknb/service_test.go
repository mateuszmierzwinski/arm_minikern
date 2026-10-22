package mknb

import (
	"encoding/binary"
	"errors"
	"fmt"
	"net/netip"
	"sync"
	"testing"
	"time"
)

// --- fakes (hand-written, as ORG.md §6 prefers) ---------------------------

// fakeStore serves one image, or an error.
type fakeStore struct {
	img *Image
	err error
}

func (f *fakeStore) Image(BoardID) (*Image, error) {
	if f.err != nil {
		return nil, f.err
	}
	return f.img, nil
}

// sent is one captured datagram.
type sent struct {
	to      netip.AddrPort
	payload []byte
}

// fakeTransport records what the service sent, and can fail on demand.
type fakeTransport struct {
	mu   sync.Mutex
	out  []sent
	fail error
}

func (f *fakeTransport) Send(to netip.AddrPort, payload []byte) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.fail != nil {
		return f.fail
	}
	f.out = append(f.out, sent{to: to, payload: append([]byte(nil), payload...)})
	return nil
}

// ofType returns the captured datagrams of one message type.
func (f *fakeTransport) ofType(t MessageType) []sent {
	f.mu.Lock()
	defer f.mu.Unlock()
	var out []sent
	for _, s := range f.out {
		if len(s.payload) > 5 && MessageType(s.payload[5]) == t {
			out = append(out, s)
		}
	}
	return out
}

func (f *fakeTransport) bytes() int {
	f.mu.Lock()
	defer f.mu.Unlock()
	n := 0
	for _, s := range f.out {
		n += len(s.payload)
	}
	return n
}

// fakeClock is time the test controls.
type fakeClock struct {
	mu  sync.Mutex
	now time.Time
}

func (f *fakeClock) Now() time.Time {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.now
}

func (f *fakeClock) advance(d time.Duration) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.now = f.now.Add(d)
}

// countingTokens issues predictable tokens so a test can echo them.
type countingTokens struct {
	mu   sync.Mutex
	next uint32
}

func (c *countingTokens) Token() uint32 {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.next++
	return 0x1000 + c.next
}

// recordingLogger keeps the lines, so a refusal can be asserted.
type recordingLogger struct {
	mu    sync.Mutex
	lines []string
}

func (r *recordingLogger) Printf(format string, args ...any) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.lines = append(r.lines, fmt.Sprintf(format, args...))
}

// --- the harness ---------------------------------------------------------

type harness struct {
	svc    *Service
	out    *fakeTransport
	store  *fakeStore
	clock  *fakeClock
	tokens *countingTokens
	log    *recordingLogger
	img    *Image
}

func newHarness(t *testing.T, imageBytes int, cfg Config) *harness {
	t.Helper()
	data := make([]byte, imageBytes)
	for i := range data {
		data[i] = byte(i * 31 % 251)
	}
	h := &harness{
		out:    &fakeTransport{},
		store:  &fakeStore{},
		clock:  &fakeClock{now: time.Unix(1700000000, 0)},
		tokens: &countingTokens{},
		log:    &recordingLogger{},
		img:    NewImage("root.tar", data),
	}
	h.store.img = h.img
	h.svc = NewService(h.store, h.out, h.clock, h.tokens, h.log, cfg)
	return h
}

func addr(s string) netip.AddrPort {
	return netip.MustParseAddrPort(s)
}

// discover sends a probe and returns the token the OFFER carried.
func (h *harness) discover(t *testing.T, from netip.AddrPort, xid uint32) uint32 {
	t.Helper()
	h.svc.Handle(from, discoverBytes(xid, testBoard()))
	offers := h.out.ofType(TypeOffer)
	if len(offers) == 0 {
		t.Fatal("no OFFER was sent")
	}
	last := offers[len(offers)-1].payload
	return binary.BigEndian.Uint32(last[HeaderLen+36:])
}

// --- the tests -----------------------------------------------------------

func TestDiscoverIsOffered(t *testing.T) {
	h := newHarness(t, 5000, Config{})
	client := addr("10.0.2.15:40000")
	token := h.discover(t, client, 0x1234)
	offers := h.out.ofType(TypeOffer)
	if len(offers) != 1 || offers[0].to != client {
		t.Fatalf("got %d offers", len(offers))
	}
	if binary.BigEndian.Uint32(offers[0].payload[12:]) != 5000 {
		t.Fatal("the offer named the wrong length")
	}
	if token == 0 {
		t.Fatal("the offer carried no token")
	}
	if s := h.svc.Stats(); s.Discovers != 1 || s.Offers != 1 || s.SessionsNow != 1 {
		t.Fatalf("stats: %+v", s)
	}
}

func TestReadServesTheWindow(t *testing.T) {
	h := newHarness(t, 5000, Config{})
	client := addr("10.0.2.15:40000")
	token := h.discover(t, client, 1)
	h.svc.Handle(client, readBytes(1, 0, 64, token))
	data := h.out.ofType(TypeData)
	if len(data) != 5 {
		t.Fatalf("got %d blocks, want 5", len(data))
	}
	for i, d := range data {
		index := binary.BigEndian.Uint32(d.payload[12:])
		block, _ := h.img.Block(uint32(i))
		if index != uint32(i) || string(d.payload[HeaderLen:]) != string(block) {
			t.Fatalf("block %d is wrong (index %d, %d bytes)", i, index, len(d.payload)-HeaderLen)
		}
	}
	if s := h.svc.Stats(); s.Blocks != 5 || s.Reads != 1 {
		t.Fatalf("stats: %+v", s)
	}
}

func TestReadHonoursTheWindowAndOffset(t *testing.T) {
	h := newHarness(t, 200*BlockSize, Config{})
	client := addr("10.0.2.15:40000")
	token := h.discover(t, client, 1)
	h.svc.Handle(client, readBytes(1, 10, 3, token))
	data := h.out.ofType(TypeData)
	if len(data) != 3 || binary.BigEndian.Uint32(data[0].payload[12:]) != 10 {
		t.Fatalf("got %d blocks starting at %d", len(data), binary.BigEndian.Uint32(data[0].payload[12:]))
	}
	// A request for more than the window gets the window.
	h.svc.Handle(client, readBytes(1, 0, 65535, token))
	if n := len(h.out.ofType(TypeData)); n != 3+MaxWindow {
		t.Fatalf("an oversized window gave %d blocks, want %d", n-3, MaxWindow)
	}
}

func TestReadPastTheEndSendsNothing(t *testing.T) {
	h := newHarness(t, 5000, Config{})
	client := addr("10.0.2.15:40000")
	token := h.discover(t, client, 1)
	h.svc.Handle(client, readBytes(1, 5, 64, token))
	h.svc.Handle(client, readBytes(1, 1<<30, 64, token))
	if n := len(h.out.ofType(TypeData)); n != 0 {
		t.Fatalf("got %d blocks for a range past the image", n)
	}
}

func TestDoneClosesTheSession(t *testing.T) {
	h := newHarness(t, 5000, Config{})
	client := addr("10.0.2.15:40000")
	token := h.discover(t, client, 1)
	p := make([]byte, HeaderLen)
	encodeHeader(p, TypeDone, 1, 0)
	h.svc.Handle(client, p)
	if s := h.svc.Stats(); s.Done != 1 || s.SessionsNow != 0 {
		t.Fatalf("stats: %+v", s)
	}
	// A READ afterwards finds nothing.
	h.svc.Handle(client, readBytes(1, 0, 64, token))
	if n := len(h.out.ofType(TypeData)); n != 0 {
		t.Fatalf("served %d blocks after DONE", n)
	}
}

func TestNoImageIsReported(t *testing.T) {
	h := newHarness(t, 5000, Config{})
	h.store.err = fmt.Errorf("looked everywhere: %w", ErrNoImage)
	h.svc.Handle(addr("10.0.2.15:40000"), discoverBytes(1, testBoard()))
	errs := h.out.ofType(TypeError)
	if len(errs) != 1 || binary.BigEndian.Uint32(errs[0].payload[12:]) != ReasonNoImage {
		t.Fatalf("got %d error messages", len(errs))
	}
	if s := h.svc.Stats(); s.NoImage != 1 || s.Offers != 0 {
		t.Fatalf("stats: %+v", s)
	}
}

// A store failure that is not "no image" is logged, and nothing is sent:
// an unexplained fault must not become a reply we can be made to reflect.
func TestStoreFailureIsSilent(t *testing.T) {
	h := newHarness(t, 5000, Config{})
	h.store.err = errors.New("disk is on fire")
	h.svc.Handle(addr("10.0.2.15:40000"), discoverBytes(1, testBoard()))
	if len(h.out.out) != 0 {
		t.Fatalf("sent %d datagrams", len(h.out.out))
	}
	if len(h.log.lines) == 0 {
		t.Fatal("nothing was logged")
	}
}

func TestSessionsExpire(t *testing.T) {
	h := newHarness(t, 5000, Config{SessionTTL: time.Second})
	client := addr("10.0.2.15:40000")
	token := h.discover(t, client, 1)
	h.clock.advance(2 * time.Second)
	h.svc.Handle(client, readBytes(1, 0, 64, token))
	if n := len(h.out.ofType(TypeData)); n != 0 {
		t.Fatalf("an expired session served %d blocks", n)
	}
	// A fresh probe works again; the expired entry is gone, not leaked.
	h.discover(t, client, 2)
	if s := h.svc.Stats(); s.SessionsNow != 1 {
		t.Fatalf("stats: %+v", s)
	}
}

func TestMalformedDatagramsAreCounted(t *testing.T) {
	h := newHarness(t, 5000, Config{})
	client := addr("10.0.2.15:40000")
	h.svc.Handle(client, nil)
	h.svc.Handle(client, []byte{1, 2, 3})
	bad := discoverBytes(1, testBoard())
	bad[0] ^= 0xff
	h.svc.Handle(client, bad)
	h.svc.Handle(client, discoverBytes(1, testBoard())[:HeaderLen+2]) // short body
	unknown := make([]byte, HeaderLen)
	encodeHeader(unknown, MessageType(99), 1, 0)
	h.svc.Handle(client, unknown)
	s := h.svc.Stats()
	if s.Malformed != 4 || s.Unknown != 1 || len(h.out.out) != 0 {
		t.Fatalf("stats: %+v, sent %d", s, len(h.out.out))
	}
}

func TestSendFailureStopsTheWindow(t *testing.T) {
	h := newHarness(t, 5000, Config{})
	client := addr("10.0.2.15:40000")
	token := h.discover(t, client, 1)
	h.out.fail = errors.New("network is down")
	h.svc.Handle(client, readBytes(1, 0, 64, token))
	if s := h.svc.Stats(); s.Blocks != 0 {
		t.Fatalf("counted %d blocks after a send failure", s.Blocks)
	}
}

// Two boards at once keep their own sessions and tokens.
func TestConcurrentSessionsAreIndependent(t *testing.T) {
	h := newHarness(t, 5000, Config{})
	a, b := addr("10.0.2.15:40000"), addr("10.0.2.16:40000")
	tokenA := h.discover(t, a, 1)
	tokenB := h.discover(t, b, 1)
	if tokenA == tokenB {
		t.Fatal("two sessions got the same token")
	}
	h.svc.Handle(a, readBytes(1, 0, 1, tokenB)) // A using B's token
	if n := len(h.out.ofType(TypeData)); n != 0 {
		t.Fatalf("a crossed token served %d blocks", n)
	}
	h.svc.Handle(a, readBytes(1, 0, 1, tokenA))
	if n := len(h.out.ofType(TypeData)); n != 1 {
		t.Fatalf("got %d blocks", n)
	}
}

// Handle must be safe from several goroutines (the transport may fan out).
func TestHandleIsConcurrencySafe(t *testing.T) {
	h := newHarness(t, 20*BlockSize, Config{MaxSessions: 64})
	var wg sync.WaitGroup
	for i := 0; i < 16; i++ {
		wg.Add(1)
		go func(n int) {
			defer wg.Done()
			client := addr(fmt.Sprintf("10.0.2.%d:40000", 20+n))
			h.svc.Handle(client, discoverBytes(uint32(n), testBoard()))
			h.svc.Handle(client, readBytes(uint32(n), 0, 4, 0)) // wrong token: refused
		}(i)
	}
	wg.Wait()
	if s := h.svc.Stats(); s.Offers != 16 || s.BadToken != 16 {
		t.Fatalf("stats: %+v", s)
	}
}
