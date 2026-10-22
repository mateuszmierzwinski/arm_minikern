package nbd

import (
	"context"
	"errors"
	"fmt"
	"io"
	"net"
	"sync"
	"testing"
	"time"
)

// The harness: a session on one end of a net.Pipe, and a test client on the
// other that speaks the protocol by hand. Nothing here opens a socket or a
// file, so the session logic is tested in isolation (master Rule 16).

// ramVolume is a volume in memory, with switches for the failures a real
// file can produce.
type ramVolume struct {
	mu        sync.Mutex
	data      []byte
	readOnly  bool
	failRead  error
	failWrite error
	failFlush error
	flushes   int
	closed    bool
}

func newRAMVolume(size int, readOnly bool) *ramVolume {
	v := &ramVolume{data: make([]byte, size), readOnly: readOnly}
	for i := range v.data {
		v.data[i] = byte(i*31 + 7)
	}
	return v
}

func (v *ramVolume) Size() int64    { return int64(len(v.data)) }
func (v *ramVolume) ReadOnly() bool { return v.readOnly }

func (v *ramVolume) ReadAt(p []byte, off int64) (int, error) {
	v.mu.Lock()
	defer v.mu.Unlock()
	if v.failRead != nil {
		return 0, v.failRead
	}
	if off < 0 || off > int64(len(v.data)) || int64(len(p)) > int64(len(v.data))-off {
		return 0, ErrRange
	}
	return copy(p, v.data[off:]), nil
}

func (v *ramVolume) WriteAt(p []byte, off int64) (int, error) {
	v.mu.Lock()
	defer v.mu.Unlock()
	if v.failWrite != nil {
		return 0, v.failWrite
	}
	if v.readOnly {
		return 0, ErrReadOnly
	}
	if off < 0 || off > int64(len(v.data)) || int64(len(p)) > int64(len(v.data))-off {
		return 0, ErrRange
	}
	return copy(v.data[off:], p), nil
}

func (v *ramVolume) Flush() error {
	v.mu.Lock()
	defer v.mu.Unlock()
	v.flushes++
	return v.failFlush
}

func (v *ramVolume) Close() error {
	v.mu.Lock()
	defer v.mu.Unlock()
	v.closed = true
	return nil
}

// ramStore is a fixed set of named volumes.
type ramStore struct {
	volumes map[string]*ramVolume
	opens   int
}

func (s *ramStore) Open(name string) (Volume, error) {
	s.opens++
	if name == "" {
		name = "default"
	}
	if v, ok := s.volumes[name]; ok {
		return v, nil
	}
	return nil, fmt.Errorf("%w: %s", ErrNoExport, name)
}

func (s *ramStore) List() []string {
	names := make([]string, 0, len(s.volumes))
	for name := range s.volumes {
		names = append(names, name)
	}
	// A stable order, so a test can assert it.
	for i := 0; i < len(names); i++ {
		for j := i + 1; j < len(names); j++ {
			if names[j] < names[i] {
				names[i], names[j] = names[j], names[i]
			}
		}
	}
	return names
}

type testLogger struct {
	mu    sync.Mutex
	lines []string
}

func (l *testLogger) Printf(format string, args ...any) {
	l.mu.Lock()
	defer l.mu.Unlock()
	l.lines = append(l.lines, fmt.Sprintf(format, args...))
}

type pipeConn struct {
	net.Conn
}

func (pipeConn) RemoteName() string { return "pipe" }

// session is one running service plus the client's end of its pipe.
type session struct {
	t      *testing.T
	client net.Conn
	store  *ramStore
	log    *testLogger
	done   chan error
}

func start(t *testing.T, volumes map[string]*ramVolume) *session {
	t.Helper()
	serverEnd, clientEnd := net.Pipe()
	s := &session{
		t:      t,
		client: clientEnd,
		store:  &ramStore{volumes: volumes},
		log:    &testLogger{},
		done:   make(chan error, 1),
	}
	svc := NewService(s.store, s.log)
	go func() { s.done <- svc.Serve(context.Background(), pipeConn{serverEnd}) }()
	t.Cleanup(func() { _ = clientEnd.Close() })
	return s
}

