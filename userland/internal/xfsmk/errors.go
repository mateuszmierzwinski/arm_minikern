package xfsmk

import "errors"

var (
	// ErrTooSmall is a device that cannot hold a filesystem: the
	// metadata, an inode chunk and a log need room before a single file
	// can exist.
	ErrTooSmall = errors.New("xfsmk: the device is too small for a filesystem")

	// ErrGeometry is a block, sector or inode size this cannot use.
	ErrGeometry = errors.New("xfsmk: unsupported geometry")

	// ErrTooLarge is a device needing more than one allocation group.
	// This formatter makes one, which bounds a filesystem at 2^31 blocks
	// — 8 TiB with 4 KiB blocks, and far past what a board will hold.
	ErrTooLarge = errors.New("xfsmk: the device needs more than one allocation group")
)
