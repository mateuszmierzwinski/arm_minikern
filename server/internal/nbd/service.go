package nbd

import (
	"context"
	"fmt"
	"io"
	"time"
)

// chunkBytes is how much of a transfer is held in memory at once. A request
// may be MaxIO (32 MiB), but nothing needs that resident: the data is moved
// a chunk at a time, so a session's memory does not depend on what a client
// asks for.
const chunkBytes = 64 << 10

// service serves one connection at a time: a handshake, then commands until
// the client says goodbye. One request is in flight at a time, which is all
// our own client does and all the protocol requires.
type service struct {
	store    VolumeStore
	log      Logger
	options  map[uint32]optionHandler
	commands map[uint16]commandHandler
}

// NewService wires the session logic over a volume store. The handler maps
// are built here rather than at a call site (master Rule 6.1).
func NewService(store VolumeStore, log Logger) Session {
	s := &service{store: store, log: log}
	s.options = newOptionHandlers(s)
	s.commands = newCommandHandlers()
	return s
}

// Serve runs one connection to its end. The returned error is what the
// session ended on: ErrClosed is the ordinary way out.
func (s *service) Serve(ctx context.Context, c Conn) error {
	defer func() { _ = c.Close() }()
	// Closing the connection is what interrupts a session blocked on it.
	// Without this a cancelled context only takes effect between requests,
	// so a shutdown waits out the idle deadline — minutes — for every
	// client that happens to be saying nothing.
	stop := context.AfterFunc(ctx, func() { _ = c.Close() })
	defer stop()
	started := time.Now()
	vol, info, err := s.handshake(c)
	if err != nil {
		s.log.Printf("nbd: %s handshake failed: %v", c.RemoteName(), err)
		return err
	}
	defer func() { _ = vol.Close() }()
	t := &transmission{conn: c, vol: vol, info: info, buf: make([]byte, chunkBytes)}
	err = s.transmit(ctx, t)
	s.log.Printf("nbd: %s %s %q: %d reads (%d B), %d writes (%d B), %d flushes, %v (%v)",
		c.RemoteName(), verb(info), info.Name, t.stats.Reads, t.stats.BytesRead,
		t.stats.Writes, t.stats.BytesWrote, t.stats.Flushes, time.Since(started).Round(time.Millisecond), err)
	return err
}

// handshake greets the client and runs the option phase until one of the
// options asks to go to transmission. The option count is bounded: an
// endless handshake is a way to hold a connection open for free.
func (s *service) handshake(c Conn) (Volume, ExportInfo, error) {
	if err := c.SetDeadline(time.Now().Add(HandshakeTimeout)); err != nil {
		return nil, ExportInfo{}, err
	}
	if err := writeGreeting(c); err != nil {
		return nil, ExportInfo{}, err
	}
	noZeroes, err := readClientFlags(c)
	if err != nil {
		return nil, ExportInfo{}, err
	}
	h := &handshake{conn: c, noZeroes: noZeroes}
	for i := 0; i < optionsMax; i++ {
		done, err := s.handleOption(h)
		if err != nil || done {
			return h.volume, h.info, err
		}
	}
	return nil, ExportInfo{}, ErrTooManyOptions
}

// handleOption reads one option and dispatches it. An option we do not
// implement is answered with NBD_REP_ERR_UNSUP rather than silence, so a
// client can fall back instead of waiting.
func (s *service) handleOption(h *handshake) (bool, error) {
	opt, err := readOption(h.conn)
	if err != nil {
		return false, err
	}
	handler, ok := s.options[opt.ID]
	if !ok {
		return false, writeOptionReply(h.conn, opt.ID, repErrUnsup, nil)
	}
	return handler.Handle(h, opt)
}

// transmission is one connection's working state: everything a command
// handler needs, and nothing else.
type transmission struct {
	conn  Conn
	vol   Volume
	info  ExportInfo
	buf   []byte
	stats Stats
}

// transmit runs the command loop. A context that ends closes the
// connection: the deadline is what actually interrupts a blocked read, so
// the loop checks the context between requests.
func (s *service) transmit(ctx context.Context, t *transmission) error {
	for {
		if err := ctx.Err(); err != nil {
			return err
		}
		if err := t.conn.SetDeadline(time.Now().Add(TransmissionIdle)); err != nil {
			return err
		}
		req, err := readRequest(t.conn)
		if err != nil {
			return ended(ctx, err)
		}
		if err := s.handleCommand(t, req); err != nil {
			return ended(ctx, err)
		}
	}
}

// handleCommand dispatches one request. A command we do not implement is
// answered with NBD_ENOTSUP, which keeps the stream in step — unlike
// closing, which would lose the requests already sent.
func (s *service) handleCommand(t *transmission, req Request) error {
	if err := t.conn.SetDeadline(time.Now().Add(TransmissionWrite)); err != nil {
		return err
	}
	handler, ok := s.commands[req.Type]
	if !ok {
		return writeReply(t.conn, req.Handle, errNotSup)
	}
	return handler.Handle(t, req)
}

// ended prefers the context's reason over the I/O error it caused: a
// connection closed because the server is shutting down ended for that
// reason, and a caller that logs the difference should see it.
func ended(ctx context.Context, err error) error {
	if ctxErr := ctx.Err(); ctxErr != nil {
		return ctxErr
	}
	return err
}

// checkRange validates a request against the volume before a byte is moved.
// The arithmetic avoids an overflow on purpose: offset + length can wrap,
// and a wrapped comparison is how a write ends up outside the file.
func checkRange(t *transmission, req Request) error {
	size := uint64(t.vol.Size())
	switch {
	case req.Length > MaxIO:
		return fmt.Errorf("%w: length %d over the maximum", ErrRange, req.Length)
	case req.Offset > size || uint64(req.Length) > size-req.Offset:
		return fmt.Errorf("%w: %d bytes at %d in a volume of %d", ErrRange, req.Length, req.Offset, size)
	}
	return nil
}

// drain reads and discards a request's payload, so a refused write does not
// leave its data in the stream to be read as the next request. It is
// bounded by MaxIO, which the caller has already checked.
func drain(t *transmission, n uint32) error {
	if n == 0 {
		return nil
	}
	if n > MaxIO {
		return fmt.Errorf("%w: refusing to drain %d bytes", ErrProtocol, n)
	}
	_, err := io.CopyN(io.Discard, t.conn, int64(n))
	return err
}
