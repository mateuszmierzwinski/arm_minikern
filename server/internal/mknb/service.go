package mknb

import (
	"errors"
	"net/netip"
	"sync"
	"time"
)

// Defaults for a Service. They bound what one client, and all clients
// together, can make the server hold or send.
const (
	DefaultMaxSessions       = 256
	DefaultSessionsPerClient = 4
	DefaultSessionTTL        = 60 * time.Second
)

// Config tunes a Service; the zero value means the defaults above.
type Config struct {
	MaxSessions       int
	SessionsPerClient int
	SessionTTL        time.Duration
}

// Service is the server's logic: it answers probes with offers and serves
// the blocks of a session that has proved it can receive what we send.
//
// Safe for concurrent use: mu protects sessions and stats, so the
// transport may call Handle from more than one goroutine.
type Service struct {
	mu       sync.Mutex // protects sessions and stats
	sessions map[sessionKey]*session
	stats    Stats

	store  ImageStore
	out    Transport
	clock  Clock
	tokens TokenSource
	log    Logger
	cfg    Config
}

// NewService wires the hexagon. Every dependency is an interface, so the
// tests drive it with fakes and no socket exists.
func NewService(store ImageStore, out Transport, clock Clock, tokens TokenSource, log Logger, cfg Config) *Service {
	if cfg.MaxSessions <= 0 {
		cfg.MaxSessions = DefaultMaxSessions
	}
	if cfg.SessionsPerClient <= 0 {
		cfg.SessionsPerClient = DefaultSessionsPerClient
	}
	if cfg.SessionTTL <= 0 {
		cfg.SessionTTL = DefaultSessionTTL
	}
	return &Service{sessions: map[sessionKey]*session{}, store: store, out: out, clock: clock,
		tokens: tokens, log: log, cfg: cfg}
}

// Handle processes one datagram from one address. It never blocks and
// never panics on malformed input: everything it cannot read is counted
// and dropped.
func (s *Service) Handle(from netip.AddrPort, payload []byte) {
	h, err := ParseHeader(payload)
	if err != nil {
		s.count(func(st *Stats) { st.Malformed++ })
		return
	}
	switch h.Type {
	case TypeDiscover:
		s.handleDiscover(from, payload, h)
	case TypeRead:
		s.handleRead(from, payload, h)
	case TypeDone:
		s.handleDone(from, h)
	default:
		s.count(func(st *Stats) { st.Unknown++ })
	}
}

// Stats is a snapshot of the counters.
func (s *Service) Stats() Stats {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := s.stats
	out.SessionsNow = len(s.sessions)
	return out
}

// count applies f to the counters under the lock.
func (s *Service) count(f func(*Stats)) {
	s.mu.Lock()
	defer s.mu.Unlock()
	f(&s.stats)
}

// handleDiscover answers a probe, after finding the board an image and
// taking a session slot for it.
func (s *Service) handleDiscover(from netip.AddrPort, payload []byte, h Header) {
	d, err := ParseDiscover(payload, h)
	if err != nil {
		s.count(func(st *Stats) { st.Malformed++ })
		return
	}
	s.count(func(st *Stats) { st.Discovers++ })
	img, err := s.store.Image(d.Board)
	if err != nil {
		s.refuseImage(from, h, d, err)
		return
	}
	s.offer(from, d, img)
}

// refuseImage logs why a board got nothing. A board we have no image for
// is told so, because that reply is no larger than the probe; anything
// else is only logged.
func (s *Service) refuseImage(from netip.AddrPort, h Header, d Discover, err error) {
	s.count(func(st *Stats) { st.NoImage++ })
	s.log.Printf("no image for %s (%s): %v", d.Board.MACString(), from, err)
	if errors.Is(err, ErrNoImage) {
		_ = s.out.Send(from, EncodeError(h.XID, ReasonNoImage, "no image for this board"))
	}
}

