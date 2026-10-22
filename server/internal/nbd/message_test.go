package nbd

import (
	"bytes"
	"errors"
	"io"
	"strings"
	"testing"
)

// The wire, on its own: every length field here is the peer's to choose, so
// each one is tested with a value no honest peer would send.

func TestWriteGreeting(t *testing.T) {
	var buf bytes.Buffer
	if err := writeGreeting(&buf); err != nil {
		t.Fatal(err)
	}
	if buf.Len() != 18 {
		t.Fatalf("greeting is %d bytes, want 18", buf.Len())
	}
	b := buf.Bytes()
	if be.Uint64(b) != magicGreeting || be.Uint64(b[8:]) != magicOption {
		t.Error("the greeting's magics are wrong")
	}
	if flags := be.Uint16(b[16:]); flags != flagFixedNewstyle|flagNoZeroes {
		t.Errorf("handshake flags %04x", flags)
	}
}

func TestReadClientFlags(t *testing.T) {
	cases := []struct {
		name     string
		flags    uint32
		noZeroes bool
		wantErr  error
	}{
		{"fixed newstyle", clientFixedNewstyle, false, nil},
		{"and no zeroes", clientFixedNewstyle | clientNoZeroes, true, nil},
		{"oldstyle", 0, false, ErrOldstyle},
		{"no zeroes alone is still oldstyle", clientNoZeroes, false, ErrOldstyle},
		{"unknown bits are ignored", clientFixedNewstyle | 0x8000, false, nil},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			buf := make([]byte, 4)
			be.PutUint32(buf, c.flags)
			noZeroes, err := readClientFlags(bytes.NewReader(buf))
			if !errors.Is(err, c.wantErr) {
				t.Fatalf("err = %v, want %v", err, c.wantErr)
			}
			if err == nil && noZeroes != c.noZeroes {
				t.Errorf("noZeroes = %v, want %v", noZeroes, c.noZeroes)
			}
		})
	}
}

func TestReadClientFlagsTruncated(t *testing.T) {
	_, err := readClientFlags(bytes.NewReader([]byte{0, 0}))
	if !errors.Is(err, ErrProtocol) {
		t.Errorf("err = %v, want ErrProtocol", err)
	}
	_, err = readClientFlags(bytes.NewReader(nil))
	if !errors.Is(err, ErrClosed) {
		t.Errorf("an immediate EOF should be ErrClosed, got %v", err)
	}
}

func TestReadOption(t *testing.T) {
	opt, err := readOption(bytes.NewReader(optionBytes(optGo, []byte("hello"))))
	if err != nil {
		t.Fatal(err)
	}
	if opt.ID != optGo || string(opt.Payload) != "hello" {
		t.Errorf("option = %+v", opt)
	}
}

func TestReadOptionRejectsBadMagic(t *testing.T) {
	raw := optionBytes(optGo, nil)
	be.PutUint64(raw, 0x1122334455667788)
	_, err := readOption(bytes.NewReader(raw))
	if !errors.Is(err, ErrProtocol) {
		t.Errorf("err = %v, want ErrProtocol", err)
	}
}

// A length field of 4 GiB must be refused by the reader, not allocated.
func TestReadOptionRejectsAGiantPayload(t *testing.T) {
	raw := make([]byte, optionHeadBytes)
	be.PutUint64(raw, magicOption)
	be.PutUint32(raw[8:], optGo)
	be.PutUint32(raw[12:], 0xffffffff)
	_, err := readOption(bytes.NewReader(raw))
	if !errors.Is(err, ErrProtocol) {
		t.Fatalf("err = %v, want ErrProtocol", err)
	}
	// One byte over the limit is refused as well as an absurd value.
	be.PutUint32(raw[12:], optionPayloadMax+1)
	if _, err := readOption(bytes.NewReader(raw)); !errors.Is(err, ErrProtocol) {
		t.Errorf("err = %v, want ErrProtocol", err)
	}
	be.PutUint32(raw[12:], optionPayloadMax)
	body := append(raw, make([]byte, optionPayloadMax)...)
	if _, err := readOption(bytes.NewReader(body)); err != nil {
		t.Errorf("the limit itself must be allowed: %v", err)
	}
}

func TestExportNamePayload(t *testing.T) {
	name, err := exportNamePayload([]byte("vol"))
	if err != nil || name != "vol" {
		t.Errorf("name = %q, err = %v", name, err)
	}
	if _, err := exportNamePayload(nil); err != nil {
		t.Errorf("an empty name is the default export, not an error: %v", err)
	}
	long := []byte(strings.Repeat("a", exportNameMax+1))
	if _, err := exportNamePayload(long); !errors.Is(err, ErrBadName) {
		t.Errorf("err = %v, want ErrBadName", err)
	}
}

func TestGoPayload(t *testing.T) {
	name, err := goPayload(goPayloadBytes("vol"))
	if err != nil || name != "vol" {
		t.Errorf("name = %q, err = %v", name, err)
	}
	if _, err := goPayload(goPayloadBytes("")); err != nil {
		t.Errorf("the default export: %v", err)
	}
}

// The name length inside the payload is the second place a peer can lie.
func TestGoPayloadRejectsBadLengths(t *testing.T) {
	cases := []struct {
		name    string
		payload []byte
	}{
		{"empty", nil},
		{"shorter than the length field", []byte{0, 0}},
		{"a name longer than the payload", func() []byte {
			b := make([]byte, 10)
			be.PutUint32(b, 1000)
			return b
		}()},
		{"a name of 4 GiB", func() []byte {
			b := make([]byte, 10)
			be.PutUint32(b, 0xffffffff)
			return b
		}()},
		{"no room for the request count", func() []byte {
			b := make([]byte, 4+3)
			be.PutUint32(b, 3)
			return b
		}()},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			if _, err := goPayload(c.payload); err == nil {
				t.Error("this should not have been accepted")
			}
		})
	}
}

