package volumefile

import (
	"fmt"
	"strings"

	"github.com/mateuszmierzwinski/arm_minikern/server/internal/nbd"
)

// NameMax bounds an export name. The protocol allows far more; a file name
// does not need it, and an unbounded name is an unbounded allocation.
const NameMax = 64

// safeName turns an export name a client chose into a single path
// component, or refuses it.
//
// Everything here is a refusal rather than a repair: sanitising by stripping
// characters is how "..%2f.." becomes a traversal. A name must be a plain
// identifier to begin with, so the result can only ever be a file directly
// inside the volume directory.
func safeName(name string) (string, error) {
	if name == "" || len(name) > NameMax {
		return "", fmt.Errorf("%w: %d characters", nbd.ErrBadName, len(name))
	}
	for _, r := range name {
		if !isNameRune(r) {
			return "", fmt.Errorf("%w: %q", nbd.ErrBadName, name)
		}
	}
	if strings.HasPrefix(name, ".") {
		return "", fmt.Errorf("%w: %q starts with a dot", nbd.ErrBadName, name)
	}
	return name, nil
}

// isNameRune allows letters, digits and a few separators — and nothing
// else, so a path separator, a NUL, a newline or anything non-ASCII cannot
// appear in a name at all.
func isNameRune(r rune) bool {
	switch {
	case r >= 'a' && r <= 'z', r >= 'A' && r <= 'Z', r >= '0' && r <= '9':
		return true
	case r == '-', r == '_', r == '.':
		return true
	}
	return false
}
