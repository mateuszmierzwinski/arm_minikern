package nbd

import "time"

// Wire constants from the NBD protocol document. They are an ABI: the
// kernel's client (src/core/nbd.c) and every other implementation agree on
// these numbers, so they are never "tidied".
const (
	magicGreeting = 0x4e42444d41474943 // "NBDMAGIC"
	magicOption   = 0x49484156454f5054 // "IHAVEOPT"
	magicOptReply = 0x0003e889045565a9
	magicRequest  = 0x25609513
	magicReply    = 0x67446698
)

// Handshake flags, ours then the client's.
const (
	flagFixedNewstyle   = 1 << 0
	flagNoZeroes        = 1 << 1
	clientFixedNewstyle = 1 << 0
	clientNoZeroes      = 1 << 1
)

// Transmission flags an export advertises.
const (
	FlagHasFlags  = 1 << 0
	FlagReadOnly  = 1 << 1
	FlagSendFlush = 1 << 2
)

// Options a client may ask for during the handshake.
const (
	optExportName      = 1
	optAbort           = 2
	optList            = 3
	optStructuredReply = 8
	optInfo            = 6
	optGo              = 7
)

// Option reply types; an error reply has the top bit set.
const (
	repAck        = 1
	repServer     = 2
	repInfo       = 3
	repError      = 0x80000000
	repErrUnsup   = 0x80000001
	repErrPolicy  = 0x80000002
	repErrInvalid = 0x80000003
	repErrUnknown = 0x80000006
)

// NBD_INFO_* payloads we send for NBD_OPT_GO.
const (
	infoExport    = 0
	infoName      = 1
	infoBlockSize = 3
)

// Commands in the transmission phase.
const (
	cmdRead        = 0
	cmdWrite       = 1
	cmdDisconnect  = 2
	cmdFlush       = 3
	cmdTrim        = 4
	cmdWriteZeroes = 6
)

// Errors as NBD numbers: a client reads these, not Go's.
const (
	errNone     = 0
	errIO       = 5
	errInvalid  = 22
	errNoSpace  = 28
	errReadOnly = 30
	errNotSup   = 95
)

// Sizes of the fixed-width messages, so a short read is a protocol error
// rather than a slice index panic.
const (
	requestBytes     = 28
	replyBytes       = 16
	optionHeadBytes  = 16
	optReplyBytes    = 20
	exportNameMax    = 256
	optionPayloadMax = 4096
	optionsMax       = 32
)

// MaxIO is the largest transfer one request may carry. The protocol
// recommends refusing more than 32 MiB, and the limit is what stops a
// 28-byte request from making the server allocate at will.
const MaxIO = 32 << 20

// MinBlock and PrefBlock are what NBD_INFO_BLOCK_SIZE reports: a volume
// file can take any byte range, and XFS reads in 4 KiB blocks.
const (
	MinBlock  = 1
	PrefBlock = 4096
)

// Timeouts. Handshake is short because an unauthenticated peer is holding a
// connection; transmission is longer because a client may idle between
// filesystem operations.
const (
	HandshakeTimeout  = 10 * time.Second
	TransmissionIdle  = 5 * time.Minute
	TransmissionWrite = 60 * time.Second
)

// Request is one transmission-phase command, already validated for shape
// but not for range.
type Request struct {
	Flags  uint16
	Type   uint16
	Handle uint64
	Offset uint64
	Length uint32
}

// Option is one handshake option and its payload.
type Option struct {
	ID      uint32
	Payload []byte
}

// ExportInfo is what a client learns about a volume before using it.
type ExportInfo struct {
	Name     string
	Size     uint64
	ReadOnly bool
}

// TransmissionFlags is what the export advertises in the handshake: flush
// is offered for a writable export, because the client needs a way to make
// its filesystem durable.
func (e ExportInfo) TransmissionFlags() uint16 {
	flags := uint16(FlagHasFlags | FlagSendFlush)
	if e.ReadOnly {
		flags |= FlagReadOnly
	}
	return flags
}

// Stats counts what a session did, for the log line it writes when it ends.
type Stats struct {
	Reads      uint64
	Writes     uint64
	Flushes    uint64
	BytesRead  uint64
	BytesWrote uint64
}