// wait returns how the session ended, or fails the test if it hangs.
func (s *session) wait() error {
	s.t.Helper()
	select {
	case err := <-s.done:
		return err
	case <-time.After(5 * time.Second):
		s.t.Fatal("the session did not end")
		return nil
	}
}

func (s *session) send(b []byte) {
	s.t.Helper()
	if _, err := s.client.Write(b); err != nil {
		s.t.Fatalf("write to the session: %v", err)
	}
}

// sendAsync writes in the background and ignores what happens. A test that
// is deliberately racing a session's shutdown cannot treat a short write as
// a failure — and a Fatalf from a goroutine after the test has finished
// panics the whole run rather than failing one case.
func (s *session) sendAsync(b []byte) {
	go func() { _, _ = s.client.Write(b) }()
}

func (s *session) read(n int) []byte {
	s.t.Helper()
	buf := make([]byte, n)
	// Reading nothing needs no connection. Touching the pipe anyway made
	// this harness fail once in forty runs: an option reply with an empty
	// body raced the session's own correct ending, and the deadline call
	// on a pipe whose far end had closed was what failed.
	if n == 0 {
		return buf
	}
	if err := s.client.SetReadDeadline(time.Now().Add(5 * time.Second)); err != nil {
		s.t.Fatalf("the session is gone before reading %d bytes: %v%s", n, err, s.why())
	}
	if _, err := io.ReadFull(s.client, buf); err != nil {
		s.t.Fatalf("read %d bytes from the session: %v%s", n, err, s.why())
	}
	return buf
}

// why reports how the session ended, when it has, and what it logged: a
// test that finds the connection closed should say what closed it rather
// than only that it was closed.
func (s *session) why() string {
	var reason string
	select {
	case err := <-s.done:
		reason = fmt.Sprintf(" (the session ended with: %v)", err)
	default:
		reason = " (the session is still running)"
	}
	if s.log != nil {
		s.log.mu.Lock()
		defer s.log.mu.Unlock()
		for _, line := range s.log.lines {
			reason += "\n\tlog: " + line
		}
	}
	return reason
}

// greet reads the server's greeting and answers it.
func (s *session) greet(clientFlags uint32) {
	s.t.Helper()
	hello := s.read(18)
	if be.Uint64(hello) != magicGreeting || be.Uint64(hello[8:]) != magicOption {
		s.t.Fatalf("greeting magic: %x", hello[:16])
	}
	if flags := be.Uint16(hello[16:]); flags&flagFixedNewstyle == 0 {
		s.t.Fatalf("the server must offer fixed newstyle, got %04x", flags)
	}
	buf := make([]byte, 4)
	be.PutUint32(buf, clientFlags)
	s.send(buf)
}

func optionBytes(id uint32, payload []byte) []byte {
	buf := make([]byte, optionHeadBytes+len(payload))
	be.PutUint64(buf, magicOption)
	be.PutUint32(buf[8:], id)
	be.PutUint32(buf[12:], uint32(len(payload)))
	copy(buf[optionHeadBytes:], payload)
	return buf
}

func goPayloadBytes(name string) []byte {
	buf := make([]byte, 4+len(name)+2)
	be.PutUint32(buf, uint32(len(name)))
	copy(buf[4:], name)
	return buf
}

// optReply is one option reply, body included.
type optReply struct {
	option uint32
	typ    uint32
	body   []byte
}

func (s *session) readOptReply() optReply {
	s.t.Helper()
	head := s.read(optReplyBytes)
	if be.Uint64(head) != magicOptReply {
		s.t.Fatalf("option reply magic: %#x", be.Uint64(head))
	}
	length := be.Uint32(head[16:])
	if length > optionPayloadMax {
		s.t.Fatalf("the server sent a %d-byte option reply", length)
	}
	return optReply{option: be.Uint32(head[8:]), typ: be.Uint32(head[12:]), body: s.read(int(length))}
}

