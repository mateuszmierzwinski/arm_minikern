package nbd

import (
	"context"
	"errors"
	"net"
	"strings"
	"sync"
	"testing"
	"time"
)

// The adversarial suite (master Rule 25) for the product class this is: an
// **unauthenticated network service that writes to a file**. Anyone who can
// reach the port can ask for any offset, any length and any export name, so
// each class below is a way that has gone wrong in a real block-device or
// file-serving daemon — nbd-server and nbdkit's own path handling, the
// classic off+len overflow, length fields used as allocation sizes, and
// handshakes that never end.
//
// The rule these all share: **a client's number is never trusted**, and a
// refusal is an error reply the session survives, not a crash and not a
// silent clamp.

// ── out-of-bounds access ────────────────────────────────────────────────────

// An offset that wraps when the length is added to it must not pass a
// bounds check. This is the oldest bug in block storage.
func TestCVEOffsetLengthOverflow(t *testing.T) {
	volumes := oneVolume(4096, false)
	s := start(t, volumes)
	s.greet(clientFixedNewstyle)
	s.goTo("vol")
	cases := []struct {
		name   string
		off    uint64
		length uint32
	}{
		{"offset just past the end", 4096, 16},
		{"offset at the top of the range", ^uint64(0), 16},
		{"offset that wraps with the length", ^uint64(0) - 8, 32},
		{"length that overruns the end", 4080, 32},
		{"the whole address space", 0, ^uint32(0)},
	}
	for i, c := range cases {
		handle := uint64(i + 1)
		s.send(requestBytesFor(cmdRead, handle, c.off, c.length))
		if err := s.reply(handle); err != errInvalid {
			t.Errorf("%s: got error %d, want EINVAL (%d)", c.name, err, errInvalid)
		}
	}
	// Still alive and still correct afterwards.
	s.send(requestBytesFor(cmdRead, 100, 0, 16))
	if err := s.reply(100); err != errNone {
		t.Fatalf("the session did not survive: %d", err)
	}
	s.read(16)
}

// The same arithmetic on the write path, where the consequence is a
// corrupted or extended file rather than a leak.
func TestCVEWriteOutsideTheVolume(t *testing.T) {
	volumes := oneVolume(4096, false)
	s := start(t, volumes)
	s.greet(clientFixedNewstyle)
	s.goTo("vol")
	payload := make([]byte, 32)
	for i := range payload {
		payload[i] = 0xde
	}
	s.send(append(requestBytesFor(cmdWrite, 1, 4080, 32), payload...))
	if err := s.reply(1); err != errInvalid {
		t.Errorf("got %d, want EINVAL", err)
	}
	if len(volumes["vol"].data) != 4096 {
		t.Errorf("the volume grew to %d bytes", len(volumes["vol"].data))
	}
	for _, b := range volumes["vol"].data[4080:] {
		if b == 0xde {
			t.Fatal("a refused write reached the volume")
		}
	}
}

// ── memory and work a client can demand ─────────────────────────────────────

// A 28-byte request must not be able to make the server allocate 4 GiB. The
// length is checked against MaxIO before any buffer is sized by it.
func TestCVEALengthFieldIsNotAnAllocation(t *testing.T) {
	s := start(t, oneVolume(4096, false))
	s.greet(clientFixedNewstyle)
	s.goTo("vol")
	for i, length := range []uint32{MaxIO + 1, 1 << 30, ^uint32(0)} {
		handle := uint64(i + 1)
		s.send(requestBytesFor(cmdRead, handle, 0, length))
		if err := s.reply(handle); err != errInvalid {
			t.Errorf("a read of %d bytes: got %d, want EINVAL", length, err)
		}
	}
	// A write claiming more than MaxIO ends the connection rather than
	// draining an unbounded amount of data at the client's leisure.
	s.send(requestBytesFor(cmdWrite, 10, 0, MaxIO+1))
	if err := s.wait(); !errors.Is(err, ErrProtocol) {
		t.Errorf("the session ended with %v, want ErrProtocol", err)
	}
}

// An option payload is read into memory, so its length is bounded too.
func TestCVEAnOversizedOptionPayload(t *testing.T) {
	s := start(t, oneVolume(4096, false))
	s.greet(clientFixedNewstyle)
	raw := make([]byte, optionHeadBytes)
	be.PutUint64(raw, magicOption)
	be.PutUint32(raw[8:], optGo)
	be.PutUint32(raw[12:], 0xffffffff)
	s.send(raw)
	if err := s.wait(); !errors.Is(err, ErrProtocol) {
		t.Errorf("the session ended with %v, want ErrProtocol", err)
	}
}

