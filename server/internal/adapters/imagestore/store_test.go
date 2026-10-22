package imagestore

import (
	"errors"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/mateuszmierzwinski/arm_minikern/server/internal/mknb"
)

func board(mac [6]byte, id string) mknb.BoardID {
	b := mknb.BoardID{MAC: mac}
	copy(b.ID[:], id)
	return b
}

var testMAC = [6]byte{0x02, 0x64, 0xb7, 0x00, 0x81, 0x11}

// write puts a file in dir and returns its path.
func write(t *testing.T, dir, name, content string) string {
	t.Helper()
	path := filepath.Join(dir, name)
	if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
		t.Fatal(err)
	}
	return path
}

func TestResolutionOrder(t *testing.T) {
	dir := t.TempDir()
	b := board(testMAC, "chip")
	s := New(dir)
	write(t, dir, "default.tar", "default image")
	img, err := s.Image(b)
	if err != nil || img.Name != "default.tar" {
		t.Fatalf("got %v, %v", img, err)
	}
	write(t, dir, "0264b7008111.tar", "per-mac image")
	img, _ = s.Image(b)
	if img.Name != "0264b7008111.tar" {
		t.Fatalf("the MAC file did not win: %s", img.Name)
	}
	write(t, dir, b.Key()+".tar", "per-board image")
	img, _ = s.Image(b)
	if img.Name != b.Key()+".tar" {
		t.Fatalf("the board file did not win: %s", img.Name)
	}
	if string(img.Data) != "per-board image" {
		t.Fatalf("got %q", img.Data)
	}
}

func TestNoImageIsReported(t *testing.T) {
	s := New(t.TempDir())
	_, err := s.Image(board(testMAC, "chip"))
	if !errors.Is(err, mknb.ErrNoImage) {
		t.Fatalf("got %v, want ErrNoImage", err)
	}
}

func TestEmptyFileIsRefused(t *testing.T) {
	dir := t.TempDir()
	write(t, dir, "default.tar", "")
	_, err := New(dir).Image(board(testMAC, "chip"))
	if !errors.Is(err, mknb.ErrNoImage) {
		t.Fatalf("an empty image gave %v", err)
	}
}

// A directory in place of an image is not a regular file, so it is skipped.
func TestDirectoryIsNotAnImage(t *testing.T) {
	dir := t.TempDir()
	if err := os.Mkdir(filepath.Join(dir, "default.tar"), 0o755); err != nil {
		t.Fatal(err)
	}
	if _, err := New(dir).Image(board(testMAC, "chip")); !errors.Is(err, mknb.ErrNoImage) {
		t.Fatalf("got %v", err)
	}
}

func TestDigestMatchesTheBytes(t *testing.T) {
	dir := t.TempDir()
	write(t, dir, "default.tar", "abc")
	img, err := New(dir).Image(board(testMAC, "chip"))
	if err != nil {
		t.Fatal(err)
	}
	const want = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
	if img.DigestString() != want {
		t.Fatalf("got %s", img.DigestString())
	}
}

// The cache must hand out the same image until the file really changes,
// so a transfer cannot straddle two versions.
func TestCacheIsStableUntilTheFileChanges(t *testing.T) {
	dir := t.TempDir()
	b := board(testMAC, "chip")
	s := New(dir)
	path := write(t, dir, "default.tar", "first")
	first, _ := s.Image(b)
	again, _ := s.Image(b)
	if first != again {
		t.Fatal("the cache returned a different image for an unchanged file")
	}
	if err := os.WriteFile(path, []byte("second!"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.Chtimes(path, time.Now().Add(time.Second), time.Now().Add(time.Second)); err != nil {
		t.Fatal(err)
	}
	changed, _ := s.Image(b)
	if string(changed.Data) != "second!" || changed == first {
		t.Fatalf("the change was missed: %q", changed.Data)
	}
}

// The size limit stops one board's image from exhausting the server.
func TestOversizedImageIsRefused(t *testing.T) {
	dir := t.TempDir()
	write(t, dir, "default.tar", "0123456789")
	s := New(dir).(*Store)
	s.maxLen = 4
	if _, err := s.Image(board(testMAC, "chip")); err == nil {
		t.Fatal("an oversized image was accepted")
	}
}

// CVE class (TFTP's classic): a board id full of traversal sequences must
// never escape the image directory.
func TestCVEBoardIDCannotEscapeTheDirectory(t *testing.T) {
	root := t.TempDir()
	dir := filepath.Join(root, "images")
	if err := os.Mkdir(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	write(t, root, "secret.tar", "not for the network")
	s := New(dir).(*Store)
	for _, attack := range []string{"../secret", "../../etc/passwd", "/etc/passwd", "....//secret"} {
		b := board(testMAC, attack)
		copy(b.MAC[:], attack)
		for _, name := range s.candidates(b) {
			joined := filepath.Join(dir, name)
			if rel, err := filepath.Rel(dir, joined); err != nil || rel != name {
				t.Fatalf("%q produced the path %q", attack, joined)
			}
		}
		if _, err := s.Image(b); !errors.Is(err, mknb.ErrNoImage) {
			t.Fatalf("%q resolved to something: %v", attack, err)
		}
	}
}

func BenchmarkImageCached(b *testing.B) {
	dir := b.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "default.tar"), make([]byte, 1<<20), 0o644); err != nil {
		b.Fatal(err)
	}
	s := New(dir)
	brd := board(testMAC, "chip")
	if _, err := s.Image(brd); err != nil {
		b.Fatal(err)
	}
	for b.Loop() {
		if _, err := s.Image(brd); err != nil {
			b.Fatal(err)
		}
	}
}
