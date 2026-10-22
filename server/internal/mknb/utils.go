package mknb

import (
	"crypto/sha256"
	"encoding/hex"
	"strings"
)

// Key names a board for a log line or a filename. Both halves come off the
// wire, so this is the chokepoint that renders them as hex: a value from
// the network can never become a path element of its own choosing.
func (b BoardID) Key() string {
	return hex.EncodeToString(b.MAC[:]) + "-" + hex.EncodeToString(b.ID[:])
}

// MACString formats the board's MAC in the usual colon-separated form.
func (b BoardID) MACString() string {
	parts := make([]string, 0, MACLen)
	for _, v := range b.MAC {
		parts = append(parts, hex.EncodeToString([]byte{v}))
	}
	return strings.Join(parts, ":")
}

// NewImage builds an image and computes the digest the OFFER promises.
func NewImage(name string, data []byte) *Image {
	return &Image{Name: name, Data: data, Digest: sha256.Sum256(data)}
}

// clampWindow bounds what one READ may ask for, so a request cannot make
// the server send an unbounded burst.
func clampWindow(count uint16) uint32 {
	if count == 0 {
		return 1
	}
	return min(uint32(count), MaxWindow)
}

// blockRange is the half-open range of blocks a READ covers, clipped to
// the image.
func blockRange(img *Image, first uint32, count uint16) (uint32, uint32) {
	if first >= img.Blocks() {
		return 0, 0
	}
	return first, min(first+clampWindow(count), img.Blocks())
}

// DigestString is the hex digest, for logs.
func (i *Image) DigestString() string {
	return hex.EncodeToString(i.Digest[:])
}
