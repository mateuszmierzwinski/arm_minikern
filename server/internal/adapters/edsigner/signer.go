// Package edsigner signs netboot offers with an Ed25519 private key.
//
// The private half lives here and nowhere else: the board carries only the
// public key, so a kernel image that is handed around gives nothing away.
// That is the whole reason this is a signature rather than a shared secret.
package edsigner

import (
	"crypto/ed25519"
	"encoding/hex"
	"fmt"
	"os"
	"strings"
)

// Signer signs with one key.
type Signer struct {
	key ed25519.PrivateKey
}

// FromSeedFile reads a 32-byte seed, given as 64 hex characters in a file,
// and returns a signer for the key it generates — the same key every time,
// so a board's configured public key keeps working across restarts.
//
// A seed in a file rather than on the command line, because a command line
// is visible to every process on the machine.
func FromSeedFile(path string) (*Signer, error) {
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	seed, err := hex.DecodeString(strings.TrimSpace(string(raw)))
	if err != nil {
		return nil, fmt.Errorf("%s: not hex: %w", path, err)
	}
	if len(seed) != ed25519.SeedSize {
		return nil, fmt.Errorf("%s: a seed is %d hex characters, not %d", path, ed25519.SeedSize*2,
			len(seed)*2)
	}
	return &Signer{key: ed25519.NewKeyFromSeed(seed)}, nil
}

// Sign returns the signature of message.
func (s *Signer) Sign(message []byte) []byte {
	return ed25519.Sign(s.key, message)
}

// PublicKeyHex is what a board is configured with
// (`minikern.netboot.key=`), which is why the server prints it.
func (s *Signer) PublicKeyHex() string {
	return hex.EncodeToString(s.key.Public().(ed25519.PublicKey))
}
