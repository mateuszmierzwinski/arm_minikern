package mknb

import (
	"net/netip"
	"time"
)

// ImageStore finds the root filesystem a board should boot.
type ImageStore interface {
	// Image returns the image for board, or an error wrapping ErrNoImage
	// when there is nothing to serve it.
	Image(board BoardID) (*Image, error)
}

// Transport sends one datagram. The service never blocks on it.
type Transport interface {
	Send(to netip.AddrPort, payload []byte) error
}

// Clock is the service's view of time, so tests need no sleeping.
type Clock interface {
	Now() time.Time
}

// TokenSource issues the per-session tokens that keep the server from
// being a UDP amplifier (NETBOOT.md §3). They must be unpredictable.
type TokenSource interface {
	Token() uint32
}

// Logger records what the server served and what it refused.
type Logger interface {
	Printf(format string, args ...any)
}