// goTo runs NBD_OPT_GO and returns the export's size and flags.
func (s *session) goTo(name string) (uint64, uint16) {
	s.t.Helper()
	s.send(optionBytes(optGo, goPayloadBytes(name)))
	var size uint64
	var flags uint16
	for {
		rep := s.readOptReply()
		switch {
		case rep.typ == repAck:
			return size, flags
		case rep.typ == repInfo && be.Uint16(rep.body) == infoExport:
			size, flags = be.Uint64(rep.body[2:]), be.Uint16(rep.body[10:])
		case rep.typ&repError != 0:
			s.t.Fatalf("NBD_OPT_GO was refused: %#x", rep.typ)
		}
	}
}

func requestBytesFor(cmd uint16, handle, off uint64, length uint32) []byte {
	buf := make([]byte, requestBytes)
	be.PutUint32(buf, magicRequest)
	be.PutUint16(buf[6:], cmd)
	be.PutUint64(buf[8:], handle)
	be.PutUint64(buf[16:], off)
	be.PutUint32(buf[24:], length)
	return buf
}

// reply reads one simple reply and checks the handle it echoes.
func (s *session) reply(handle uint64) uint32 {
	s.t.Helper()
	rep := s.read(replyBytes)
	if be.Uint32(rep) != magicReply {
		s.t.Fatalf("reply magic: %#x", be.Uint32(rep))
	}
	if got := be.Uint64(rep[8:]); got != handle {
		s.t.Fatalf("reply handle %d, want %d", got, handle)
	}
	return be.Uint32(rep[4:])
}

func oneVolume(size int, readOnly bool) map[string]*ramVolume {
	return map[string]*ramVolume{"vol": newRAMVolume(size, readOnly)}
}

// ── the tests ───────────────────────────────────────────────────────────────

func TestHandshakeWithOptGo(t *testing.T) {
	s := start(t, oneVolume(8192, false))
	s.greet(clientFixedNewstyle | clientNoZeroes)
	size, flags := s.goTo("vol")
	if size != 8192 {
		t.Errorf("export size %d, want 8192", size)
	}
	if flags&FlagHasFlags == 0 || flags&FlagSendFlush == 0 || flags&FlagReadOnly != 0 {
		t.Errorf("transmission flags %04x", flags)
	}
	s.send(requestBytesFor(cmdDisconnect, 1, 0, 0))
	if err := s.wait(); !errors.Is(err, ErrClosed) {
		t.Errorf("the session ended with %v, want ErrClosed", err)
	}
}

func TestReadAndWrite(t *testing.T) {
	volumes := oneVolume(8192, false)
	s := start(t, volumes)
	s.greet(clientFixedNewstyle)
	s.goTo("vol")

	s.send(requestBytesFor(cmdRead, 10, 512, 256))
	if err := s.reply(10); err != errNone {
		t.Fatalf("read failed with %d", err)
	}
	got := s.read(256)
	want := make([]byte, 256)
	if _, err := volumes["vol"].ReadAt(want, 512); err != nil {
		t.Fatal(err)
	}
	if string(got) != string(want) {
		t.Error("the bytes read are not the volume's")
	}

	payload := make([]byte, 300)
	for i := range payload {
		payload[i] = 0xa7
	}
	s.send(append(requestBytesFor(cmdWrite, 11, 1000, 300), payload...))
	if err := s.reply(11); err != errNone {
		t.Fatalf("write failed with %d", err)
	}
	if volumes["vol"].data[1000] != 0xa7 || volumes["vol"].data[1299] != 0xa7 {
		t.Error("the write did not land")
	}
	if volumes["vol"].data[999] == 0xa7 || volumes["vol"].data[1300] == 0xa7 {
		t.Error("the write spilled outside its range")
	}

	s.send(requestBytesFor(cmdFlush, 12, 0, 0))
	if err := s.reply(12); err != errNone {
		t.Fatalf("flush failed with %d", err)
	}
	if volumes["vol"].flushes != 1 {
		t.Errorf("flushes = %d, want 1", volumes["vol"].flushes)
	}
}

