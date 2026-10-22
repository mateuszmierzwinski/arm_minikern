package nbd

import (
	"errors"
	"io"
)

// The package's pure transforms (master Rule 9): data in, data out, no
// connection and no file in sight, so each one is tested and benchmarked on
// its own.

// verb says how an export was served, so a log line distinguishes a
// read-only client from one that could have changed the volume.
func verb(info ExportInfo) string {
	if info.ReadOnly {
		return "read"
	}
	return "served"
}

// nbdError maps one of this package's errors onto the number a client
// understands. Anything unrecognised is EIO: a client can retry or give up
// on that, which is the safe reading of an error we cannot explain.
func nbdError(err error) uint32 {
	switch {
	case err == nil:
		return errNone
	case errors.Is(err, ErrRange):
		return errInvalid
	case errors.Is(err, ErrReadOnly):
		return errReadOnly
	case errors.Is(err, io.ErrShortWrite):
		return errNoSpace
	}
	return errIO
}

// infoFor describes an open volume under the name it was asked for.
func infoFor(name string, vol Volume) ExportInfo {
	return ExportInfo{Name: name, Size: uint64(vol.Size()), ReadOnly: vol.ReadOnly()}
}

// chunkOf is the next piece of a transfer: at most len(buf), and never more
// than is left. It exists so the read and write paths cannot disagree about
// the arithmetic that bounds their memory.
func chunkOf(left uint32, buf []byte) uint32 {
	if left > uint32(len(buf)) {
		return uint32(len(buf))
	}
	return left
}
