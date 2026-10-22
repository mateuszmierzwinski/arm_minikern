package xfsmk

// One chunk of inodes: every one of them initialised, because XFS allocates
// inodes in chunks and a repair tool checks each one in an allocated chunk
// — a zeroed inode would be a filesystem with 61 broken files in it.
//
// Three are given contents: the root directory, and the realtime bitmap and
// summary, which the superblock points at whether or not there is a
// realtime device.

func writeInodes(dev Device, l *Layout) error {
	perBlock := l.InopBlock
	for i := uint32(0); i < l.ChunkBlocks; i++ {
		blk := l.blockOf()
		for slot := uint32(0); slot < perBlock; slot++ {
			ino := l.RootIno + uint64(i*perBlock+slot)
			di := blk[slot*l.InodeSize : (slot+1)*l.InodeSize]
			putInode(di, l, ino)
			seal(di, diOffCRC)
		}
		if err := writeBlock(dev, l, l.DeviceBlock(0, l.InodeChunk+i), blk); err != nil {
			return err
		}
	}
	return nil
}

// putInode writes one inode: free unless it is one of the three that are
// not.
func putInode(di []byte, l *Layout, ino uint64) {
	be.PutUint16(di[0:], inodeMagic)
	di[diOffVersion] = inodeVersion
	be.PutUint32(di[diOffNextUnlinked:], ^uint32(0))
	be.PutUint64(di[diOffIno:], ino)
	copy(di[diOffUUID:], l.UUID[:])
	be.PutUint64(di[diOffChangecount:], 1)
	// Every inode has an attribute fork, even an empty one: a format of
	// zero means "a device", and xfs_repair says so ("bad attribute
	// format 0 in inode 32, would reset value").
	di[diOffAformat] = fmtExtents
	switch ino {
	case l.RootIno:
		putRootDir(di, l)
	case l.RbmIno, l.RsumIno:
		putEmptyFile(di)
	default:
		di[diOffFormat] = fmtExtents // free: no mode, no content
	}
}

// putRootDir makes the root an empty shortform directory: a header naming
// its own parent, and no entries. "." and ".." are not stored — a reader
// synthesizes them (XFS.md §2).
func putRootDir(di []byte, l *Layout) {
	sf := di[diCoreSize:]
	be.PutUint16(di[diOffMode:], modeDir)
	di[diOffFormat] = fmtLocal
	be.PutUint32(di[diOffNlink:], 2) // itself, and its own ".."
	sf[0] = 0                        // count
	sf[1] = 0                        // i8count: four-byte inode numbers
	be.PutUint32(sf[2:], uint32(l.RootIno))
	be.PutUint64(di[diOffSize:], 6) // the header is all there is
}

// putEmptyFile makes a regular file of no bytes: what the realtime bitmap
// and summary are on a filesystem with no realtime device.
func putEmptyFile(di []byte) {
	be.PutUint16(di[diOffMode:], modeFile)
	di[diOffFormat] = fmtExtents
	be.PutUint32(di[diOffNlink:], 1)
}