// A transfer larger than one chunk must still be moved whole.
func TestATransferLargerThanTheChunk(t *testing.T) {
	size := chunkBytes*2 + 1024
	volumes := map[string]*ramVolume{"vol": newRAMVolume(size, false)}
	s := start(t, volumes)
	s.greet(clientFixedNewstyle)
	s.goTo("vol")
	payload := make([]byte, size)
	for i := range payload {
		payload[i] = byte(i % 251)
	}
	s.sendAsync(append(requestBytesFor(cmdWrite, 1, 0, uint32(size)), payload...))
	if err := s.reply(1); err != errNone {
		t.Fatalf("a big write failed with %d", err)
	}
	s.send(requestBytesFor(cmdRead, 2, 0, uint32(size)))
	if err := s.reply(2); err != errNone {
		t.Fatalf("a big read failed with %d", err)
	}
	if string(s.read(size)) != string(payload) {
		t.Error("what came back is not what went in")
	}
}

func TestOutOfRangeIsAnErrorReplyNotAClose(t *testing.T) {
	s := start(t, oneVolume(4096, false))
	s.greet(clientFixedNewstyle)
	s.goTo("vol")
	s.send(requestBytesFor(cmdRead, 1, 4096, 1))
	if err := s.reply(1); err != errInvalid {
		t.Errorf("a read past the end: got %d, want %d", err, errInvalid)
	}
	s.send(requestBytesFor(cmdRead, 2, 4000, 1000))
	if err := s.reply(2); err != errInvalid {
		t.Errorf("a read overrunning the end: got %d, want %d", err, errInvalid)
	}
	// The connection is still usable: that is the point.
	s.send(requestBytesFor(cmdRead, 3, 0, 16))
	if err := s.reply(3); err != errNone {
		t.Errorf("the session did not survive a refused read: %d", err)
	}
	s.read(16)
}

// A write whose range is bad must have its payload drained, or the data
// would be read as the next request.
func TestARefusedWriteStaysInStep(t *testing.T) {
	volumes := oneVolume(4096, false)
	s := start(t, volumes)
	s.greet(clientFixedNewstyle)
	s.goTo("vol")
	payload := make([]byte, 64)
	for i := range payload {
		payload[i] = 0xff
	}
	s.send(append(requestBytesFor(cmdWrite, 1, 4064, 64), payload...))
	if err := s.reply(1); err != errInvalid {
		t.Fatalf("the write should have been refused, got %d", err)
	}
	s.send(requestBytesFor(cmdRead, 2, 0, 8))
	if err := s.reply(2); err != errNone {
		t.Fatalf("the stream is out of step: %d", err)
	}
	s.read(8)
	if volumes["vol"].data[4064] == 0xff {
		t.Error("a refused write still landed")
	}
}

func TestAReadOnlyExportRefusesWrites(t *testing.T) {
	volumes := oneVolume(4096, true)
	s := start(t, volumes)
	s.greet(clientFixedNewstyle)
	_, flags := s.goTo("vol")
	if flags&FlagReadOnly == 0 {
		t.Fatalf("a read-only export must say so: %04x", flags)
	}
	payload := make([]byte, 16)
	s.send(append(requestBytesFor(cmdWrite, 1, 0, 16), payload...))
	if err := s.reply(1); err != errReadOnly {
		t.Errorf("got %d, want EROFS (%d)", err, errReadOnly)
	}
	if volumes["vol"].data[0] != byte(7) {
		t.Error("the volume changed")
	}
	// Flush on a read-only export is a no-op that still succeeds.
	s.send(requestBytesFor(cmdFlush, 2, 0, 0))
	if err := s.reply(2); err != errNone {
		t.Errorf("flush on a read-only export: %d", err)
	}
}

func TestExportNameHandshake(t *testing.T) {
	s := start(t, oneVolume(2048, false))
	s.greet(clientFixedNewstyle) // no NO_ZEROES: the 124 bytes must follow
	s.send(optionBytes(optExportName, []byte("vol")))
	head := s.read(10)
	if be.Uint64(head) != 2048 {
		t.Errorf("size %d, want 2048", be.Uint64(head))
	}
	s.read(124)
	s.send(requestBytesFor(cmdRead, 1, 0, 4))
	if err := s.reply(1); err != errNone {
		t.Fatalf("read after EXPORT_NAME: %d", err)
	}
	s.read(4)
}

