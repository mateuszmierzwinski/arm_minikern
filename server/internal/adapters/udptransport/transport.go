// Package udptransport is the server's UDP socket: the adapter that turns
// datagrams into Service calls and the Service's replies into datagrams.
package udptransport

import (
	"context"
	"errors"
	"fmt"
	"net"
	"net/netip"
	"os"
	"time"

	"github.com/mateuszmierzwinski/arm_minikern/server/internal/mknb"
)

// Handler is what a received datagram is given to (the Service).
type Handler interface {
	Handle(from netip.AddrPort, payload []byte)
}

// Conn is an open UDP socket. It implements mknb.Transport.
type Conn struct {
	conn *net.UDPConn
}

// Listen opens the socket on addr ("" means every address on mknb.Port).
func Listen(addr string) (*Conn, error) {
	if addr == "" {
		addr = fmt.Sprintf(":%d", mknb.Port)
	}
	a, err := net.ResolveUDPAddr("udp4", addr)
	if err != nil {
		return nil, fmt.Errorf("resolving %q: %w", addr, err)
	}
	conn, err := net.ListenUDP("udp4", a)
	if err != nil {
		return nil, fmt.Errorf("listening on %s: %w", a, err)
	}
	return &Conn{conn: conn}, nil
}

// LocalAddr is the address the socket really bound (the port may have been 0).
func (c *Conn) LocalAddr() netip.AddrPort {
	return c.conn.LocalAddr().(*net.UDPAddr).AddrPort()
}

// Send delivers one datagram. A send failure is the caller's to log: one
// unreachable client must not stop the server.
func (c *Conn) Send(to netip.AddrPort, payload []byte) error {
	_, err := c.conn.WriteToUDPAddrPort(payload, to)
	return err
}

// Close releases the socket.
func (c *Conn) Close() error {
	return c.conn.Close()
}

// Serve reads datagrams until ctx is cancelled or the socket is closed,
// handing each to the handler. It owns no goroutines: the caller decides
// whether to run it in one.
func (c *Conn) Serve(ctx context.Context, h Handler) error {
	buf := make([]byte, mknb.MaxDatagram)
	go func() {
		<-ctx.Done()
		_ = c.conn.SetReadDeadline(deadlineNow())
	}()
	for {
		n, from, err := c.conn.ReadFromUDPAddrPort(buf)
		if err != nil {
			if ctx.Err() != nil || errors.Is(err, net.ErrClosed) {
				return ctx.Err()
			}
			if errors.Is(err, os.ErrDeadlineExceeded) {
				return ctx.Err()
			}
			return fmt.Errorf("reading: %w", err)
		}
		h.Handle(from, buf[:n])
	}
}

// deadlineNow is a time already past, which unblocks a pending read.
func deadlineNow() time.Time {
	return time.Unix(1, 0)
}