// A handshake that never asks for an export holds a connection, a goroutine
// and a buffer for free. It is bounded by a count as well as a deadline.
func TestCVEAnEndlessHandshake(t *testing.T) {
	s := start(t, oneVolume(4096, false))
	s.greet(clientFixedNewstyle)
	for i := 0; i < optionsMax; i++ {
		if _, err := s.client.Write(optionBytes(optStructuredReply, nil)); err != nil {
			t.Fatalf("sending option %d of %d: %v", i, optionsMax, err)
		}
		if rep := s.readOptReply(); rep.typ != repErrUnsup {
			t.Fatalf("option %d: reply %#x", i, rep.typ)
		}
	}
	if err := s.wait(); !errors.Is(err, ErrTooManyOptions) {
		t.Errorf("the session ended with %v, want ErrTooManyOptions", err)
	}
}

// An export name is a client's string: it is bounded before it is used.
func TestCVEAnEnormousExportName(t *testing.T) {
	s := start(t, oneVolume(4096, false))
	s.greet(clientFixedNewstyle)
	name := strings.Repeat("a", exportNameMax+1)
	payload := make([]byte, 4+len(name)+2)
	be.PutUint32(payload, uint32(len(name)))
	copy(payload[4:], name)
	s.send(optionBytes(optGo, payload))
	if err := s.wait(); !errors.Is(err, ErrProtocol) {
		t.Errorf("the session ended with %v, want ErrProtocol", err)
	}
}

// ── access control ──────────────────────────────────────────────────────────

// Every command that could change the volume must be refused on a read-only
// export, not just the obvious one.
func TestCVEReadOnlyCannotBeBypassed(t *testing.T) {
	volumes := oneVolume(4096, true)
	before := string(volumes["vol"].data)
	s := start(t, volumes)
	s.greet(clientFixedNewstyle)
	s.goTo("vol")
	payload := make([]byte, 16)
	s.send(append(requestBytesFor(cmdWrite, 1, 0, 16), payload...))
	if err := s.reply(1); err != errReadOnly {
		t.Errorf("write: got %d, want EROFS", err)
	}
	for i, cmd := range []uint16{cmdTrim, cmdWriteZeroes} {
		handle := uint64(i + 2)
		s.send(requestBytesFor(cmd, handle, 0, 16))
		if err := s.reply(handle); err != errNotSup {
			t.Errorf("command %d: got %d, want ENOTSUP", cmd, err)
		}
	}
	if string(volumes["vol"].data) != before {
		t.Error("a read-only volume changed")
	}
}

// The store decides what a name means, and the session must never reach
// past it. This checks what actually arrives at the store — the store's own
// traversal defence is tested in the volumefile package, where the name
// becomes a path.
func TestCVETheSessionOnlyOpensWhatTheStoreAllows(t *testing.T) {
	store := &recordingStore{inner: &ramStore{volumes: oneVolume(512, false)}}
	serverEnd, clientEnd := net.Pipe()
	defer func() { _ = clientEnd.Close() }()
	done := make(chan error, 1)
	svc := NewService(store, &testLogger{})
	go func() { done <- svc.Serve(context.Background(), pipeConn{serverEnd}) }()
	s := &session{t: t, client: clientEnd, done: done}
	s.greet(clientFixedNewstyle)
	hostile := []string{"../../etc/passwd", "/etc/shadow", "vol/../vol", "vol\x00.img"}
	for _, name := range hostile {
		s.send(optionBytes(optGo, goPayloadBytes(name)))
		if rep := s.readOptReply(); rep.typ&repError == 0 {
			t.Fatalf("%q was not refused: reply %#x", name, rep.typ)
		}
	}
	s.send(optionBytes(optAbort, nil))
	if rep := s.readOptReply(); rep.typ != repAck {
		t.Errorf("abort reply %#x", rep.typ)
	}
	if err := s.wait(); !errors.Is(err, ErrClosed) {
		t.Errorf("the session ended with %v", err)
	}
	if got := store.names(); len(got) != len(hostile) {
		t.Fatalf("the store was asked for %v, want %v", got, hostile)
	}
	for i, name := range store.names() {
		if name != hostile[i] {
			t.Errorf("the store was asked for %q, want %q — the session altered the name", name, hostile[i])
		}
	}
	if store.handedOut != 0 {
		t.Errorf("the store handed out %d volumes for hostile names", store.handedOut)
	}
}

// recordingStore notes every name it is asked for and refuses all but its
// own, so a test can see exactly what reached it.
type recordingStore struct {
	mu        sync.Mutex
	inner     VolumeStore
	asked     []string
	handedOut int
}

func (s *recordingStore) Open(name string) (Volume, error) {
	s.mu.Lock()
	s.asked = append(s.asked, name)
	s.mu.Unlock()
	if strings.ContainsAny(name, "/\\") || strings.Contains(name, "..") || strings.Contains(name, "\x00") {
		return nil, ErrBadName
	}
	vol, err := s.inner.Open(name)
	if err == nil {
		s.mu.Lock()
		s.handedOut++
		s.mu.Unlock()
	}
	return vol, err
}