func TestExportNameWithNoZeroes(t *testing.T) {
	s := start(t, oneVolume(2048, false))
	s.greet(clientFixedNewstyle | clientNoZeroes)
	s.send(optionBytes(optExportName, []byte("vol")))
	head := s.read(10)
	if be.Uint64(head) != 2048 {
		t.Errorf("size %d, want 2048", be.Uint64(head))
	}
	// Straight into transmission: no padding to skip.
	s.send(requestBytesFor(cmdRead, 1, 0, 4))
	if err := s.reply(1); err != errNone {
		t.Fatalf("read after EXPORT_NAME: %d", err)
	}
	s.read(4)
}

func TestTheDefaultExport(t *testing.T) {
	s := start(t, map[string]*ramVolume{"default": newRAMVolume(1024, false)})
	s.greet(clientFixedNewstyle)
	if size, _ := s.goTo(""); size != 1024 {
		t.Errorf("the empty name should open the default export, got size %d", size)
	}
}

// A refused export must leave the handshake open: a client that asks for
// something it cannot have needs to be able to ask for something else, or
// to list what is on offer. nbdinfo --list does exactly this.
func TestAnUnknownExportIsRefusedWithoutClosing(t *testing.T) {
	s := start(t, oneVolume(1024, false))
	s.greet(clientFixedNewstyle)
	s.send(optionBytes(optGo, goPayloadBytes("nope")))
	if rep := s.readOptReply(); rep.typ != repErrUnknown {
		t.Errorf("reply type %#x, want NBD_REP_ERR_UNKNOWN", rep.typ)
	}
	// The connection is still in the option phase.
	s.send(optionBytes(optList, nil))
	for {
		if rep := s.readOptReply(); rep.typ == repAck {
			break
		}
	}
	if size, _ := s.goTo("vol"); size != 1024 {
		t.Errorf("size %d after a refusal, want 1024", size)
	}
	s.send(requestBytesFor(cmdDisconnect, 1, 0, 0))
	if err := s.wait(); !errors.Is(err, ErrClosed) {
		t.Errorf("the session ended with %v, want ErrClosed", err)
	}
}

func TestInfoDoesNotEnterTransmission(t *testing.T) {
	s := start(t, oneVolume(4096, false))
	s.greet(clientFixedNewstyle)
	s.send(optionBytes(optInfo, goPayloadBytes("vol")))
	for {
		if rep := s.readOptReply(); rep.typ == repAck {
			break
		}
	}
	// Still in the option phase, so NBD_OPT_GO works now.
	if size, _ := s.goTo("vol"); size != 4096 {
		t.Errorf("size after INFO then GO: %d", size)
	}
	if s.store.opens != 2 {
		t.Errorf("the store was opened %d times, want 2", s.store.opens)
	}
}

func TestList(t *testing.T) {
	s := start(t, map[string]*ramVolume{"beta": newRAMVolume(512, false), "alpha": newRAMVolume(512, false)})
	s.greet(clientFixedNewstyle)
	s.send(optionBytes(optList, nil))
	var names []string
	for {
		rep := s.readOptReply()
		if rep.typ == repAck {
			break
		}
		if rep.typ != repServer {
			t.Fatalf("reply type %#x during LIST", rep.typ)
		}
		names = append(names, string(rep.body[4:]))
	}
	if len(names) != 2 || names[0] != "alpha" || names[1] != "beta" {
		t.Errorf("LIST gave %v", names)
	}
}

func TestAbort(t *testing.T) {
	s := start(t, oneVolume(512, false))
	s.greet(clientFixedNewstyle)
	s.send(optionBytes(optAbort, nil))
	if rep := s.readOptReply(); rep.typ != repAck {
		t.Errorf("abort reply %#x, want ack", rep.typ)
	}
	if err := s.wait(); !errors.Is(err, ErrClosed) {
		t.Errorf("the session ended with %v, want ErrClosed", err)
	}
}

