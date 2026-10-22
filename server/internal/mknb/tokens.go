package mknb

import (
	"crypto/rand"
	"encoding/binary"
	"fmt"
)

// RandomTokens issues session tokens from the system's cryptographic
// randomness. It lives here, beside the code whose safety depends on it,
// because an off-path attacker who can predict a token can aim the
// server's block window at a third party (NETBOOT.md §3): the
// unpredictability is a protocol requirement, not a deployment choice.
type RandomTokens struct{}

// Token returns 32 unpredictable bits.
func (RandomTokens) Token() uint32 {
	var b [4]byte
	if _, err := rand.Read(b[:]); err != nil {
		// crypto/rand does not fail on any platform we run on; if it ever
		// does, serving with a guessable token is worse than stopping.
		panic(fmt.Sprintf("mknb: no randomness for session tokens: %v", err))
	}
	return binary.BigEndian.Uint32(b[:])
}
