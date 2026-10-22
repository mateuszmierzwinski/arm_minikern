package xfsmk

import "io"

// Device is the thing being formatted: a block device, or a file on the
// build machine when a test is checking the result with xfs_repair. Both
// are the same code path, which is the point.
type Device interface {
	io.WriterAt
	io.ReaderAt

	// Size is how many bytes there are to format.
	Size() (int64, error)

	// Sync makes everything written durable. On /dev/nbd0 this is what
	// sends the buffer cache to the server.
	Sync() error
}

// Random supplies the filesystem's UUID. It is a port so a test can format
// twice and get the same bytes.
type Random interface {
	Read(p []byte) (int, error)
}