func TestInfoBodies(t *testing.T) {
	info := ExportInfo{Name: "vol", Size: 1 << 20}
	body := infoExportBody(info)
	if be.Uint16(body) != infoExport || be.Uint64(body[2:]) != 1<<20 {
		t.Error("NBD_INFO_EXPORT is wrong")
	}
	if be.Uint16(body[10:]) != info.TransmissionFlags() {
		t.Error("the flags in NBD_INFO_EXPORT do not match the export")
	}
	bs := infoBlockSizeBody()
	if be.Uint16(bs) != infoBlockSize || be.Uint32(bs[2:]) != MinBlock ||
		be.Uint32(bs[6:]) != PrefBlock || be.Uint32(bs[10:]) != MaxIO {
		t.Error("NBD_INFO_BLOCK_SIZE is wrong")
	}
	n := infoNameBody("vol")
	if be.Uint16(n) != infoName || string(n[2:]) != "vol" {
		t.Error("NBD_INFO_NAME is wrong")
	}
}

func TestWriteOptionReply(t *testing.T) {
	var buf bytes.Buffer
	if err := writeOptionReply(&buf, optGo, repInfo, []byte("body")); err != nil {
		t.Fatal(err)
	}
	b := buf.Bytes()
	if be.Uint64(b) != magicOptReply || be.Uint32(b[8:]) != optGo || be.Uint32(b[12:]) != repInfo {
		t.Error("the option reply header is wrong")
	}
	if be.Uint32(b[16:]) != 4 || string(b[optReplyBytes:]) != "body" {
		t.Error("the option reply body is wrong")
	}
}

func TestWriteExportNameReply(t *testing.T) {
	var withZeroes, without bytes.Buffer
	info := ExportInfo{Size: 4096}
	if err := writeExportNameReply(&withZeroes, info, false); err != nil {
		t.Fatal(err)
	}
	if err := writeExportNameReply(&without, info, true); err != nil {
		t.Fatal(err)
	}
	if withZeroes.Len() != 134 || without.Len() != 10 {
		t.Errorf("lengths: %d and %d, want 134 and 10", withZeroes.Len(), without.Len())
	}
}

func TestReadRequest(t *testing.T) {
	req, err := readRequest(bytes.NewReader(requestBytesFor(cmdWrite, 7, 1024, 512)))
	if err != nil {
		t.Fatal(err)
	}
	if req.Type != cmdWrite || req.Handle != 7 || req.Offset != 1024 || req.Length != 512 {
		t.Errorf("request = %+v", req)
	}
}

func TestReadRequestRejectsBadMagicAndTruncation(t *testing.T) {
	bad := requestBytesFor(cmdRead, 1, 0, 0)
	be.PutUint32(bad, 0)
	if _, err := readRequest(bytes.NewReader(bad)); !errors.Is(err, ErrProtocol) {
		t.Errorf("bad magic: err = %v", err)
	}
	short := requestBytesFor(cmdRead, 1, 0, 0)[:20]
	if _, err := readRequest(bytes.NewReader(short)); !errors.Is(err, ErrProtocol) {
		t.Errorf("truncated: err = %v", err)
	}
	if _, err := readRequest(bytes.NewReader(nil)); !errors.Is(err, ErrClosed) {
		t.Errorf("a clean close should be ErrClosed, got %v", err)
	}
}

func TestWriteReply(t *testing.T) {
	var buf bytes.Buffer
	if err := writeReply(&buf, 42, errInvalid); err != nil {
		t.Fatal(err)
	}
	b := buf.Bytes()
	if len(b) != replyBytes || be.Uint32(b) != magicReply {
		t.Error("the reply header is wrong")
	}
	if be.Uint32(b[4:]) != errInvalid || be.Uint64(b[8:]) != 42 {
		t.Error("the reply's error or handle is wrong")
	}
}

// A writer that fails must surface, not be swallowed.
type failWriter struct{}

func (failWriter) Write([]byte) (int, error) { return 0, io.ErrClosedPipe }

func TestWriteErrorsSurface(t *testing.T) {
	if err := writeGreeting(failWriter{}); !errors.Is(err, io.ErrClosedPipe) {
		t.Errorf("writeGreeting: %v", err)
	}
	if err := writeReply(failWriter{}, 1, 0); !errors.Is(err, io.ErrClosedPipe) {
		t.Errorf("writeReply: %v", err)
	}
	if err := writeOptionReply(failWriter{}, optGo, repAck, nil); !errors.Is(err, io.ErrClosedPipe) {
		t.Errorf("writeOptionReply: %v", err)
	}
}

func BenchmarkReadRequest(b *testing.B) {
	raw := requestBytesFor(cmdRead, 1, 4096, 512)
	r := bytes.NewReader(raw)
	for b.Loop() {
		r.Reset(raw)
		if _, err := readRequest(r); err != nil {
			b.Fatal(err)
		}
	}
}

func BenchmarkWriteReply(b *testing.B) {
	var buf bytes.Buffer
	for b.Loop() {
		buf.Reset()
		if err := writeReply(&buf, 1, 0); err != nil {
			b.Fatal(err)
		}
	}
}
