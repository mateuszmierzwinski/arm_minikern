package mknb

import "errors"

// Errors the protocol and the store report. A store returns ErrNoImage (or
// an error wrapping it) when a board has nothing to boot; everything else
// is treated as a fault worth logging.
var (
	ErrNoImage       = errors.New("mknb: no image for this board")
	ErrShort         = errors.New("mknb: datagram shorter than its header")
	ErrMagic         = errors.New("mknb: not an MKNB datagram")
	ErrVersion       = errors.New("mknb: unsupported protocol version")
	ErrBody          = errors.New("mknb: message body is the wrong size")
	ErrNoSession     = errors.New("mknb: no session for this address and xid")
	ErrToken         = errors.New("mknb: wrong session token")
	ErrImageTooLarge = errors.New("mknb: image is larger than the protocol allows")
)