func TestAnUnknownOptionIsUnsupportedNotFatal(t *testing.T) {
	s := start(t, oneVolume(512, false))
	s.greet(clientFixedNewstyle)
	s.send(optionBytes(optStructuredReply, nil))
	if rep := s.readOptReply(); rep.typ != repErrUnsup {
		t.Errorf("reply type %#x, want NBD_REP_ERR_UNSUP", rep.typ)
	}
	s.send(optionBytes(9999, []byte("whatever")))
	if rep := s.readOptReply(); rep.typ != repErrUnsup {
		t.Errorf("reply type %#x, want NBD_REP_ERR_UNSUP", rep.typ)
	}
	// And the handshake still works afterwards.
	if size, _ := s.goTo("vol"); size != 512 {
		t.Errorf("size %d after unknown options", size)
	}
}

func TestAnOldstyleClientIsRefused(t *testing.T) {
	s := start(t, oneVolume(512, false))
	s.greet(0) // no NBD_FLAG_C_FIXED_NEWSTYLE
	if err := s.wait(); !errors.Is(err, ErrOldstyle) {
		t.Errorf("the session ended with %v, want ErrOldstyle", err)
	}
}

func TestAnUnknownCommandIsNotSupported(t *testing.T) {
	s := start(t, oneVolume(512, false))
	s.greet(clientFixedNewstyle)
	s.goTo("vol")
	s.send(requestBytesFor(cmdWriteZeroes, 1, 0, 0))
	if err := s.reply(1); err != errNotSup {
		t.Errorf("got %d, want ENOTSUP (%d)", err, errNotSup)
	}
	s.send(requestBytesFor(cmdTrim, 2, 0, 0))
	if err := s.reply(2); err != errNotSup {
		t.Errorf("got %d, want ENOTSUP (%d)", err, errNotSup)
	}
}

func TestABadRequestMagicEndsTheConnection(t *testing.T) {
	s := start(t, oneVolume(512, false))
	s.greet(clientFixedNewstyle)
	s.goTo("vol")
	bad := requestBytesFor(cmdRead, 1, 0, 16)
	be.PutUint32(bad, 0xdeadbeef)
	s.send(bad)
	if err := s.wait(); !errors.Is(err, ErrProtocol) {
		t.Errorf("the session ended with %v, want ErrProtocol", err)
	}
}

func TestAFailingVolumeIsReported(t *testing.T) {
	volumes := oneVolume(4096, false)
	volumes["vol"].failFlush = errors.New("the disk is full of bees")
	s := start(t, volumes)
	s.greet(clientFixedNewstyle)
	s.goTo("vol")
	s.send(requestBytesFor(cmdFlush, 1, 0, 0))
	if err := s.reply(1); err != errIO {
		t.Errorf("a failed flush: got %d, want EIO (%d)", err, errIO)
	}
}

// The volume must be closed when the session ends, whatever ended it.
func TestTheVolumeIsClosed(t *testing.T) {
	volumes := oneVolume(512, false)
	s := start(t, volumes)
	s.greet(clientFixedNewstyle)
	s.goTo("vol")
	s.send(requestBytesFor(cmdDisconnect, 1, 0, 0))
	_ = s.wait()
	if !volumes["vol"].closed {
		t.Error("the volume was left open")
	}
}

func TestACancelledContextEndsTheSession(t *testing.T) {
	serverEnd, clientEnd := net.Pipe()
	defer func() { _ = clientEnd.Close() }()
	store := &ramStore{volumes: oneVolume(512, false)}
	svc := NewService(store, &testLogger{})
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- svc.Serve(ctx, pipeConn{serverEnd}) }()
	s := &session{t: t, client: clientEnd, store: store, done: done}
	s.greet(clientFixedNewstyle)
	s.goTo("vol")
	// Cancelling must end the session on its own, with nothing else
	// happening on the connection: a server that only noticed between
	// requests would wait out its idle deadline on a quiet client.
	cancel()
	if err := s.wait(); !errors.Is(err, context.Canceled) {
		t.Errorf("the session ended with %v, want context.Canceled", err)
	}
}
