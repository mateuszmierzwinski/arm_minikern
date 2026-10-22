package nbd

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
)

// This file is the wire: every byte in and out of a connection is encoded
// or decoded here, so the session logic in service.go never touches an
// offset. Nothing here reads more than it was told to.

// be is the protocol's byte order, which is network order throughout.
var be = binary.BigEndian

// writeGreeting sends the server's opening message: the two magics and the
// handshake flags. NBD_FLAG_NO_ZEROES is offered because the 124 zero bytes
// after an NBD_OPT_EXPORT_NAME reply exist only for very old clients.
func writeGreeting(w io.Writer) error {
	buf := make([]byte, 18)
	be.PutUint64(buf, magicGreeting)
	be.PutUint64(buf[8:], magicOption)
	be.PutUint16(buf[16:], flagFixedNewstyle|flagNoZeroes)
	return write(w, buf)
}

// readClientFlags takes the client's answer to the greeting. A client that
// does not set NBD_FLAG_C_FIXED_NEWSTYLE gets ErrOldstyle: the oldstyle
// handshake has no option phase, so there would be no way to pick an
// export.
func readClientFlags(r io.Reader) (noZeroes bool, err error) {
	buf := make([]byte, 4)
	if err := read(r, buf); err != nil {
		return false, err
	}
	flags := be.Uint32(buf)
	if flags&clientFixedNewstyle == 0 {
		return false, ErrOldstyle
	}
	return flags&clientNoZeroes != 0, nil
}

// readOption takes one handshake option. The payload length is checked
// against optionPayloadMax before anything is allocated, because the length
// is the client's to choose and ours to disbelieve.
func readOption(r io.Reader) (Option, error) {
	head := make([]byte, optionHeadBytes)
	if err := read(r, head); err != nil {
		return Option{}, err
	}
	if be.Uint64(head) != magicOption {
		return Option{}, fmt.Errorf("%w: option magic %#x", ErrProtocol, be.Uint64(head))
	}
	id, length := be.Uint32(head[8:]), be.Uint32(head[12:])
	if length > optionPayloadMax {
		return Option{}, fmt.Errorf("%w: option %d payload %d bytes", ErrProtocol, id, length)
	}
	payload := make([]byte, length)
	if err := read(r, payload); err != nil {
		return Option{}, err
	}
	return Option{ID: id, Payload: payload}, nil
}

// writeOptionReply answers one option, with an optional body.
func writeOptionReply(w io.Writer, option, replyType uint32, body []byte) error {
	buf := make([]byte, optReplyBytes+len(body))
	be.PutUint64(buf, magicOptReply)
	be.PutUint32(buf[8:], option)
	be.PutUint32(buf[12:], replyType)
	be.PutUint32(buf[16:], uint32(len(body)))
	copy(buf[optReplyBytes:], body)
	return write(w, buf)
}

// exportNamePayload reads the name out of an NBD_OPT_EXPORT_NAME payload,
// which is the whole payload and nothing else.
func exportNamePayload(p []byte) (string, error) {
	if len(p) > exportNameMax {
		return "", ErrBadName
	}
	return string(p), nil
}

// goPayload reads an NBD_OPT_GO or NBD_OPT_INFO payload: a name, then the
// NBD_INFO_* types the client would like. The requests are read so the
// stream stays in step; which information we send does not depend on them.
func goPayload(p []byte) (string, error) {
	if len(p) < 4 {
		return "", fmt.Errorf("%w: option payload %d bytes", ErrProtocol, len(p))
	}
	nameLen := be.Uint32(p)
	if nameLen > exportNameMax || int(nameLen)+6 > len(p) {
		return "", fmt.Errorf("%w: name length %d in %d bytes", ErrProtocol, nameLen, len(p))
	}
	return string(p[4 : 4+nameLen]), nil
}

// infoExportBody is NBD_INFO_EXPORT: the size and the transmission flags,
// which is the one piece of information a client cannot do without.
func infoExportBody(e ExportInfo) []byte {
	body := make([]byte, 12)
	be.PutUint16(body, infoExport)
	be.PutUint64(body[2:], e.Size)
	be.PutUint16(body[10:], e.TransmissionFlags())
	return body
}

// infoBlockSizeBody is NBD_INFO_BLOCK_SIZE: the smallest, preferred and
// largest I/O. A volume file takes any byte range, so the minimum is 1.
func infoBlockSizeBody() []byte {
	body := make([]byte, 14)
	be.PutUint16(body, infoBlockSize)
	be.PutUint32(body[2:], MinBlock)
	be.PutUint32(body[6:], PrefBlock)
	be.PutUint32(body[10:], MaxIO)
	return body
}

// infoNameBody is NBD_INFO_NAME, so a client that asked for the default
// export learns which one it got.
func infoNameBody(name string) []byte {
	body := make([]byte, 2+len(name))
	be.PutUint16(body, infoName)
	copy(body[2:], name)
	return body
}

// writeExportNameReply is the reply to NBD_OPT_EXPORT_NAME: size, flags and
// — unless the client allowed us to skip them — 124 zero bytes.
func writeExportNameReply(w io.Writer, e ExportInfo, noZeroes bool) error {
	buf := make([]byte, 10)
	be.PutUint64(buf, e.Size)
	be.PutUint16(buf[8:], e.TransmissionFlags())
	if !noZeroes {
		buf = append(buf, make([]byte, 124)...)
	}
	return write(w, buf)
}

// readRequest takes one transmission-phase request. A bad magic ends the
// connection: with requests and replies interleaved on one stream there is
// no way to find the next message boundary.
func readRequest(r io.Reader) (Request, error) {
	buf := make([]byte, requestBytes)
	if err := read(r, buf); err != nil {
		return Request{}, err
	}
	if be.Uint32(buf) != magicRequest {
		return Request{}, fmt.Errorf("%w: request magic %#x", ErrProtocol, be.Uint32(buf))
	}
	return Request{
		Flags:  be.Uint16(buf[4:]),
		Type:   be.Uint16(buf[6:]),
		Handle: be.Uint64(buf[8:]),
		Offset: be.Uint64(buf[16:]),
		Length: be.Uint32(buf[24:]),
	}, nil
}

// writeReply sends a simple reply header. Structured replies are never
// negotiated, so this is the only reply shape a client can receive.
func writeReply(w io.Writer, handle uint64, nbdErr uint32) error {
	buf := make([]byte, replyBytes)
	be.PutUint32(buf, magicReply)
	be.PutUint32(buf[4:], nbdErr)
	be.PutUint64(buf[8:], handle)
	return write(w, buf)
}

// read fills buf exactly, turning a truncated message into ErrProtocol and
// a clean close into ErrClosed, so the caller can tell them apart.
func read(r io.Reader, buf []byte) error {
	if len(buf) == 0 {
		return nil
	}
	_, err := io.ReadFull(r, buf)
	switch {
	case errors.Is(err, io.EOF):
		return ErrClosed
	case errors.Is(err, io.ErrUnexpectedEOF):
		return fmt.Errorf("%w: message cut short", ErrProtocol)
	}
	return err
}

func write(w io.Writer, buf []byte) error {
	_, err := w.Write(buf)
	return err
}
