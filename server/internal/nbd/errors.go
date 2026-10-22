package nbd

import "errors"

// The service's named errors. A client never sees these strings — it sees
// an NBD error number — but the log does, and so do the tests.
var (
	// ErrNoExport is a name the store does not have. It becomes
	// NBD_REP_ERR_UNKNOWN, which is how a client learns to stop asking.
	ErrNoExport = errors.New("nbd: no such export")

	// ErrBadName is a name that could escape the volume directory, or is
	// too long. It is refused before any file is opened.
	ErrBadName = errors.New("nbd: invalid export name")

	// ErrProtocol is a client that broke the protocol: a bad magic, a
	// length that cannot be honoured, a message cut short. The connection
	// is finished, because there is nothing to resynchronise to.
	ErrProtocol = errors.New("nbd: protocol violation")

	// ErrOldstyle is a client that will not speak fixed newstyle. We do
	// not implement the oldstyle handshake at all.
	ErrOldstyle = errors.New("nbd: client refused fixed newstyle")

	// ErrTooManyOptions is a handshake that will not end: an unbounded
	// option phase is a way to hold a connection and allocate memory.
	ErrTooManyOptions = errors.New("nbd: too many handshake options")

	// ErrRange is an offset or length outside the volume, or larger than
	// MaxIO. It becomes NBD_EINVAL.
	ErrRange = errors.New("nbd: offset or length out of range")

	// ErrReadOnly is a write to a read-only export. It becomes NBD_EROFS.
	ErrReadOnly = errors.New("nbd: export is read-only")

	// ErrClosed says the client said goodbye, or the context ended. It is
	// the one "error" that means everything went fine.
	ErrClosed = errors.New("nbd: connection closed")
)
