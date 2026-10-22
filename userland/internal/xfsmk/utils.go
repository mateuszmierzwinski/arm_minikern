package xfsmk

import "encoding/binary"

// The package's pure helpers (master Rule 9): arithmetic and byte-poking,
// with no device in sight.

// be is the order every field is written in, apart from the checksums.
var be = binary.BigEndian

// log2exact returns the base-2 logarithm of a power of two, and false for
// anything else. A geometry that is not a power of two would make every
// shift in the format wrong.
func log2exact(n uint64) (uint8, bool) {
	if n == 0 || n&(n-1) != 0 {
		return 0, false
	}
	var log uint8
	for n > 1 {
		n >>= 1
		log++
	}
	return log, true
}

// log2ceil returns the number of bits needed to hold n-1: the form XFS uses
// for agblklog, where the group's block count need not be a power of two.
func log2ceil(n uint64) uint8 {
	var log uint8
	for (uint64(1) << log) < n {
		log++
	}
	return log
}

// roundUp rounds n up to a multiple of to, which must not be zero.
func roundUp(n, to uint64) uint64 {
	return (n + to - 1) / to * to
}

// clamp keeps n within [lo, hi].
func clamp(n, lo, hi uint64) uint64 {
	switch {
	case n < lo:
		return lo
	case n > hi:
		return hi
	}
	return n
}

// putLE32 writes the one field in the format that is little-endian: a
// checksum.
func putLE32(buf []byte, v uint32) {
	binary.LittleEndian.PutUint32(buf, v)
}
