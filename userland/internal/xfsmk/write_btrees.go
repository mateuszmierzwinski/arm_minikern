package xfsmk

// The three btrees of every allocation group, each one leaf block deep:
// free space by block, free space by count, and the inodes. A fresh group
// has at most two free extents and at most one inode chunk, which is what
// keeps this formatter free of btree insertion logic altogether.

// putBtreeHeader fills the 56-byte short-form header every one of them
// shares (XFS.md §2). blkno is in 512-byte units, of the **device**.
func putBtreeHeader(blk []byte, l *Layout, magic uint32, device uint64, ag uint32, numrecs uint16) {
	be.PutUint32(blk[0:], magic)
	be.PutUint16(blk[btOffLevel:], 0) // a leaf: the root is the only block
	be.PutUint16(blk[btOffNumrecs:], numrecs)
	be.PutUint32(blk[btOffLeftsib:], ^uint32(0))
	be.PutUint32(blk[btOffRightsib:], ^uint32(0))
	be.PutUint64(blk[btOffBlkno:], device*uint64(l.BlockSize/512))
	copy(blk[btOffUUID:], l.UUID[:])
	be.PutUint32(blk[btOffOwner:], ag)
}

func writeBtrees(dev Device, l *Layout) error {
	for i := range l.Groups {
		if err := writeGroupBtrees(dev, l, &l.Groups[i]); err != nil {
			return err
		}
	}
	return nil
}

func writeGroupBtrees(dev Device, l *Layout, g *Group) error {
	if err := writeFreeTree(dev, l, g, bnobtMagic, l.BnobtBlock, byBlock(g.Free)); err != nil {
		return err
	}
	if err := writeFreeTree(dev, l, g, cntbtMagic, l.CntbtBlock, byCount(g.Free)); err != nil {
		return err
	}
	return writeInodeTree(dev, l, g)
}

// writeFreeTree writes one free-space btree, whose records are the group's
// free extents in the order that tree keeps them.
func writeFreeTree(dev Device, l *Layout, g *Group, magic, block uint32, extents []Extent) error {
	blk := l.blockOf()
	device := l.DeviceBlock(g.Index, block)
	putBtreeHeader(blk, l, magic, device, g.Index, uint16(len(extents)))
	for i, e := range extents {
		rec := blk[btRecords+i*8:]
		be.PutUint32(rec[0:], e.Start)
		be.PutUint32(rec[4:], e.Count)
	}
	seal(blk, btOffCRC)
	return writeBlock(dev, l, device, blk)
}

// writeInodeTree writes the inode btree: one record for the group that has
// a chunk, none for the groups that do not.
func writeInodeTree(dev Device, l *Layout, g *Group) error {
	blk := l.blockOf()
	device := l.DeviceBlock(g.Index, l.InobtBlock)
	records := uint16(0)
	if g.Icount > 0 {
		records = 1
	}
	putBtreeHeader(blk, l, inobtMagic, device, g.Index, records)
	if records == 1 {
		rec := blk[btRecords:]
		be.PutUint32(rec[0:], uint32(l.RootIno))
		be.PutUint32(rec[4:], g.Ifree)
		be.PutUint64(rec[8:], FreeInodeMask())
	}
	seal(blk, btOffCRC)
	return writeBlock(dev, l, device, blk)
}

// FreeInodeMask is the inode chunk's free bitmap as this formatter leaves
// it: every inode free except the ones it filled in.
func FreeInodeMask() uint64 {
	return ^((uint64(1) << usedInodes) - 1)
}

// byBlock and byCount are the two orders the free-space trees keep their
// records in. They are the same extents, which is the whole point of there
// being two trees.
func byBlock(extents []Extent) []Extent {
	out := append([]Extent(nil), extents...)
	for i := range out {
		for j := i + 1; j < len(out); j++ {
			if out[j].Start < out[i].Start {
				out[i], out[j] = out[j], out[i]
			}
		}
	}
	return out
}

func byCount(extents []Extent) []Extent {
	out := append([]Extent(nil), extents...)
	for i := range out {
		for j := i + 1; j < len(out); j++ {
			if out[j].Count < out[i].Count ||
				(out[j].Count == out[i].Count && out[j].Start < out[i].Start) {
				out[i], out[j] = out[j], out[i]
			}
		}
	}
	return out
}
