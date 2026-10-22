// Package tcpserver accepts NBD clients over TCP and hands each one to the
// session logic. It is an adapter: it owns sockets, goroutines and limits,
// and knows nothing about the protocol.
package tcpserver

import (
	"context"
	"errors"
	"fmt"
	"net"
	"sync"

	"github.com/mateuszmierzwinski/arm_minikern/server/internal/nbd"
)

// server is a listener plus the session logic to run on each connection.
type server struct {
	listener net.Listener
	session  nbd.Session
	log      nbd.Logger
	slots    chan struct{} // a counting semaphore: the connection limit
	wg       sync.WaitGroup
}

// New listens on addr ("host:port"; port 0 picks one, which is what the
// tests use) and returns a server that is already bound, so a caller that
// gets no error knows the address is theirs.
//
// maxConns bounds concurrent clients. A block device server is
// unauthenticated, so an unbounded accept loop is an invitation: each
// connection costs a goroutine, a 64 KiB buffer and an open file.
func New(addr string, session nbd.Session, log nbd.Logger, maxConns int) (nbd.Server, error) {
	if maxConns < 1 {
		return nil, fmt.Errorf("maxConns must be at least 1, got %d", maxConns)
	}
	l, err := net.Listen("tcp", addr)
	if err != nil {
		return nil, fmt.Errorf("listen on %q: %w", addr, err)
	}
	return &server{listener: l, session: session, log: log, slots: make(chan struct{}, maxConns)}, nil
}

func (s *server) Addr() string { return s.listener.Addr().String() }

// Serve accepts until ctx ends or the listener fails, then waits for the
// sessions it started.
//
// Goroutines (master Rule 20.6): one per accepted connection, running one
// session; each ends when its session returns, and the WaitGroup below is
// what Serve waits on before returning. A second goroutine closes the
// listener when ctx ends, which is what unblocks Accept.
func (s *server) Serve(ctx context.Context) error {
	stop := context.AfterFunc(ctx, func() { _ = s.listener.Close() })
	defer stop()
	s.log.Printf("nbd: listening on %s", s.Addr())
	for {
		conn, err := s.listener.Accept()
		if err != nil {
			s.wg.Wait()
			if ctx.Err() != nil {
				return ctx.Err()
			}
			return fmt.Errorf("accept: %w", err)
		}
		s.start(ctx, conn)
	}
}

// start takes a slot and runs one session, or refuses the connection when
// the server is full. Refusing immediately is kinder than queueing: a
// client learns now rather than after its deadline.
func (s *server) start(ctx context.Context, conn net.Conn) {
	select {
	case s.slots <- struct{}{}:
	default:
		s.log.Printf("nbd: %s refused: %d connections already", conn.RemoteAddr(), cap(s.slots))
		_ = conn.Close()
		return
	}
	s.wg.Add(1)
	go func() {
		defer s.wg.Done()
		defer func() { <-s.slots }()
		err := s.session.Serve(ctx, &wrapped{Conn: conn})
		if err != nil && !errors.Is(err, nbd.ErrClosed) && ctx.Err() == nil {
			s.log.Printf("nbd: %s ended: %v", conn.RemoteAddr(), err)
		}
	}()
}

// wrapped adds the one method nbd.Conn needs beyond net.Conn. The peer is
// named by its address and nothing else: no reverse lookup, which would put
// a client's DNS in charge of our log.
type wrapped struct {
	net.Conn
}

func (w *wrapped) RemoteName() string { return w.Conn.RemoteAddr().String() }
