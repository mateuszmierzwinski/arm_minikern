package volumefile

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/mateuszmierzwinski/arm_minikern/server/internal/nbd"
)

// makeDir builds a volume directory with the named files, each of the given
// size, and returns its path.
func makeDir(t *testing.T, files map[string]int) string {
	t.Helper()
	dir := t.TempDir()
	for name, size := range files {
		path := filepath.Join(dir, name)
		if err := os.WriteFile(path, make([]byte, size), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	return dir
}

func TestNewChecksTheDirectory(t *testing.T) {
	dir := makeDir(t, map[string]int{"vol.img": 4096})
	if _, err := New(dir, false); err != nil {
		t.Fatalf("a real directory: %v", err)
	}
	if _, err := New(filepath.Join(dir, "nope"), false); err == nil {
		t.Error("a missing directory should be an error")
	}
	if _, err := New(filepath.Join(dir, "vol.img"), false); err == nil {
		t.Error("a file is not a directory")
	}
}

func TestOpenAndRead(t *testing.T) {
	dir := makeDir(t, map[string]int{"vol.img": 4096})
	if err := os.WriteFile(filepath.Join(dir, "vol.img"), []byte(strings.Repeat("x", 4096)), 0o600); err != nil {
		t.Fatal(err)
	}
	store, err := New(dir, false)
	if err != nil {
		t.Fatal(err)
	}
	vol, err := store.Open("vol")
	if err != nil {
		t.Fatal(err)
	}
	defer func() { _ = vol.Close() }()
	if vol.Size() != 4096 || vol.ReadOnly() {
		t.Errorf("size %d, readOnly %v", vol.Size(), vol.ReadOnly())
	}
	buf := make([]byte, 4)
	if _, err := vol.ReadAt(buf, 0); err != nil || string(buf) != "xxxx" {
		t.Errorf("read: %q, %v", buf, err)
	}
	if _, err := vol.WriteAt([]byte("abcd"), 100); err != nil {
		t.Fatal(err)
	}
	if err := vol.Flush(); err != nil {
		t.Fatal(err)
	}
	onDisk, err := os.ReadFile(filepath.Join(dir, "vol.img"))
	if err != nil {
		t.Fatal(err)
	}
	if string(onDisk[100:104]) != "abcd" {
		t.Error("the write did not reach the file")
	}
}

func TestTheDefaultExport(t *testing.T) {
	dir := makeDir(t, map[string]int{"default.img": 512})
	store, _ := New(dir, false)
	vol, err := store.Open("")
	if err != nil {
		t.Fatalf("the empty name should open %s%s: %v", DefaultName, Suffix, err)
	}
	_ = vol.Close()
}

func TestOpenRefusesWhatIsNotThere(t *testing.T) {
	dir := makeDir(t, map[string]int{"vol.img": 512, "empty.img": 0})
	store, _ := New(dir, false)
	if _, err := store.Open("nope"); !errors.Is(err, nbd.ErrNoExport) {
		t.Errorf("a missing export: %v", err)
	}
	if _, err := store.Open("empty"); !errors.Is(err, nbd.ErrNoExport) {
		t.Errorf("an empty file is not a usable volume: %v", err)
	}
}

func TestList(t *testing.T) {
	dir := makeDir(t, map[string]int{"b.img": 512, "a.img": 512, "notes.txt": 10})
	if err := os.Mkdir(filepath.Join(dir, "sub.img"), 0o700); err != nil {
		t.Fatal(err)
	}
	store, _ := New(dir, false)
	got := store.List()
	if len(got) != 2 || got[0] != "a" || got[1] != "b" {
		t.Errorf("List() = %v, want [a b]", got)
	}
}

func TestReadOnlyStore(t *testing.T) {
	dir := makeDir(t, map[string]int{"vol.img": 512})
	store, _ := New(dir, true)
	vol, err := store.Open("vol")
	if err != nil {
		t.Fatal(err)
	}
	defer func() { _ = vol.Close() }()
	if !vol.ReadOnly() {
		t.Error("the volume should be read-only")
	}
	if _, err := vol.WriteAt([]byte("x"), 0); !errors.Is(err, nbd.ErrReadOnly) {
		t.Errorf("write: %v, want ErrReadOnly", err)
	}
	if err := vol.Flush(); err != nil {
		t.Errorf("flush on a read-only volume: %v", err)
	}
}

// The adapter's own bounds check, independent of the session's.
func TestTheVolumeChecksItsOwnBounds(t *testing.T) {
	dir := makeDir(t, map[string]int{"vol.img": 1024})
	store, _ := New(dir, false)
	vol, _ := store.Open("vol")
	defer func() { _ = vol.Close() }()
	buf := make([]byte, 16)
	cases := []struct {
		name string
		off  int64
		n    int
	}{
		{"past the end", 1024, 1},
		{"overrunning the end", 1020, 16},
		{"a negative offset", -1, 1},
		{"a wildly negative offset", -1 << 40, 16},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			if _, err := vol.ReadAt(buf[:c.n], c.off); !errors.Is(err, nbd.ErrRange) {
				t.Errorf("ReadAt: %v, want ErrRange", err)
			}
			if _, err := vol.WriteAt(buf[:c.n], c.off); !errors.Is(err, nbd.ErrRange) {
				t.Errorf("WriteAt: %v, want ErrRange", err)
			}
		})
	}
	if size := fileSize(t, filepath.Join(dir, "vol.img")); size != 1024 {
		t.Errorf("the file grew to %d bytes", size)
	}
}

