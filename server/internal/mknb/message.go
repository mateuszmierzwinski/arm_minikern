package mknb

import "encoding/binary"

// All multi-byte fields are big-endian, as NETBOOT.md §3 specifies.
var order = binary.BigEndian

// ParseHeader reads and validates the common header. It is the only place
// the magic and version are checked, so every message passes through it.
func ParseHeader(p []byte) (Header, error) {
	if len(p) < HeaderLen {
		return Header{}, ErrShort
	}
	if order.Uint32(p) != Magic {
		return Header{}, ErrMagic
	}
	if p[4] != Version {
		return Header{}, ErrVersion
	}
	return Header{Type: MessageType(p[5]), XID: order.Uint32(p[8:]), Arg: order.Uint32(p[12:])}, nil
}

// ParseDiscover reads a DISCOVER's body.
func ParseDiscover(p []byte, h Header) (Discover, error) {
	if len(p) < HeaderLen+DiscoverBody {
		return Discover{}, ErrBody
	}
	d := Discover{Header: h}
	copy(d.Board.MAC[:], p[HeaderLen:])
	copy(d.Board.ID[:], p[HeaderLen+MACLen:])
	return d, nil
}

// ParseRead reads a READ's body.
func ParseRead(p []byte, h Header) (Read, error) {
	if len(p) < HeaderLen+ReadBody {
		return Read{}, ErrBody
	}
	return Read{Header: h, Count: order.Uint16(p[HeaderLen:]), Token: order.Uint32(p[HeaderLen+4:])}, nil
}

// encodeHeader writes a header into the first HeaderLen bytes of p.
func encodeHeader(p []byte, t MessageType, xid, arg uint32) {
	order.PutUint32(p, Magic)
	p[4] = Version
	p[5] = byte(t)
	order.PutUint16(p[6:], 0)
	order.PutUint32(p[8:], xid)
	order.PutUint32(p[12:], arg)
}

// EncodeOffer builds the reply to a DISCOVER: the image's length, its block
// size and digest, and the session token the client must echo.
func EncodeOffer(xid uint32, img *Image, token uint32) ([]byte, error) {
	if uint64(len(img.Data)) > uint64(^uint32(0)) {
		return nil, ErrImageTooLarge
	}
	p := make([]byte, HeaderLen+OfferBody)
	encodeHeader(p, TypeOffer, xid, uint32(len(img.Data)))
	order.PutUint16(p[HeaderLen:], BlockSize)
	order.PutUint16(p[HeaderLen+2:], 0)
	copy(p[HeaderLen+4:], img.Digest[:])
	order.PutUint32(p[HeaderLen+36:], token)
	return p, nil
}

// EncodeData builds one block's datagram.
func EncodeData(xid, index uint32, block []byte) []byte {
	p := make([]byte, HeaderLen+len(block))
	encodeHeader(p, TypeData, xid, index)
	copy(p[HeaderLen:], block)
	return p
}

// EncodeError builds an ERROR with a short, truncated explanation.
func EncodeError(xid, reason uint32, text string) []byte {
	const maxText = 64
	if len(text) > maxText {
		text = text[:maxText]
	}
	p := make([]byte, HeaderLen+len(text))
	encodeHeader(p, TypeError, xid, reason)
	copy(p[HeaderLen:], text)
	return p
}
