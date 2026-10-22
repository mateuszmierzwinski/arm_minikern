// Package mknb implements the minikern netboot protocol (NETBOOT.md) and
// the server's session logic. It is the hexagon: pure Go over the
// interfaces in interfaces.go, with no knowledge of sockets or files.
package mknb

import "net/netip"

// Protocol constants. They are the wire format and must match the kernel's
// src/core/net/netboot.h exactly.
const (
	Magic        uint32 = 0x4d4b4e42 // "MKNB"
	Version      byte   = 1
	Port                = 7634
	HeaderLen           = 16
	BlockSize           = 1024
	MaxWindow           = 64 // blocks a single READ may ask for
	OfferBody           = 40
	// SignedOfferBody is an offer with an Ed25519 signature after it, over
	// the session id, the image length and the digest — the three things
	// that decide what a board will run. A board told which key to trust
	// refuses an offer without one (core/net/netboot.h).
	SignedOfferBody = OfferBody + 64
	SignedBytes     = 40
	ReadBody            = 8
	BoardIDLen          = 16
	MACLen              = 6
	DiscoverBody        = MACLen + BoardIDLen
	MaxDatagram         = HeaderLen + BlockSize
)

// MessageType is the header's type field.
type MessageType byte

// The message types, as NETBOOT.md §3 lists them.
const (
	TypeDiscover MessageType = 1
	TypeOffer    MessageType = 2
	TypeRead     MessageType = 3
	TypeData     MessageType = 4
	TypeDone     MessageType = 5
	TypeError    MessageType = 6
)

// Reason codes carried in an ERROR message's arg.
const (
	ReasonNoImage      uint32 = 1
	ReasonImageChanged uint32 = 2
	ReasonMalformed    uint32 = 3
	ReasonBusy         uint32 = 4
)

// Header is the 16 bytes every message starts with.
type Header struct {
	Type MessageType
	XID  uint32
	Arg  uint32
}

// BoardID is how a board names itself: its MAC address and the unique id
// of its chip. Both come off the wire, so neither may ever reach a
// filesystem path without being formatted as hex (see BoardID.Key).
type BoardID struct {
	MAC [MACLen]byte
	ID  [BoardIDLen]byte
}

// Discover is a board asking who can serve it.
type Discover struct {
	Header
	Board BoardID
}

// Read is a request for the window of blocks starting at Header.Arg.
type Read struct {
	Header
	Count uint16
	Token uint32
}

// Image is one board's root filesystem, held in memory with the digest
// the OFFER promises, so a transfer cannot straddle two versions.
type Image struct {
	Name   string
	Data   []byte
	Digest [32]byte
}

// Blocks is how many blocks the image occupies.
func (i *Image) Blocks() uint32 {
	return uint32((len(i.Data) + BlockSize - 1) / BlockSize)
}

// Block returns the bytes of one block, and whether it exists.
func (i *Image) Block(index uint32) ([]byte, bool) {
	start := int(index) * BlockSize
	if index >= i.Blocks() || start >= len(i.Data) {
		return nil, false
	}
	end := min(start+BlockSize, len(i.Data))
	return i.Data[start:end], true
}

// Stats is what the server has done, for the log and for the tests.
type Stats struct {
	Discovers   uint64
	Offers      uint64
	Reads       uint64
	Blocks      uint64
	Done        uint64
	Malformed   uint64
	Unknown     uint64
	NoImage     uint64
	BadToken    uint64
	Busy        uint64
	SessionsNow int
}

// session is one board's transfer, keyed by the address that probed us and
// the transaction id it chose.
type session struct {
	board   BoardID
	image   *Image
	token   uint32
	started int64 // unix nanoseconds
	blocks  uint64
}

// sessionKey identifies a session. The address is part of it, so a READ
// that arrives from anywhere else finds nothing.
type sessionKey struct {
	addr netip.AddrPort
	xid  uint32
}
