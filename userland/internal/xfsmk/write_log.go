package xfsmk

// The log, as mkfs leaves it: zeroed, with one record whose single
// operation is the unmount transaction. That is what "clean" means — the
// newest thing in the log says the filesystem was put down properly — and
// it is what the kernel's reader looks for before it will mount (XFS.md
// §2).

func writeLog(dev Device, l *Layout) error {
	zero := l.blockOf()
	for i := uint32(0); i < l.LogBlocks; i++ {
		if err := writeBlock(dev, l, l.DeviceBlock(l.LogAg, l.LogStart+i), zero); err != nil {
			return err
		}
	}
	return writeBlock(dev, l, l.DeviceBlock(l.LogAg, l.LogStart), unmountRecord(l))
}

// unmountRecord builds the log's first block: a record header in its first
// sector and the unmount operation in its second.
//
// The log is written "packed": the first word of every block after the
// header is replaced by the cycle number, and the word it displaced is kept
// in the header's cycle-data array. That is how a reader can find the head
// of the log by looking at one word per block, and it is what the kernel's
// own check relies on (XFS.md §2).
func unmountRecord(l *Layout) []byte {
	blk := l.blockOf()
	hdr, op := blk[:logSector], blk[logSector:2*logSector]
	putLogRecordHeader(hdr, l)
	putUnmountOp(op)
	be.PutUint32(hdr[logRecCycleData:], be.Uint32(op)) // the displaced word
	be.PutUint32(op, 1)                                // the cycle takes its place
	return blk
}

// putLogRecordHeader fills a record header: cycle 1, one operation, and a
// length covering that operation's header and its eight bytes of payload.
func putLogRecordHeader(hdr []byte, l *Layout) {
	be.PutUint32(hdr[0:], logMagic)
	be.PutUint32(hdr[logRecCycle:], 1)
	be.PutUint32(hdr[logRecVersion:], 2)
	be.PutUint32(hdr[logRecLen:], logSector)
	be.PutUint64(hdr[logRecLSN:], uint64(1)<<32)     // cycle 1, block 0
	be.PutUint64(hdr[logRecTailLSN:], uint64(1)<<32) // nothing is outstanding
	be.PutUint32(hdr[logRecPrevBlock:], ^uint32(0))
	be.PutUint32(hdr[logRecNumOps:], 1)
	be.PutUint32(hdr[logRecFmt:], logFmtLinux)
	copy(hdr[logRecFsUUID:], l.UUID[:])
	be.PutUint32(hdr[logRecSize:], logIclogSize)
}

// putUnmountOp fills the one operation. Its payload's magic is written in
// the host's byte order, as whoever wrote the log would have; the flag is
// what identifies it either way.
func putUnmountOp(op []byte) {
	be.PutUint32(op[logOpTID:], logUnmountTID)
	be.PutUint32(op[logOpLen:], 8)
	op[logOpClientID] = logClientLog
	op[logOpFlags] = logUnmountTrans
	be.PutUint16(op[logOpHdrSize:], logUnmountMagic)
}
