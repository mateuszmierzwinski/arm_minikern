// Package imagestore serves boot images from a directory: an adapter over
// the filesystem, equal in status to any other (master Rule 1.3).
//
// A board gets <dir>/<mac>-<chip id>.tar if that exists, else
// <dir>/<mac>.tar, else <dir>/default.tar. Every name is built from hex
// rendered by BoardID.Key, so nothing from the network ever becomes a path
// element: a traversal attempt cannot leave the directory.
package imagestore

import (
	"encoding/hex"
	"fmt"
	"os"
	"path/filepath"
	"sync"
	"time"

	"github.com/mateuszmierzwinski/arm_minikern/server/internal/mknb"
)

// MaxImageBytes bounds what the server will hold for one board.
const MaxImageBytes = 64 << 20

// Store reads images from one directory, caching each until its file
// changes, so a transfer never straddles two versions of an image.
type Store struct {
	dir string

	mu     sync.Mutex // protects cache
	cache  map[string]*entry
	maxLen int64
}

// entry is one cached image and what it was loaded from.
type entry struct {
	image    *mknb.Image
	modTime  time.Time
	size     int64
	fromPath string
}

// New returns a store over dir. It is an mknb.ImageStore.
func New(dir string) mknb.ImageStore {
	return &Store{dir: dir, cache: map[string]*entry{}, maxLen: MaxImageBytes}
}

// Image finds the board's image, loading it if the file changed.
func (s *Store) Image(board mknb.BoardID) (*mknb.Image, error) {
	path, info, err := s.resolve(board)
	if err != nil {
		return nil, err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	key := board.Key()
	if e, ok := s.cache[key]; ok && e.fromPath == path && e.modTime.Equal(info.ModTime()) && e.size == info.Size() {
		return e.image, nil
	}
	img, err := s.load(path, info)
	if err != nil {
		return nil, err
	}
	s.cache[key] = &entry{image: img, modTime: info.ModTime(), size: info.Size(), fromPath: path}
	return img, nil
}

// candidates are the names tried for a board, most specific first.
func (s *Store) candidates(board mknb.BoardID) []string {
	mac := hex.EncodeToString(board.MAC[:])
	return []string{board.Key() + ".tar", mac + ".tar", "default.tar"}
}

// resolve picks the first candidate that exists as a regular file.
func (s *Store) resolve(board mknb.BoardID) (string, os.FileInfo, error) {
	for _, name := range s.candidates(board) {
		path := filepath.Join(s.dir, name)
		info, err := os.Stat(path)
		if err == nil && info.Mode().IsRegular() {
			return path, info, nil
		}
	}
	return "", nil, fmt.Errorf("%w: tried %v in %s", mknb.ErrNoImage, s.candidates(board), s.dir)
}

// load reads the file and computes its digest.
func (s *Store) load(path string, info os.FileInfo) (*mknb.Image, error) {
	if info.Size() > s.maxLen {
		return nil, fmt.Errorf("%s is %d bytes, over the %d-byte limit", path, info.Size(), s.maxLen)
	}
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("reading %s: %w", path, err)
	}
	if len(data) == 0 {
		return nil, fmt.Errorf("%w: %s is empty", mknb.ErrNoImage, path)
	}
	return mknb.NewImage(filepath.Base(path), data), nil
}