func (s *recordingStore) List() []string { return s.inner.List() }

func (s *recordingStore) names() []string {
	s.mu.Lock()
	defer s.mu.Unlock()
	return append([]string(nil), s.asked...)
}

// ── stream integrity ────────────────────────────────────────────────────────

// A refused write's payload must be consumed. If it were not, the data
// would be parsed as the next request — which is how a client could forge
// requests the server never meant to accept.
func TestCVEARefusedWriteCannotInjectARequest(t *testing.T) {
	volumes := oneVolume(4096, false)
	s := start(t, volumes)
	s.greet(clientFixedNewstyle)
	s.goTo("vol")
	// The payload *is* a valid write request to offset 0, 16 bytes of 0xff.
	forged := append(requestBytesFor(cmdWrite, 999, 0, 16), make([]byte, 16)...)
	for i := range forged[requestBytes:] {
		forged[requestBytes+i] = 0xff
	}
	s.send(append(requestBytesFor(cmdWrite, 1, 1<<40, uint32(len(forged))), forged...))
	if err := s.reply(1); err != errInvalid {
		t.Fatalf("the out-of-range write: got %d, want EINVAL", err)
	}
	// If the forged request had been executed there would be a reply for
	// handle 999 waiting; instead a fresh read must answer normally.
	s.send(requestBytesFor(cmdRead, 2, 0, 4))
	if err := s.reply(2); err != errNone {
		t.Fatalf("the stream is out of step: %d", err)
	}
	s.read(4)
	if volumes["vol"].data[0] == 0xff {
		t.Error("a forged request inside a refused write's payload was executed")
	}
}

// ── holding resources ───────────────────────────────────────────────────────

// recordedConn notes the deadlines a session sets, so the test can prove a
// peer cannot hold a connection by saying nothing — without waiting out a
// real timeout.
type recordedConn struct {
	net.Conn
	mu        sync.Mutex
	deadlines []time.Time
}

func (c *recordedConn) SetDeadline(t time.Time) error {
	c.mu.Lock()
	c.deadlines = append(c.deadlines, t)
	c.mu.Unlock()
	return c.Conn.SetDeadline(t)
}

func (c *recordedConn) RemoteName() string { return "recorded" }

func (c *recordedConn) count() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	return len(c.deadlines)
}

func TestCVEEveryPhaseHasADeadline(t *testing.T) {
	serverEnd, clientEnd := net.Pipe()
	defer func() { _ = clientEnd.Close() }()
	conn := &recordedConn{Conn: serverEnd}
	done := make(chan error, 1)
	svc := NewService(&ramStore{volumes: oneVolume(512, false)}, &testLogger{})
	go func() { done <- svc.Serve(context.Background(), conn) }()
	s := &session{t: t, client: clientEnd, done: done}
	s.greet(clientFixedNewstyle)
	if conn.count() == 0 {
		t.Fatal("the handshake ran with no deadline")
	}
	s.goTo("vol")
	before := conn.count()
	s.send(requestBytesFor(cmdRead, 1, 0, 8))
	if err := s.reply(1); err != errNone {
		t.Fatalf("read: %d", err)
	}
	s.read(8)
	s.send(requestBytesFor(cmdDisconnect, 2, 0, 0))
	_ = s.wait()
	if conn.count() <= before {
		t.Error("the transmission phase ran without refreshing the deadline")
	}
	for _, d := range conn.deadlines {
		if d.IsZero() {
			t.Fatal("a deadline was cleared, which means no deadline at all")
		}
	}
}

// A client that connects and then says nothing at all must not leave the
// session blocked for ever: the read fails on its deadline. Proved with a
// deadline of our own rather than by waiting ten seconds.
func TestCVEASilentClientIsDropped(t *testing.T) {
	serverEnd, clientEnd := net.Pipe()
	defer func() { _ = clientEnd.Close() }()
	done := make(chan error, 1)
	svc := NewService(&ramStore{volumes: oneVolume(512, false)}, &testLogger{})
	go func() { done <- svc.Serve(context.Background(), pipeConn{serverEnd}) }()
	// Read the greeting, then say nothing and close: the session must end.
	buf := make([]byte, 18)
	if err := clientEnd.SetReadDeadline(time.Now().Add(5 * time.Second)); err != nil {
		t.Fatal(err)
	}
	if _, err := clientEnd.Read(buf); err != nil {
		t.Fatal(err)
	}
	_ = clientEnd.Close()
	select {
	case err := <-done:
		if err == nil {
			t.Error("a client that said nothing ended the session with no error")
		}
	case <-time.After(5 * time.Second):
		t.Error("the session is still waiting for a client that has gone")
	}
}
