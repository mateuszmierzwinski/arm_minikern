// Package volumefile serves NBD exports out of a directory of image files.
//
// It is the one place an export name becomes a path, so it is also the only
// place a path traversal could be introduced — which is why the name is
// validated here and the result is never allowed to leave the directory.
package volumefile

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"

	"github.com/mateuszmierzwinski/arm_minikern/server/internal/nbd"
)

// Suffix is the extension a volume file carries, so the directory can hold
// other things (a README, a checksum) without them becoming exports.
const Suffix = ".img"

// DefaultName is the export served when a client asks for "", which is what
// a client that does not care gets.
const DefaultName = "default"

// store is a directory of volume files.
type store struct {
	dir      string
	readOnly bool
}

// New returns a volume store over dir. The directory must exist: creating
// it here would turn a typo in a flag into an empty server that looks fine.
func New(dir string, readOnly bool) (nbd.VolumeStore, error) {
	info, err := os.Stat(dir)
	if err != nil {
		return nil, fmt.Errorf("volume directory %q: %w", dir, err)
	}
	if !info.IsDir() {
		return nil, fmt.Errorf("volume directory %q: not a directory", dir)
	}
	return &store{dir: dir, readOnly: readOnly}, nil
}

// Open resolves a name to a file in the directory and opens it.
func (s *store) Open(name string) (nbd.Volume, error) {
	if name == "" {
		name = DefaultName
	}
	safe, err := safeName(name)
	if err != nil {
		return nil, err
	}
	return openVolume(filepath.Join(s.dir, safe+Suffix), s.readOnly)
}

// List names the exports, without their suffix, in a stable order.
func (s *store) List() []string {
	entries, err := os.ReadDir(s.dir)
	if err != nil {
		return nil
	}
	var names []string
	for _, e := range entries {
		if e.IsDir() || !strings.HasSuffix(e.Name(), Suffix) {
			continue
		}
		names = append(names, strings.TrimSuffix(e.Name(), Suffix))
	}
	sort.Strings(names)
	return names
}

// volume is one open image file.
type volume struct {
	mu       sync.Mutex // protects f against a close racing an in-flight request
	f        *os.File
	size     int64
	readOnly bool
}

// openVolume opens one image file and measures it. A volume of zero bytes
// is refused: a client cannot do anything with it, and it is almost always
// a half-created file.
func openVolume(path string, readOnly bool) (nbd.Volume, error) {
	flag := os.O_RDWR
	if readOnly {
		flag = os.O_RDONLY
	}
	f, err := os.OpenFile(path, flag, 0)
	if errors.Is(err, os.ErrNotExist) {
		return nil, fmt.Errorf("%w: %s", nbd.ErrNoExport, filepath.Base(path))
	}
	if err != nil {
		return nil, err
	}
	info, err := f.Stat()
	if err != nil || info.Size() == 0 {
		_ = f.Close()
		return nil, fmt.Errorf("%w: %s is empty or unreadable", nbd.ErrNoExport, filepath.Base(path))
	}
	return &volume{f: f, size: info.Size(), readOnly: readOnly}, nil
}

func (v *volume) Size() int64 { return v.size }

func (v *volume) ReadOnly() bool { return v.readOnly }

// ReadAt refuses anything outside the size it measured at open, so the
// client's view of the geometry and the file's contents cannot diverge even
// if something else truncates the file underneath.
func (v *volume) ReadAt(p []byte, off int64) (int, error) {
	if err := v.inRange(p, off); err != nil {
		return 0, err
	}
	return v.f.ReadAt(p, off)
}

func (v *volume) WriteAt(p []byte, off int64) (int, error) {
	if v.readOnly {
		return 0, nbd.ErrReadOnly
	}
	if err := v.inRange(p, off); err != nil {
		return 0, err
	}
	return v.f.WriteAt(p, off)
}

// inRange is the adapter's own bounds check. The service checks too; this
// is the belt to that pair of braces, because a bug there must not be able
// to grow the file.
func (v *volume) inRange(p []byte, off int64) error {
	if off < 0 || off > v.size || int64(len(p)) > v.size-off {
		return fmt.Errorf("%w: %d bytes at %d of %d", nbd.ErrRange, len(p), off, v.size)
	}
	return nil
}

func (v *volume) Flush() error {
	if v.readOnly {
		return nil
	}
	return v.f.Sync()
}

func (v *volume) Close() error {
	v.mu.Lock()
	defer v.mu.Unlock()
	return v.f.Close()
}