// offer records a session and sends the OFFER that opens the transfer.
func (s *Service) offer(from netip.AddrPort, d Discover, img *Image) {
	token, ok := s.openSession(from, d, img)
	if !ok {
		s.count(func(st *Stats) { st.Busy++ })
		return
	}
	payload, err := EncodeOffer(d.XID, img, token)
	if err != nil {
		s.log.Printf("cannot offer %s to %s: %v", img.Name, from, err)
		return
	}
	s.count(func(st *Stats) { st.Offers++ })
	s.log.Printf("offer %s (%d bytes, %d blocks, sha256 %s) to %s for %s", img.Name, len(img.Data),
		img.Blocks(), img.DigestString()[:16], from, d.Board.MACString())
	_ = s.out.Send(from, payload)
}

// openSession takes a slot for (from, xid), replacing any session that key
// already had. It refuses when the table or the client's share is full,
// which is what keeps a flood of probes from costing us memory.
func (s *Service) openSession(from netip.AddrPort, d Discover, img *Image) (uint32, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	key := sessionKey{addr: from, xid: d.XID}
	s.expireLocked()
	if _, exists := s.sessions[key]; !exists {
		if len(s.sessions) >= s.cfg.MaxSessions || s.perClientLocked(from) >= s.cfg.SessionsPerClient {
			return 0, false
		}
	}
	token := s.tokens.Token()
	s.sessions[key] = &session{board: d.Board, image: img, token: token, started: s.clock.Now().UnixNano()}
	return token, true
}

// perClientLocked counts one address's live sessions.
func (s *Service) perClientLocked(addr netip.AddrPort) int {
	n := 0
	for k := range s.sessions {
		if k.addr == addr {
			n++
		}
	}
	return n
}

// expireLocked drops sessions older than the TTL.
func (s *Service) expireLocked() {
	cutoff := s.clock.Now().Add(-s.cfg.SessionTTL).UnixNano()
	for k, sess := range s.sessions {
		if sess.started < cutoff {
			delete(s.sessions, k)
		}
	}
}

// lookup finds the session a READ or DONE belongs to and checks its token.
// This is the amplification defence: without the token the server sends
// nothing, so it cannot be aimed at a third party (NETBOOT.md §3). Stale
// sessions are dropped first, so the TTL bounds the serving path too and
// not just the table's size.
func (s *Service) lookup(from netip.AddrPort, xid, token uint32, checkToken bool) (*session, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.expireLocked()
	sess, ok := s.sessions[sessionKey{addr: from, xid: xid}]
	if !ok {
		return nil, ErrNoSession
	}
	if checkToken && token != sess.token {
		return nil, ErrToken
	}
	return sess, nil
}

// handleRead serves the window a session asked for.
func (s *Service) handleRead(from netip.AddrPort, payload []byte, h Header) {
	r, err := ParseRead(payload, h)
	if err != nil {
		s.count(func(st *Stats) { st.Malformed++ })
		return
	}
	sess, err := s.lookup(from, h.XID, r.Token, true)
	if err != nil {
		s.count(func(st *Stats) { st.BadToken++ })
		s.log.Printf("refused read from %s: %v", from, err)
		return
	}
	s.count(func(st *Stats) { st.Reads++ })
	s.sendWindow(from, sess, r)
}

// sendWindow sends each block of the requested range.
func (s *Service) sendWindow(from netip.AddrPort, sess *session, r Read) {
	first, end := blockRange(sess.image, r.Arg, r.Count)
	sent := uint64(0)
	for index := first; index < end; index++ {
		block, ok := sess.image.Block(index)
		if !ok {
			break
		}
		if err := s.out.Send(from, EncodeData(r.XID, index, block)); err != nil {
			s.log.Printf("send to %s failed at block %d: %v", from, index, err)
			break
		}
		sent++
	}
	s.mu.Lock()
	sess.blocks += sent
	s.stats.Blocks += sent
	s.mu.Unlock()
}

// handleDone closes a session the client says it finished with.
func (s *Service) handleDone(from netip.AddrPort, h Header) {
	sess, err := s.lookup(from, h.XID, 0, false)
	if err != nil {
		return
	}
	s.mu.Lock()
	delete(s.sessions, sessionKey{addr: from, xid: h.XID})
	s.stats.Done++
	s.mu.Unlock()
	s.log.Printf("done: %s took %s (%d blocks sent)", sess.board.MACString(), sess.image.Name, sess.blocks)
}
