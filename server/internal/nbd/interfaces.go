package nbd

import (
	"context"
	"io"
	"time"
)

// Volume is one export: a file on the server's disk, addressed by byte
// offset. Every implementation is responsible for its own bounds — the
// service checks ranges too, and neither trusts the other.
type Volume interface {
	// Size is the volume's length in bytes. It does not change while the
	// volume is open, so a client's view of the geometry stays true.
	Size() int64

	// ReadOnly says whether writes must be refused.
	ReadOnly() bool

	// ReadAt and WriteAt behave as io.ReaderAt and io.WriterAt: a short
	// result is an error, never a partial success.
	io.ReaderAt
	io.WriterAt

	// Flush makes earlier writes durable.
	Flush() error

	// Close releases the underlying file.
	Close() error
}

// VolumeStore finds a volume by the export name a client asked for. It owns
// the mapping from that name to a path, which is the only place a traversal
// could be introduced, so it is also where the name is validated.
type VolumeStore interface {
	// Open returns the named volume, or ErrNoExport. An empty name means
	// the store's default export.
	Open(name string) (Volume, error)

	// List names every export, for NBD_OPT_LIST. The order is stable so a
	// test can assert it.
	List() []string
}

// Conn is one client connection: a byte stream with deadlines. The TCP
// adapter wraps a net.Conn to add RemoteName; a test uses a pipe, so the
// service is tested without a socket at all (master Rule 16).
type Conn interface {
	io.ReadWriteCloser

	// SetDeadline bounds a single blocking read or write. The service sets
	// it before every message, which is what stops a peer from holding a
	// connection open by saying nothing. net.Conn satisfies this as it is.
	SetDeadline(t time.Time) error

	// RemoteName identifies the peer for the log, with no reverse lookup.
	RemoteName() string
}

// Logger is the one output port: the service never writes to a stream
// itself, so a test can read back what a session reported.
type Logger interface {
	Printf(format string, args ...any)
}

// Session serves one client connection from its handshake to its end.
type Session interface {
	Serve(ctx context.Context, c Conn) error
}

// optionHandler answers one handshake option. It returns true when the
// handshake is over and the connection moves to transmission.
type optionHandler interface {
	Handle(h *handshake, opt Option) (bool, error)
}

// commandHandler answers one transmission-phase request. A returned error
// ends the connection; a refusal the client should survive is an error
// reply written by the handler itself.
type commandHandler interface {
	Handle(t *transmission, req Request) error
}

// Server accepts connections and serves them until its context ends.
type Server interface {
	Serve(ctx context.Context) error

	// Addr is where it is listening, which a test needs when it asked for
	// port 0.
	Addr() string
}
