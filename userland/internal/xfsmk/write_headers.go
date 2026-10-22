package xfsmk

// The four structures that live in block 0 of every allocation group: the
// superblock (a copy in every group, which is the redundancy XFS insists
// on) and the three group headers, each checksummed over its own sector
// (XFS.md §2).

func writeHeaders(dev Device, l *Layout) error {
	for i := range l.Groups {
		if err := writeGroupHeaders(dev, l, &l.Groups[i]); err != nil {
			return err
		}
	}
	return nil
}

// writeGroupHeaders fills and writes one group's first block.
func writeGroupHeaders(dev Device, l *Layout, g *Group) error {
	block := l.blockOf()
	sector := int(l.SectorSize)
	putSuper(block[0:sector], l)
	putAGF(block[sector:2*sector], l, g)
	putAGI(block[2*sector:3*sector], l, g)
	putAGFL(block[3*sector:4*sector], l, g)
	seal(block[0:sector], sbOffCRC)
	seal(block[sector:2*sector], agfOffCRC)
	seal(block[2*sector:3*sector], agiOffCRC)
	seal(block[3*sector:4*sector], agflOffCRC)
	return writeBlock(dev, l, l.DeviceBlock(g.Index, headerBlock), block)
}

// putSuper fills a superblock sector. Every group gets the same one: a
// secondary superblock is a copy, not a variant.
func putSuper(sb []byte, l *Layout) {
	be.PutUint32(sb[0:], sbMagic)
	be.PutUint32(sb[sbOffBlocksize:], l.BlockSize)
	be.PutUint64(sb[sbOffDblocks:], l.Blocks)
	copy(sb[sbOffUUID:], l.UUID[:])
	be.PutUint64(sb[sbOffLogstart:], l.LogFsBlock())
	be.PutUint64(sb[sbOffRootino:], l.RootIno)
	be.PutUint64(sb[sbOffRbmino:], l.RbmIno)
	be.PutUint64(sb[sbOffRsumino:], l.RsumIno)
	be.PutUint32(sb[sbOffRextsize:], 1)
	be.PutUint32(sb[sbOffAgblocks:], l.AgBlocks)
	be.PutUint32(sb[sbOffAgcount:], l.AgCount)
	be.PutUint32(sb[sbOffLogblocks:], l.LogBlocks)
	be.PutUint16(sb[sbOffVersionnum:], versionNum)
	be.PutUint16(sb[sbOffSectsize:], uint16(l.SectorSize))
	be.PutUint16(sb[sbOffInodesize:], uint16(l.InodeSize))
	be.PutUint16(sb[sbOffInopblock:], uint16(l.InopBlock))
	putSuperShifts(sb, l)
	putSuperCounters(sb, l)
}

// putSuperShifts writes the logarithms the format is addressed by.
func putSuperShifts(sb []byte, l *Layout) {
	sb[sbOffBlocklog] = l.BlockLog
	sb[sbOffSectlog] = l.SectorLog
	sb[sbOffInodelog] = l.InodeLog
	sb[sbOffInopblog] = l.InopBlog
	sb[sbOffAgblklog] = l.AgBlkLog
	sb[sbOffRextslog] = 0
	sb[sbOffInprogress] = 0
	sb[sbOffImaxPct] = 25
	sb[sbOffDirblklog] = 0
}

// putSuperCounters writes what the filesystem holds, and which features it
// carries. The features are the narrow set in XFS.md §4: nothing is set
// that the kernel's reader would refuse.
func putSuperCounters(sb []byte, l *Layout) {
	be.PutUint64(sb[sbOffIcount:], l.Icount())
	be.PutUint64(sb[sbOffIfree:], l.Ifree())
	be.PutUint64(sb[sbOffFdblocks:], l.Fdblocks())
	be.PutUint64(sb[sbOffFrextents:], 0)
	be.PutUint32(sb[sbOffInoalignmt:], l.InodeAlignment())
	be.PutUint32(sb[sbOffLogsunit:], 1)
	be.PutUint32(sb[sbOffFeatures2:], features2)
	be.PutUint32(sb[sbOffBadFeat2:], features2)
	be.PutUint32(sb[sbOffFeatCompat:], 0)
	be.PutUint32(sb[sbOffFeatRo:], 0) // no finobt: there is none to keep
	be.PutUint32(sb[sbOffFeatIncompat:], featInFtype)
	be.PutUint32(sb[sbOffFeatLogIncompat:], 0)
}

// putAGF describes one group's free space: where its two free-space btrees
// are, how much is free, and the longest run of it.
func putAGF(agf []byte, l *Layout, g *Group) {
	be.PutUint32(agf[0:], agfMagic)
	be.PutUint32(agf[agfOffVersion:], 1)
	be.PutUint32(agf[agfOffSeqno:], g.Index)
	be.PutUint32(agf[agfOffLength:], g.Blocks)
	be.PutUint32(agf[agfOffRoots:], l.BnobtBlock)
	be.PutUint32(agf[agfOffRoots+4:], l.CntbtBlock)
	be.PutUint32(agf[agfOffLevels:], 1)
	be.PutUint32(agf[agfOffLevels+4:], 1)
	be.PutUint32(agf[agfOffFlfirst:], 0)
	be.PutUint32(agf[agfOffFllast:], 0)
	be.PutUint32(agf[agfOffFlcount:], 0)
	be.PutUint32(agf[agfOffFreeblks:], g.FreeBlocks())
	be.PutUint32(agf[agfOffLongest:], g.Longest())
	be.PutUint32(agf[agfOffBtreeblks:], 0)
	copy(agf[agfOffUUID:], l.UUID[:])
}

// putAGI describes one group's inodes. Only the first group has any: the
// rest carry an empty inode btree, which is what a group with no files in
// it looks like.
func putAGI(agi []byte, l *Layout, g *Group) {
	be.PutUint32(agi[0:], agiMagic)
	be.PutUint32(agi[agiOffVersion:], 1)
	be.PutUint32(agi[agiOffSeqno:], g.Index)
	be.PutUint32(agi[agiOffLength:], g.Blocks)
	be.PutUint32(agi[agiOffCount:], g.Icount)
	be.PutUint32(agi[agiOffRoot:], l.InobtBlock)
	be.PutUint32(agi[agiOffLevel:], 1)
	be.PutUint32(agi[agiOffFreecount:], g.Ifree)
	be.PutUint32(agi[agiOffNewino:], newinoFor(l, g))
	be.PutUint32(agi[agiOffDirino:], ^uint32(0))
	for i := 0; i < 64; i++ {
		be.PutUint32(agi[agiOffUnlinked+i*4:], ^uint32(0))
	}
	copy(agi[agiOffUUID:], l.UUID[:])
	be.PutUint32(agi[agiOffFreeRoot:], ^uint32(0))
	be.PutUint32(agi[agiOffFreeLevel:], 0)
}

// newinoFor is the last chunk allocated in this group, or null when none
// has been.
func newinoFor(l *Layout, g *Group) uint32 {
	if g.Icount == 0 {
		return ^uint32(0)
	}
	return uint32(l.RootIno)
}

// putAGFL writes an empty free list. It is a cache of recently freed
// blocks, and a new filesystem has freed nothing.
func putAGFL(agfl []byte, l *Layout, g *Group) {
	be.PutUint32(agfl[0:], agflMagic)
	be.PutUint32(agfl[agflOffSeqno:], g.Index)
	copy(agfl[agflOffUUID:], l.UUID[:])
	for at := agflOffBnos; at+4 <= len(agfl); at += 4 {
		be.PutUint32(agfl[at:], ^uint32(0))
	}
}
