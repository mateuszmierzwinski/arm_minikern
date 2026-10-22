package udptransport

import (
	"context"
	"errors"
	"net"
	"net/netip"
	"sync"
	"testing"
	"time"

	"github.com/mateuszmierzwinski/arm_minikern/server/internal/mknb"
)

// collector is a Handler that records what arrived.
type collector struct {
	mu   sync.Mutex
	got  [][]byte
	from []netip.AddrPort
	ch   chan struct{}
}

func newCollector() *collector {
	return &collector{ch: make(chan struct{}, 16)}
}

func (c *collector) Handle(from netip.AddrPort, payload []byte) {
	c.mu.Lock()
	c.got = append(c.got, append([]byte(nil), payload...))
	c.from = append(c.from, from)
	c.mu.Unlock()
	c.ch <- struct{}{}
}

func (c *collector) count() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	return len(c.got)
}

// listenLoopback opens a server socket on an arbitrary loopback port.
func listenLoopback(t *testing.T) *Conn {
	t.Helper()
	conn, err := Listen("127.0.0.1:0")
	if err != nil {
		t.Fatalf("Listen: %v", err)
	}
	t.Cleanup(func() { _ = conn.Close() })
	return conn
}

func TestListenRejectsABadAddress(t *testing.T) {
	if _, err := Listen("not-an-address"); err == nil {
		t.Fatal("a bad address was accepted")
	}
}

func TestServeDeliversDatagrams(t *testing.T) {
	conn := listenLoopback(t)
	c := newCollector()
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	done := make(chan error, 1)
	go func() { done <- conn.Serve(ctx, c) }()

	client, err := net.Dial("udp4", conn.LocalAddr().String())
	if err != nil {
		t.Fatal(err)
	}
	defer client.Close()
	if _, err := client.Write([]byte("hello")); err != nil {
		t.Fatal(err)
	}
	select {
	case <-c.ch:
	case <-time.After(5 * time.Second):
		t.Fatal("the datagram never arrived")
	}
	if c.count() != 1 || string(c.got[0]) != "hello" {
		t.Fatalf("got %q", c.got)
	}
	cancel()
	if err := <-done; !errors.Is(err, context.Canceled) {
		t.Fatalf("Serve returned %v", err)
	}
}

func TestSendReachesTheClient(t *testing.T) {
	conn := listenLoopback(t)
	client, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	defer client.Close()
	to := client.LocalAddr().(*net.UDPAddr).AddrPort()
	if err := conn.Send(to, []byte("block")); err != nil {
		t.Fatalf("Send: %v", err)
	}
	buf := make([]byte, 64)
	if err := client.SetReadDeadline(time.Now().Add(5 * time.Second)); err != nil {
		t.Fatal(err)
	}
	n, _, err := client.ReadFromUDP(buf)
	if err != nil || string(buf[:n]) != "block" {
		t.Fatalf("got %q, %v", buf[:n], err)
	}
}

// A datagram larger than the protocol's maximum is truncated to the
// buffer, never overflowing it.
func TestOversizedDatagramIsTruncated(t *testing.T) {
	conn := listenLoopback(t)
	c := newCollector()
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go func() { _ = conn.Serve(ctx, c) }()
	client, err := net.Dial("udp4", conn.LocalAddr().String())
	if err != nil {
		t.Fatal(err)
	}
	defer client.Close()
	if _, err := client.Write(make([]byte, 4000)); err != nil {
		t.Fatal(err)
	}
	select {
	case <-c.ch:
	case <-time.After(5 * time.Second):
		t.Fatal("nothing arrived")
	}
	if len(c.got[0]) > mknb.MaxDatagram {
		t.Fatalf("the handler saw %d bytes, over the %d-byte maximum", len(c.got[0]), mknb.MaxDatagram)
	}
}

func TestSendAfterCloseFails(t *testing.T) {
	conn, err := Listen("127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	if err := conn.Close(); err != nil {
		t.Fatal(err)
	}
	if err := conn.Send(netip.MustParseAddrPort("127.0.0.1:9"), []byte("x")); err == nil {
		t.Fatal("a send on a closed socket succeeded")
	}
}