func fileSize(t *testing.T, path string) int64 {
	t.Helper()
	info, err := os.Stat(path)
	if err != nil {
		t.Fatal(err)
	}
	return info.Size()
}

// ── the adversarial part (master Rule 25): names are a client's input ───────

// A traversal must be refused, not sanitised: this is the whole reason the
// name never becomes a path anywhere else.
func TestCVEExportNameTraversal(t *testing.T) {
	dir := makeDir(t, map[string]int{"vol.img": 512})
	outside := filepath.Join(filepath.Dir(dir), "outside.img")
	if err := os.WriteFile(outside, make([]byte, 512), 0o600); err != nil {
		t.Fatal(err)
	}
	store, _ := New(dir, false)
	hostile := []string{
		"../outside",
		"../../etc/passwd",
		"/etc/passwd",
		"..",
		".",
		"....//....//etc/passwd",
		"vol/../../outside",
		"sub/vol",
		`..\outside`,
		"vol\x00.img",
		"vol\n../outside",
		"~/outside",
		"$HOME/outside",
		"%2e%2e%2foutside",
		strings.Repeat("a", NameMax+1),
		"",
	}
	for _, name := range hostile {
		t.Run(name, func(t *testing.T) {
			vol, err := store.Open(name)
			if err == nil {
				_ = vol.Close()
				t.Fatalf("%q was accepted", name)
			}
			if !errors.Is(err, nbd.ErrBadName) && !errors.Is(err, nbd.ErrNoExport) {
				t.Errorf("%q: %v", name, err)
			}
		})
	}
}

// safeName is the chokepoint, so it is tested directly as well.
func TestSafeName(t *testing.T) {
	for _, good := range []string{"vol", "vol1", "a-b_c", "v.2", "A", strings.Repeat("a", NameMax)} {
		if _, err := safeName(good); err != nil {
			t.Errorf("safeName(%q) = %v", good, err)
		}
	}
	for _, bad := range []string{"", "/", "a/b", "..", ".hidden", "a b", "a\tb", "ü", "a;b", "a|b", "a*b",
		strings.Repeat("a", NameMax+1)} {
		if _, err := safeName(bad); err == nil {
			t.Errorf("safeName(%q) was accepted", bad)
		}
	}
}

func TestIsNameRune(t *testing.T) {
	for _, r := range "abzABZ019-_." {
		if !isNameRune(r) {
			t.Errorf("%q should be allowed", r)
		}
	}
	for _, r := range "/\\:;*?\"<>| \t\n\x00ü€" {
		if isNameRune(r) {
			t.Errorf("%q should not be allowed", r)
		}
	}
}

// A volume opened read-write must not be reachable as read-only or the
// other way round by asking twice under different names.
func TestTheStoreModeIsNotNegotiable(t *testing.T) {
	dir := makeDir(t, map[string]int{"vol.img": 512})
	ro, _ := New(dir, true)
	vol, err := ro.Open("vol")
	if err != nil {
		t.Fatal(err)
	}
	defer func() { _ = vol.Close() }()
	if !vol.ReadOnly() {
		t.Fatal("a read-only store handed out a writable volume")
	}
}

func BenchmarkSafeName(b *testing.B) {
	for b.Loop() {
		if _, err := safeName("volume-1"); err != nil {
			b.Fatal(err)
		}
	}
}

func BenchmarkIsNameRune(b *testing.B) {
	for b.Loop() {
		_ = isNameRune('a')
	}
}
