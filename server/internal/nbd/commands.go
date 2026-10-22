package nbd

import (
	"fmt"
	"io"
)

// The transmission-phase commands, one struct each (master Rule 6.3).
//
// Two rules hold for all of them. A request is validated before any byte
// moves, because an offset outside the volume must become an error reply
// rather than a short file. And once a success header has been sent the
// connection is committed: an I/O failure after it can only be reported by
// closing, since the client is already reading the data that follows.

func newCommandHandlers() map[uint16]commandHandler {
	return map[uint16]commandHandler{
		cmdRead:       &readCommand{},
		cmdWrite:      &writeCommand{},
		cmdFlush:      &flushCommand{},
		cmdDisconnect: &disconnectCommand{},
	}
}

type readCommand struct{}

func (c *readCommand) Handle(t *transmission, req Request) error {
	if err := checkRange(t, req); err != nil {
		return writeReply(t.conn, req.Handle, nbdError(err))
	}
	if err := writeReply(t.conn, req.Handle, errNone); err != nil {
		return err
	}
	if err := c.stream(t, req); err != nil {
		return err
	}
	t.stats.Reads++
	t.stats.BytesRead += uint64(req.Length)
	return nil
}

// stream sends the data a chunk at a time, so the memory a request costs
// does not depend on how much it asked for.
func (c *readCommand) stream(t *transmission, req Request) error {
	off, left := req.Offset, req.Length
	for left > 0 {
		n := chunkOf(left, t.buf)
		if _, err := t.vol.ReadAt(t.buf[:n], int64(off)); err != nil {
			return fmt.Errorf("read %d at %d: %w", n, off, err)
		}
		if _, err := t.conn.Write(t.buf[:n]); err != nil {
			return err
		}
		off += uint64(n)
		left -= n
	}
	return nil
}

type writeCommand struct{}

// Handle refuses a bad write *after* draining its payload: the data is
// already on its way, and leaving it in the stream would make it look like
// the next request.
func (c *writeCommand) Handle(t *transmission, req Request) error {
	if req.Length > MaxIO {
		return fmt.Errorf("%w: write of %d bytes", ErrProtocol, req.Length)
	}
	if err := c.refusal(t, req); err != nil {
		if drainErr := drain(t, req.Length); drainErr != nil {
			return drainErr
		}
		return writeReply(t.conn, req.Handle, nbdError(err))
	}
	if err := c.store(t, req); err != nil {
		return err
	}
	t.stats.Writes++
	t.stats.BytesWrote += uint64(req.Length)
	return writeReply(t.conn, req.Handle, errNone)
}

// refusal is the reason this write cannot be done, or nil.
func (c *writeCommand) refusal(t *transmission, req Request) error {
	if t.info.ReadOnly {
		return ErrReadOnly
	}
	return checkRange(t, req)
}

// store moves the payload into the volume a chunk at a time. A failure here
// ends the connection: part of the write has landed, and the client must
// not be told it all did.
func (c *writeCommand) store(t *transmission, req Request) error {
	off, left := req.Offset, req.Length
	for left > 0 {
		n := chunkOf(left, t.buf)
		if _, err := io.ReadFull(t.conn, t.buf[:n]); err != nil {
			return err
		}
		if _, err := t.vol.WriteAt(t.buf[:n], int64(off)); err != nil {
			return fmt.Errorf("write %d at %d: %w", n, off, err)
		}
		off += uint64(n)
		left -= n
	}
	return nil
}

type flushCommand struct{}

func (c *flushCommand) Handle(t *transmission, req Request) error {
	if t.info.ReadOnly {
		return writeReply(t.conn, req.Handle, errNone) // nothing was written: nothing to flush
	}
	if err := t.vol.Flush(); err != nil {
		return writeReply(t.conn, req.Handle, errIO)
	}
	t.stats.Flushes++
	return writeReply(t.conn, req.Handle, errNone)
}

// disconnectCommand is the client saying goodbye. There is no reply to it,
// and ErrClosed is how the session ends well.
type disconnectCommand struct{}

func (c *disconnectCommand) Handle(_ *transmission, _ Request) error {
	return ErrClosed
}
