package tcpserver

import (
	"context"
	"errors"
	"fmt"
	"net"
	"sync"
	"testing"
	"time"

	"github.com/mateuszmierzwinski/arm_minikern/server/internal/nbd"
)

// fakeSession stands in for the protocol: it records what it was given and
// blocks until released, so the adapter's own behaviour — limits, shutdown,
// goroutine lifetime — is what is under test.
type fakeSession struct {
	mu      sync.Mutex
	active  int
	peak    int
	served  int
	release chan struct{}
}

func (f *fakeSession) Serve(ctx context.Context, c nbd.Conn) error {
	f.mu.Lock()
	f.active++
	f.served++
	if f.active > f.peak {
		f.peak = f.active
	}
	f.mu.Unlock()
	defer func() {
		f.mu.Lock()
		f.active--
		f.mu.Unlock()
		_ = c.Close()
	}()
	if _, err := c.Write([]byte("hi")); err != nil {
		return err
	}
	select {
	case <-f.release:
	case <-ctx.Done():
		return ctx.Err()
	case <-time.After(10 * time.Second):
	}
	return nbd.ErrClosed
}

func (f *fakeSession) counts() (active, peak, served int) {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.active, f.peak, f.served
}

type testLogger struct {
	mu    sync.Mutex
	lines []string
}

func (l *testLogger) Printf(format string, args ...any) {
	l.mu.Lock()
	defer l.mu.Unlock()
	l.lines = append(l.lines, fmt.Sprintf(format, args...))
}

func (l *testLogger) contains(substr string) bool {
	l.mu.Lock()
	defer l.mu.Unlock()
	for _, line := range l.lines {
		if len(line) >= len(substr) && contains(line, substr) {
			return true
		}
	}
	return false
}

func contains(haystack, needle string) bool {
	for i := 0; i+len(needle) <= len(haystack); i++ {
		if haystack[i:i+len(needle)] == needle {
			return true
		}
	}
	return false
}

// start brings up a server on a port of the kernel's choosing.
func start(t *testing.T, maxConns int) (nbd.Server, *fakeSession, *testLogger, context.CancelFunc) {
	t.Helper()
	session := &fakeSession{release: make(chan struct{})}
	log := &testLogger{}
	server, err := New("127.0.0.1:0", session, log, maxConns)
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- server.Serve(ctx) }()
	t.Cleanup(func() {
		cancel()
		select {
		case <-done:
		case <-time.After(5 * time.Second):
			t.Error("Serve did not return after its context ended")
		}
	})
	return server, session, log, cancel
}

// dial connects and reads the greeting the fake session writes, so the test
// knows the session is really running.
func dial(t *testing.T, addr string) net.Conn {
	t.Helper()
	c, err := net.DialTimeout("tcp", addr, 5*time.Second)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = c.Close() })
	if err := c.SetReadDeadline(time.Now().Add(5 * time.Second)); err != nil {
		t.Fatal(err)
	}
	buf := make([]byte, 2)
	if _, err := c.Read(buf); err != nil {
		t.Fatalf("the session did not greet us: %v", err)
	}
	return c
}

func TestNewRejectsABadLimit(t *testing.T) {
	if _, err := New("127.0.0.1:0", &fakeSession{}, &testLogger{}, 0); err == nil {
		t.Error("maxConns of 0 should be refused")
	}
	if _, err := New("127.0.0.1:0", &fakeSession{}, &testLogger{}, -1); err == nil {
		t.Error("a negative maxConns should be refused")
	}
}

func TestNewReportsABadAddress(t *testing.T) {
	if _, err := New("nope:nope", &fakeSession{}, &testLogger{}, 1); err == nil {
		t.Error("an unresolvable address should be refused")
	}
}

func TestServeRunsASessionPerConnection(t *testing.T) {
	server, session, _, _ := start(t, 4)
	for i := 0; i < 3; i++ {
		dial(t, server.Addr())
	}
	waitFor(t, func() bool { _, _, served := session.counts(); return served == 3 })
	close(session.release)
	waitFor(t, func() bool { active, _, _ := session.counts(); return active == 0 })
}

// The limit is the point: an unauthenticated service must not let a peer
// open as many connections as it likes.
func TestCVEConnectionsAreLimited(t *testing.T) {
	server, session, log, _ := start(t, 2)
	dial(t, server.Addr())
	dial(t, server.Addr())
	waitFor(t, func() bool { _, peak, _ := session.counts(); return peak == 2 })

	// The third is accepted by the kernel and then closed by us.
	extra, err := net.DialTimeout("tcp", server.Addr(), 5*time.Second)
	if err != nil {
		t.Fatal(err)
	}
	defer func() { _ = extra.Close() }()
	if err := extra.SetReadDeadline(time.Now().Add(5 * time.Second)); err != nil {
		t.Fatal(err)
	}
	buf := make([]byte, 1)
	if _, err := extra.Read(buf); err == nil {
		t.Error("a connection over the limit was served")
	}
	waitFor(t, func() bool { return log.contains("refused") })
	if _, peak, _ := session.counts(); peak > 2 {
		t.Errorf("%d sessions ran at once, limit was 2", peak)
	}
	// A slot freed is a slot reusable.
	close(session.release)
	waitFor(t, func() bool { active, _, _ := session.counts(); return active == 0 })
	dial(t, server.Addr())
}

func TestServeStopsWithItsContext(t *testing.T) {
	server, session, _, cancel := start(t, 2)
	dial(t, server.Addr())
	waitFor(t, func() bool { active, _, _ := session.counts(); return active == 1 })
	cancel()
	// The listener closes, so a new connection is refused.
	waitFor(t, func() bool {
		c, err := net.DialTimeout("tcp", server.Addr(), time.Second)
		if err == nil {
			_ = c.Close()
		}
		return err != nil
	})
}

func TestAddrIsBoundBeforeServe(t *testing.T) {
	session := &fakeSession{release: make(chan struct{})}
	server, err := New("127.0.0.1:0", session, &testLogger{}, 1)
	if err != nil {
		t.Fatal(err)
	}
	// Bound by New, so the address is usable before Serve is called: that
	// is what lets a test (or a systemd unit) know where it is.
	if _, _, err := net.SplitHostPort(server.Addr()); err != nil {
		t.Errorf("Addr() = %q: %v", server.Addr(), err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if err := server.Serve(ctx); !errors.Is(err, context.Canceled) {
		t.Errorf("Serve with a cancelled context: %v", err)
	}
}

func TestTheConnectionNamesItsPeer(t *testing.T) {
	// The wrapper is what adds RemoteName; a nbd.Conn must have it.
	left, right := net.Pipe()
	defer func() { _ = left.Close() }()
	defer func() { _ = right.Close() }()
	var c nbd.Conn = &wrapped{Conn: left}
	if c.RemoteName() == "" {
		t.Error("RemoteName is empty")
	}
}

func waitFor(t *testing.T, cond func() bool) {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		if cond() {
			return
		}
		time.Sleep(5 * time.Millisecond)
	}
	t.Fatal("the condition never came true")
}
