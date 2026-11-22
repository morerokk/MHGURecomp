// The lobby / relay server for MHGURecomp's Local Play over the internet (runtime/net/ldn.c "internet hubs").
//
// A hub's host keeps one connection here (HOST): its hub is listed for players with the same matchmaking key and
// port version (LIST). A player who picks it (JOIN) gets a ticket sent to the host, the host opens a second
// connection with that ticket (ATTACH), and from then on the two connections are spliced: the bytes are the same
// link the games speak over a direct connection (ldn.c link frames), which this server neither reads nor changes.
// Game packets go by UDP when that gets through: datagrams to this server's UDP port, prefixed with the pair's id
// and the sender's secret, are passed to the other end of the pair (without the prefix). Nobody needs a forwarded
// port, and players never see each other's IP addresses - unless both ends ask for direct UDP (hole punching, see
// peerQuery): then each is told the other's public UDP address, the games send their packets straight to each other
// when their routers let them, and the relay stays as the fallback.
//
// Control frames: u32 size (of what follows), u8 type, payload; little endian. The first frame of a connection is a
// request, whose payload starts with a header: "MHGS", u16 server protocol, u16 link protocol (the port's
// PROTO_VERSION: only hubs of the same one, or of one it plays with - see Settings.group - are listed or joined), room[16]
// (the first 16 bytes of SHA-256 of the matchmaking key, all zero without a key).
//
// Link protocol 4 (MHGURecomp 0.19) plays with 3: its hosts take a protocol 3 player's hello, and its players say 3
// to a protocol 3 host, the link frames being the same. So 3 and 4 share one pool of hubs, and a game of either sees
// and joins the others' hubs. A game asking under 4 gets more:
//   - LIST answers HUBS4, which tells each hub's link protocol and session code.
//   - HOST may carry the hub's session code after the NetworkInfo: 8 characters of codeAlphabet that the host picks
//     for its network and keeps across re-listings, so a code a player shared goes on working. A code another hub
//     holds is refused (errCodeTaken: the host picks another); the same hub (session id and BSSID) listing again
//     takes its code back from the listing it left.
//   - FIND + code: that hub, whatever its room (a code is an invitation), as a HUBS4 of 0 or 1 hubs.
//   - JOIN may carry the hub's code after its id: with the right code the room doesn't have to match.
//   - PRESENCE: friends (see presence). An install's identity is a random 32-byte secret it keeps; its friend code
//     is the first 10 bytes of SHA-256("MHGURecomp friend:" + secret). The server stores neither for long: only who
//     was online in the last presenceTTL, with their friend codes and what they were doing. A player's state is told
//     only to the players it lists that list it back.
//
// Requests may carry more than this server reads: bytes after the fields it knows are ignored, so a newer game can
// add some without a new server (and loses nothing with this one but what it added).
//
// Everything a client sends is untrusted: sizes are capped, NetworkInfo is checked before it is listed, requests are
// rate limited per IP address, connections and hubs per IP address are limited, and the relay only ever forwards
// between the two ends of a pair it made itself (never to an address a client names; a relayed datagram is smaller
// than the one that caused it).
package main

import (
	"bytes"
	"crypto/rand"
	"crypto/sha256"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"log"
	mrand "math/rand/v2"
	"net"
	"sort"
	"strings"
	"sync"
	"sync/atomic"
	"time"
	"unicode/utf8"
)

const (
	serverProto = 1
	headerSize  = 24
	netInfoSize = 0x480
	maxFrame    = 4096

	// requests (the first frame)
	tList   = 1 // header -> HUBS
	tHost   = 2 // header, NetworkInfo -> HOSTED; then UPDATE / PING from the host, INCOMING / PING to it
	tAttach = 3 // header, u64 ticket -> SPLICED (the host's side of a join)
	tJoin   = 4 // header, u64 hub id [, code[8] (protocol 4)] -> SPLICED (the player's side) or ERROR
	// on a hub's connection
	tUpdate = 5 // NetworkInfo: the hub changed (players, advertise data)
	tPing   = 6 // keeps the connection alive (both ways)
	// protocol 4 requests
	tFind     = 7 // header, code[8] -> HUBS4 (0 or 1 hub)
	tPresence = 8 // header, secret[32], u8 state, code[8], u8 n, n x friend[10] [, u8 len, name] -> FRIENDS
	// answers
	tHubs     = 16 // u32 count, count x (u64 hub id, NetworkInfo)
	tHosted   = 17 // u64 hub id
	tIncoming = 18 // u64 ticket: a player joins, ATTACH with it
	tSpliced  = 19 // u32 pair id, u64 secret (for UDP); then the connection is the link
	tError    = 20 // u32 code
	tHubs4    = 21 // u32 count, count x (u64 hub id, u16 link protocol, code[8] (zeros: none), NetworkInfo)
	tFriends  = 22 // u8 n, n x (friend[10], u8 state, code[8]): the listed friends online that list the asker back;
	// then u8 r, r x (friend[10], u8 len, name): friend requests, the players online that list the asker unlisted

	errNotFound  = 1 // no such hub (or not for this key / version)
	errNoAnswer  = 2 // the host did not take the player
	errBusy      = 3 // a limit was reached
	errVersion   = 4 // server protocol mismatch, or a link protocol older than Config.MinProto or newer than Config.MaxProto
	errBadReq    = 5
	errCodeTaken = 6 // another hub holds that session code: pick another

	// protocol 4
	codeLen      = 8
	codeAlphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ" // (Crockford's base 32: no I, L, O, U)
	secretLen    = 32
	friendLen    = 10
	maxFriends   = 64
	presenceTTL  = 90 * time.Second // the game asks every 30 s
	stateOnline  = 0                // in the game
	stateHosting = 1                // hosting a hub (code: its session code, or zeros)
	stateInHub   = 2                // in someone's hub (code: that hub's, when known); a state the server doesn't
	// know is passed on as it came (a newer game's: older games show it as online)
	nameMax       = 32 // bytes of a name in PRESENCE (UTF-8: the hunter name the player goes by)
	maxRequests   = 8  // friend requests in one answer (the newest)
	friendSalt    = "MHGURecomp friend:"
	protoSessions = 4 // the first link protocol with session codes, FIND, PRESENCE and HUBS4

	udpMagic  = "MHGR"
	udpHeader = 16 // magic, u32 pair, u64 secret
	udpMax    = udpHeader + 12 + 16 + 8192
	// Direct UDP between the two ends (hole punching): a datagram of exactly udpQuerySize bytes whose payload is
	// "MHGQ" + 12 zero bytes asks for the other end's address. It is not passed on; once both ends have asked (each
	// end's consent to show its address to the other; a game too old to ask stays on the relay), the answer goes
	// back to the asker, as big as the query: "MHGP", u32 pair, u64 the asker's secret, IPv4 address[4], u16 port
	// (big endian), 2 zero bytes, u64 the pair's direct key. The games prefix their direct datagrams with "MHGR",
	// the pair and that key, so each end can tell the other's datagrams from anything else reaching its port.
	peerQuery    = "MHGQ"
	peerAnswer   = "MHGP"
	udpQuerySize = udpHeader + 16

	// NetworkInfo offsets (runtime/net/ldn.c)
	niSessionID = 0x10 // 16 random bytes, new for each hub
	niBSSID     = 0x20 // the host's MAC address
	niSSID      = 0x26
	niNetType   = 0x4B
	niNodeMax   = 0x66
	niNodeCount = 0x67
	niAdvSize   = 0x26A
	niAdv       = 0x26C
	advMax      = 0x180

	// MHGU's advertise data: u16 passcode at 0xC0, 0xFFFF = none, else the 4-digit code in bits 0-14 and bit 15 set
	// when it applies to everyone (clear: everyone but friends)
	advPasscode = 0xC0
	// ... and 0xFF at 0x1C in a Hunter Search network (hubs have 1 there, after the host's account id at 0x14): see
	// huntSearch
	advHuntSearch = 0x1C
	maxNodes      = 8
)

type Config struct {
	Addr          string
	MaxConns      int     // all connections
	MaxConnsPerIP int     // connections from one IP address
	MaxHubs       int     // hubs listed at once
	MaxHubsPerIP  int     // hubs one IP address hosts
	ListMax       int     // hubs in one LIST answer (the game's Join Hub list shows 18: 3 pages of 6)
	ListSearchMax int     // Hunter Search networks in one LIST answer, after the hubs (its list shows 6; the game scans for 24)
	ReqRate       float64 // LIST / JOIN requests a second per IP address (burst: ReqBurst)
	ReqBurst      float64
	Bandwidth     float64       // bytes a second per pair and direction (TCP), and per pair (UDP)
	MaxPresence   int           // players online at once that the friends list keeps (PRESENCE)
	HandshakeTime time.Duration // for the first frame, and a joining player's wait for the host
	PingEvery     time.Duration
	// The settings that can change while the server runs (Server.Apply: these, LinkPool* and SeparatePhones): their
	// values at the start.
	MinProto uint16 // the oldest link protocol (PROTO_VERSION) served; older games get errVersion (0: any)
	// the newest link protocol served; newer ones get errVersion (0: any). Other ports that borrow this server under
	// a link protocol of their own (MH Rise's sends 0x5201) are above it, as long as MHGURecomp's stays below it.
	MaxProto uint16
	// The link protocols from LinkPoolLow to LinkPoolHigh play together: their hubs are listed and joined across them
	// (the game tells HUBS4 askers each hub's protocol, and speaks the lower one). Others play with their own only.
	// LinkPoolLow 0: no pool. It can change while the server runs too.
	LinkPoolLow, LinkPoolHigh uint16
	// Android's public hubs (the game sends the key "public-phone" when it has none) in a room of their own, as the
	// game asks; false: that room is the public room, so phones and PCs without a key see each other's hubs
	SeparatePhones bool
	Verbose        bool
}

func DefaultConfig() Config {
	return Config{
		Addr: ":39510", MaxConns: 4000, MaxConnsPerIP: 24, MaxHubs: 2000, MaxHubsPerIP: 4, ListMax: 18, ListSearchMax: 6,
		ReqRate: 2, ReqBurst: 15, Bandwidth: 1 << 20, MaxPresence: 20000, HandshakeTime: 10 * time.Second,
		PingEvery: 15 * time.Second, MaxProto: 100, LinkPoolLow: 3, LinkPoolHigh: 4, SeparatePhones: true,
	}
}

type hub struct {
	id      uint64
	room    [16]byte // as the host sent it (Android's public room too: whether it is the public room is decided per request)
	proto   uint16
	code    [codeLen]byte     // its session code (protocol 4; zeros: none)
	conn    *net.TCPConn      // the host's connection (Apply closes it when the hub's protocol is no longer served)
	info    [netInfoSize]byte // guarded by Server.mu
	created time.Time
	ip      string
	out     chan []byte // frames to the host
	done    chan struct{}
	joins   int // pending and spliced players (guarded by Server.mu)
}

type pending struct {
	hub      *hub
	host     chan *net.TCPConn // the host's ATTACH connection (buffered: never blocks)
	finished chan struct{}     // the splice ended (the ATTACH handler waits for it)
}

type pair struct {
	id     uint32
	secret [2]uint64 // 0: host, 1: player
	mu     sync.Mutex
	addr   [2]*net.UDPAddr
	direct [2]bool // the end asked for the other's address (it may see its own then)
	dkey   uint64  // the pair's direct key (both ends get it with the address)
	lim    *bucket
}

type ipState struct {
	conns, hubs int
	req         *bucket
	seen        time.Time
}

// A player online, for its friends (PRESENCE): kept presenceTTL after its last request.
type presence struct {
	friends map[[friendLen]byte]bool // the friend codes it lists
	state   uint8
	code    [codeLen]byte
	name    string // the name it goes by (for its friend requests)
	seen    time.Time
}

type Server struct {
	cfg     Config
	mu      sync.Mutex
	hubs    map[uint64]*hub
	codes   map[[codeLen]byte]*hub // hubs by session code
	pending map[uint64]*pending
	pairs   map[uint32]*pair
	ips     map[string]*ipState
	online  map[[friendLen]byte]*presence
	// who lists whom, the other way around: listedBy[b][a] = since when a (online) lists b. A listing not returned
	// is a friend request to b.
	listedBy map[[friendLen]byte]map[[friendLen]byte]time.Time
	conns    int
	ln       *net.TCPListener
	udp      *net.UDPConn
	// the live settings (Apply): cfg.MinProto, cfg.MaxProto, cfg.LinkPool* and cfg.SeparatePhones are only their
	// first values
	minProto atomic.Uint32
	maxProto atomic.Uint32
	pool     atomic.Uint32 // LinkPoolLow | LinkPoolHigh<<16
	separate atomic.Bool
}

func NewServer(cfg Config) *Server {
	s := &Server{cfg: cfg, hubs: map[uint64]*hub{}, codes: map[[codeLen]byte]*hub{}, pending: map[uint64]*pending{},
		pairs: map[uint32]*pair{}, ips: map[string]*ipState{}, online: map[[friendLen]byte]*presence{},
		listedBy: map[[friendLen]byte]map[[friendLen]byte]time.Time{}}
	s.minProto.Store(uint32(cfg.MinProto))
	s.maxProto.Store(uint32(cfg.MaxProto))
	s.pool.Store(uint32(cfg.LinkPoolLow) | uint32(cfg.LinkPoolHigh)<<16)
	s.separate.Store(cfg.SeparatePhones)
	return s
}

// Settings: what can change while the server runs.
type Settings struct {
	MinProto       uint16
	MaxProto       uint16 // 0: no newest
	LinkPoolLow    uint16 // the link protocols that play together (0: none)
	LinkPoolHigh   uint16
	SeparatePhones bool
}

// serves: whether games of this link protocol are served (MinProto up to MaxProto).
func (st Settings) serves(proto uint16) bool {
	return proto >= st.MinProto && (st.MaxProto == 0 || proto <= st.MaxProto)
}

func (s *Server) Settings() Settings {
	pool := s.pool.Load()
	return Settings{MinProto: uint16(s.minProto.Load()), MaxProto: uint16(s.maxProto.Load()), LinkPoolLow: uint16(pool),
		LinkPoolHigh: uint16(pool >> 16), SeparatePhones: s.separate.Load()}
}

// Apply changes the settings without a restart; open connections stay. A higher MinProto or a lower MaxProto refuses
// the games left out from the next request on and takes their hubs off the list: their hosts' connections here are
// closed (the game tries again and is told the server runs another version), while players already in such a hub
// play on through the relay until they leave. The link pool and SeparatePhones apply to every search and join from now
// on, to the hubs already listed too (no host has to reconnect). Returns how many hubs were taken off.
func (s *Server) Apply(n Settings) int {
	old := s.Settings()
	s.minProto.Store(uint32(n.MinProto))
	s.maxProto.Store(uint32(n.MaxProto))
	s.pool.Store(uint32(n.LinkPoolLow) | uint32(n.LinkPoolHigh)<<16)
	s.separate.Store(n.SeparatePhones)
	var gone []*net.TCPConn
	s.mu.Lock()
	for _, h := range s.hubs {
		if !n.serves(h.proto) {
			gone = append(gone, h.conn)
		}
	}
	s.mu.Unlock()
	for _, c := range gone {
		c.Close()
	}
	if old != n {
		log.Printf("settings: min-proto %d -> %d, max-proto %d -> %d, link-pool %s -> %s, separate-phones %v -> %v (%d "+
			"hub(s) of games no longer served taken off)", old.MinProto, n.MinProto, old.MaxProto, n.MaxProto, old.poolText(),
			n.poolText(), old.SeparatePhones, n.SeparatePhones, len(gone))
	}
	return len(gone)
}

// group: the link protocols that play together share one, the pool's (LinkPoolLow to LinkPoolHigh: 3 to 4 by default,
// as 4's hosts take a protocol 3 hello and its players speak 3 to a protocol 3 host), so their hubs are listed and
// joined across them. A protocol outside the pool plays with its own only.
func (st Settings) group(proto uint16) uint16 {
	if st.LinkPoolLow != 0 && proto >= st.LinkPoolLow && proto <= st.LinkPoolHigh {
		return st.LinkPoolLow
	}
	return proto
}

// poolText: the pool as the settings file writes it ("3-4", or "off").
func (st Settings) poolText() string {
	if st.LinkPoolLow == 0 {
		return "off"
	}
	return fmt.Sprintf("%d-%d", st.LinkPoolLow, st.LinkPoolHigh)
}

// zeros: all zero bytes ("none" for a code).
func zeros(b []byte) bool { return len(bytes.Trim(b, "\x00")) == 0 }

// codeOK: a session code as the game makes them (codeAlphabet only; zeros are "none" and not a code).
func codeOK(c []byte) bool {
	if len(c) != codeLen {
		return false
	}
	for _, b := range c {
		if !bytes.ContainsRune([]byte(codeAlphabet), rune(b)) {
			return false
		}
	}
	return true
}

// The room a hub or request is in: Android's public room is the public room unless phones are kept apart.
func meet(r [16]byte, separate bool) [16]byte {
	if r == phoneRoom && !separate {
		return [16]byte{}
	}
	return r
}

func (s *Server) vlog(format string, args ...any) {
	if s.cfg.Verbose {
		log.Printf(format, args...)
	}
}

// The room of Android's public hubs: the key "public-phone" (runtime/net/ldn.c lobby_header).
var phoneRoom = func() (r [16]byte) {
	h := sha256.Sum256([]byte("MHGURecomp matchmaking key:public-phone"))
	copy(r[:], h[:])
	return
}()

// A room for the log: "public", "phone" (Android's public room) or "key" and the room: the key's hash as the game
// sends it (the key itself never reaches the server). A guessed key's room, to compare: the first 16 bytes of
// SHA-256("MHGURecomp matchmaking key:" + key), README.md.
func roomName(r [16]byte) string {
	switch r {
	case [16]byte{}:
		return "public"
	case phoneRoom:
		return "phone"
	}
	return fmt.Sprintf("key %x", r)
}

// Listen opens the TCP and UDP ports (the same number); Serve then runs until Close.
func (s *Server) Listen() error {
	ta, err := net.ResolveTCPAddr("tcp", s.cfg.Addr)
	if err != nil {
		return err
	}
	if s.ln, err = net.ListenTCP("tcp", ta); err != nil {
		return err
	}
	ua := &net.UDPAddr{IP: ta.IP, Port: s.ln.Addr().(*net.TCPAddr).Port}
	if s.udp, err = net.ListenUDP("udp", ua); err != nil {
		s.ln.Close()
		return err
	}
	return nil
}

func (s *Server) Addr() *net.TCPAddr { return s.ln.Addr().(*net.TCPAddr) }

func (s *Server) Close() {
	s.ln.Close()
	s.udp.Close()
}

func (s *Server) Serve() error {
	go s.serveUDP()
	go s.janitor()
	for {
		c, err := s.ln.AcceptTCP()
		if err != nil {
			if errors.Is(err, net.ErrClosed) {
				return nil
			}
			time.Sleep(50 * time.Millisecond)
			continue
		}
		go s.handle(c)
	}
}

// Forgets the request budgets of addresses not seen for a while, and the players gone from the friends list.
func (s *Server) janitor() {
	for range time.Tick(time.Minute) {
		s.mu.Lock()
		for ip, st := range s.ips {
			if st.conns == 0 && st.hubs == 0 && time.Since(st.seen) > 5*time.Minute {
				delete(s.ips, ip)
			}
		}
		s.prunePresenceLocked()
		s.mu.Unlock()
	}
}

func (s *Server) prunePresenceLocked() {
	for id, p := range s.online {
		if time.Since(p.seen) > presenceTTL {
			s.unlistLocked(id, p.friends, nil)
			delete(s.online, id)
		}
	}
}

// unlistLocked: a's listings of the friends in old that keep (nil: none) doesn't have, out of listedBy.
func (s *Server) unlistLocked(a [friendLen]byte, old, keep map[[friendLen]byte]bool) {
	for f := range old {
		if keep[f] {
			continue
		}
		if m := s.listedBy[f]; m != nil {
			delete(m, a)
			if len(m) == 0 {
				delete(s.listedBy, f)
			}
		}
	}
}

func rand64() uint64 {
	var b [8]byte
	if _, err := rand.Read(b[:]); err != nil {
		panic(err)
	}
	return binary.LittleEndian.Uint64(b[:])
}

// ---- frames
func readFrame(r io.Reader, max int) (byte, []byte, error) {
	var h [5]byte
	if _, err := io.ReadFull(r, h[:4]); err != nil {
		return 0, nil, err
	}
	size := binary.LittleEndian.Uint32(h[:4])
	if size < 1 || size > uint32(max)+1 {
		return 0, nil, errors.New("bad frame size")
	}
	p := make([]byte, size)
	if _, err := io.ReadFull(r, p); err != nil {
		return 0, nil, err
	}
	return p[0], p[1:], nil
}

func frame(t byte, payload ...[]byte) []byte {
	n := 1
	for _, p := range payload {
		n += len(p)
	}
	b := make([]byte, 5, 4+n)
	binary.LittleEndian.PutUint32(b, uint32(n))
	b[4] = t
	for _, p := range payload {
		b = append(b, p...)
	}
	return b
}

func writeFrame(c net.Conn, f []byte) error {
	c.SetWriteDeadline(time.Now().Add(10 * time.Second))
	_, err := c.Write(f)
	return err
}

func u32(v uint32) []byte { return binary.LittleEndian.AppendUint32(nil, v) }
func u64(v uint64) []byte { return binary.LittleEndian.AppendUint64(nil, v) }

func sendError(c net.Conn, code uint32) { writeFrame(c, frame(tError, u32(code))) }

// infoOK: a NetworkInfo fit to be shown to other players (the game parses it).
func infoOK(i []byte) bool {
	if len(i) != netInfoSize {
		return false
	}
	max, count := i[niNodeMax], i[niNodeCount]
	return i[niNetType] == 2 && i[niSSID] <= 32 && max >= 1 && max <= maxNodes && count >= 1 && count <= max &&
		binary.LittleEndian.Uint16(i[niAdvSize:]) <= advMax
}

// ---- connections
func (s *Server) admit(ip string) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	st := s.ips[ip]
	if st == nil {
		st = &ipState{req: newBucket(s.cfg.ReqRate, s.cfg.ReqBurst)}
		s.ips[ip] = st
	}
	st.seen = time.Now()
	if s.conns >= s.cfg.MaxConns || st.conns >= s.cfg.MaxConnsPerIP {
		return false
	}
	s.conns++
	st.conns++
	return true
}

func (s *Server) release(ip string) {
	s.mu.Lock()
	s.conns--
	if st := s.ips[ip]; st != nil {
		st.conns--
		st.seen = time.Now()
	}
	s.mu.Unlock()
}

func (s *Server) allowRequest(ip string) bool {
	s.mu.Lock()
	st := s.ips[ip]
	s.mu.Unlock()
	return st != nil && st.req.take(1)
}

func (s *Server) handle(c *net.TCPConn) {
	ip := c.RemoteAddr().(*net.TCPAddr).IP.String()
	if !s.admit(ip) {
		c.Close()
		return
	}
	defer s.release(ip)
	defer c.Close()
	c.SetNoDelay(true)
	c.SetKeepAlive(true)
	c.SetKeepAlivePeriod(30 * time.Second)
	c.SetReadDeadline(time.Now().Add(s.cfg.HandshakeTime))
	t, p, err := readFrame(c, maxFrame)
	if err != nil || len(p) < headerSize || !bytes.Equal(p[:4], []byte("MHGS")) {
		return
	}
	if binary.LittleEndian.Uint16(p[4:]) != serverProto {
		sendError(c, errVersion)
		return
	}
	proto := binary.LittleEndian.Uint16(p[6:])
	var room [16]byte
	copy(room[:], p[8:24])
	if !s.Settings().serves(proto) { // (the game: "the hub server runs another version", LAN / Join hubs only)
		if t == tHost {
			s.vlog("hub refused (protocol %d, %s)", proto, roomName(room))
		}
		sendError(c, errVersion)
		return
	}
	body := p[headerSize:]
	v4 := proto >= protoSessions
	switch t {
	case tList:
		if !s.allowRequest(ip) {
			sendError(c, errBusy)
			return
		}
		s.list(c, proto, room)
	case tHost: // (NetworkInfo [, code[8] (protocol 4: zeros, or none, for no code)], and whatever a newer game adds)
		var code []byte
		if v4 && len(body) >= netInfoSize+codeLen && !zeros(body[netInfoSize:netInfoSize+codeLen]) {
			code = body[netInfoSize : netInfoSize+codeLen]
			if !codeOK(code) {
				sendError(c, errBadReq)
				return
			}
		}
		if len(body) < netInfoSize || !infoOK(body[:netInfoSize]) {
			sendError(c, errBadReq)
			return
		}
		s.host(c, ip, proto, room, body[:netInfoSize], code)
	case tAttach:
		if len(body) < 8 {
			return
		}
		s.attach(c, binary.LittleEndian.Uint64(body))
	case tJoin: // (u64 hub id [, code[8] (protocol 4)], ...)
		var code []byte
		if v4 && len(body) >= 8+codeLen && !zeros(body[8:8+codeLen]) {
			code = body[8 : 8+codeLen]
		}
		if len(body) < 8 {
			return
		}
		if !s.allowRequest(ip) {
			sendError(c, errBusy)
			return
		}
		s.join(c, proto, room, binary.LittleEndian.Uint64(body), code)
	case tFind:
		if !v4 || len(body) < codeLen {
			sendError(c, errBadReq)
			return
		}
		if !s.allowRequest(ip) {
			sendError(c, errBusy)
			return
		}
		s.find(c, proto, body[:codeLen])
	case tPresence:
		if !v4 {
			sendError(c, errBadReq)
			return
		}
		if !s.allowRequest(ip) {
			sendError(c, errBusy)
			return
		}
		s.presence(c, body)
	}
}

// passcoded: the hub asks a passcode (of everyone, or of everyone but friends: the same here, internet hub players
// aren't each other's Switch friends).
func passcoded(info []byte) bool {
	if int(binary.LittleEndian.Uint16(info[niAdvSize:])) < advPasscode+2 {
		return false
	}
	return binary.LittleEndian.Uint16(info[niAdv+advPasscode:]) != 0xFFFF
}

// huntSearch: a Hunter Search network, not a hub. With Hunter Search on (in a village or the offline Hub) the game
// hosts one for ~10 s between its searches, over and over: room for 2 players, and 0xFF at advertise byte 0x1C,
// where hubs have 1 (a 2-player hub too: checked). The game scans for 24 networks and shows up to 18 hubs in Join Hub and 6 players in its Hunter
// Search list, so in an answer these get places of their own instead of taking the hubs'. (The 8 bytes at 0x14 are the
// host's account id: the same for every game before protocol 4, its own for each install since; not looked at.)
func huntSearch(info []byte) bool {
	adv := info[niAdv:]
	return info[niNodeMax] == 2 && int(binary.LittleEndian.Uint16(info[niAdvSize:])) > advHuntSearch &&
		adv[advHuntSearch] == 0xFF
}

type listEntry struct {
	id      uint64
	info    [netInfoSize]byte
	created time.Time
	proto   uint16
	code    [codeLen]byte
	rank    int // 0 room and no passcode, 1 room and a passcode, 2 full, 3 full with a passcode
}

func entryOf(h *hub) listEntry {
	return listEntry{id: h.id, info: h.info, created: h.created, proto: h.proto, code: h.code}
}

// The answer to a list (or find): HUBS for a game before protocol 4, HUBS4 (with each hub's link protocol and code)
// for one of 4 and later.
func writeHubs(c net.Conn, proto uint16, es []listEntry) {
	out := u32(uint32(len(es)))
	for i := range es {
		out = append(out, u64(es[i].id)...)
		if proto >= protoSessions {
			out = binary.LittleEndian.AppendUint16(out, es[i].proto)
			out = append(out, es[i].code[:]...)
		}
		out = append(out, es[i].info[:]...)
	}
	t := byte(tHubs)
	if proto >= protoSessions {
		t = tHubs4
	}
	writeFrame(c, frame(t, out))
}

// pick: at most max of es for an answer, sorted: the ones with room for more players first (without a passcode before
// passcoded ones), then the fullest, then the newest. With more than max, a random choice by rank (so passcoded hubs
// only take the places open hubs with room leave), so each search (the game's Retry) can show others.
func pick(es []listEntry, max int) []listEntry {
	for i := range es {
		e := &es[i]
		if e.info[niNodeCount] >= e.info[niNodeMax] {
			e.rank = 2
		}
		if passcoded(e.info[:]) {
			e.rank++
		}
	}
	if max < 0 {
		max = 0
	}
	if len(es) > max {
		mrand.Shuffle(len(es), func(i, j int) { es[i], es[j] = es[j], es[i] })
		sort.SliceStable(es, func(i, j int) bool { return es[i].rank < es[j].rank })
		es = es[:max]
	}
	sort.Slice(es, func(i, j int) bool {
		a, b := &es[i], &es[j]
		if a.rank != b.rank {
			return a.rank < b.rank
		}
		if a.info[niNodeCount] != b.info[niNodeCount] {
			return a.info[niNodeCount] > b.info[niNodeCount]
		}
		return a.created.After(b.created)
	})
	return es
}

// The hubs a player with this key and version sees (up to ListMax, picked and sorted by pick), then the Hunter
// Search networks (up to ListSearchMax, the same way). They come last: the game puts LAN hubs before the server's,
// and an older game keeps fewer, so what doesn't fit is theirs.
func (s *Server) list(c *net.TCPConn, proto uint16, room [16]byte) {
	var hubs, searches []listEntry
	sep := s.separate.Load()
	room = meet(room, sep)
	set := s.Settings()
	group := set.group(proto)
	s.mu.Lock()
	for _, h := range s.hubs {
		if set.group(h.proto) == group && meet(h.room, sep) == room {
			e := entryOf(h)
			if huntSearch(h.info[:]) {
				searches = append(searches, e)
			} else {
				hubs = append(hubs, e)
			}
		}
	}
	s.mu.Unlock()
	writeHubs(c, proto, append(pick(hubs, s.cfg.ListMax), pick(searches, s.cfg.ListSearchMax)...))
}

// FIND (protocol 4): the hub holding a session code, whatever its room - the code is the invitation.
func (s *Server) find(c *net.TCPConn, proto uint16, code []byte) {
	var es []listEntry
	var k [codeLen]byte
	copy(k[:], code)
	s.mu.Lock()
	set := s.Settings()
	if h := s.codes[k]; h != nil && set.group(h.proto) == set.group(proto) {
		es = append(es, entryOf(h))
	}
	s.mu.Unlock()
	writeHubs(c, proto, es)
}

// sameHub: two listings of one network (its session id and BSSID).
func sameHub(a, b []byte) bool {
	return bytes.Equal(a[niSessionID:niBSSID+6], b[niSessionID:niBSSID+6])
}

func (s *Server) host(c *net.TCPConn, ip string, proto uint16, room [16]byte, info, code []byte) {
	h := &hub{room: room, proto: proto, conn: c, created: time.Now(), ip: ip, out: make(chan []byte, 16),
		done: make(chan struct{})}
	copy(h.info[:], info)
	copy(h.code[:], code)
	hasCode := code != nil
	s.mu.Lock()
	if !s.Settings().serves(proto) { // (Apply changed them since handle checked: Apply's sweep may have run already)
		s.mu.Unlock()
		sendError(c, errVersion)
		return
	}
	st := s.ips[ip]
	if len(s.hubs) >= s.cfg.MaxHubs || st.hubs >= s.cfg.MaxHubsPerIP {
		s.mu.Unlock()
		sendError(c, errBusy)
		return
	}
	var stale *net.TCPConn // the same hub's listing it left: its host's connection here, to close
	if hasCode {
		if o := s.codes[h.code]; o != nil {
			if !sameHub(o.info[:], h.info[:]) {
				s.mu.Unlock()
				sendError(c, errCodeTaken)
				return
			}
			stale = o.conn
		}
	}
	for h.id == 0 || s.hubs[h.id] != nil {
		h.id = rand64()
	}
	s.hubs[h.id] = h
	if hasCode {
		s.codes[h.code] = h
	}
	st.hubs++
	s.mu.Unlock()
	if stale != nil {
		stale.Close()
	}
	s.vlog("hub %016x up (%d players, protocol %d, %s)", h.id, h.info[niNodeCount], proto, roomName(room))
	defer func() {
		s.mu.Lock()
		delete(s.hubs, h.id)
		if hasCode && s.codes[h.code] == h {
			delete(s.codes, h.code)
		}
		if st := s.ips[ip]; st != nil {
			st.hubs--
		}
		s.mu.Unlock()
		close(h.done)
		s.vlog("hub %016x down", h.id)
	}()
	if writeFrame(c, frame(tHosted, u64(h.id))) != nil {
		return
	}
	// writer: tickets and pings; it closes the connection on a write error, which ends the reader too
	go func() {
		t := time.NewTicker(s.cfg.PingEvery)
		defer t.Stop()
		for {
			var f []byte
			select {
			case <-h.done:
				return
			case f = <-h.out:
			case <-t.C:
				f = frame(tPing)
			}
			if writeFrame(c, f) != nil {
				c.Close()
				return
			}
		}
	}()
	for {
		c.SetReadDeadline(time.Now().Add(3 * s.cfg.PingEvery))
		t, p, err := readFrame(c, maxFrame)
		if err != nil {
			return
		}
		switch t {
		case tUpdate:
			if !infoOK(p) {
				return
			}
			s.mu.Lock()
			copy(h.info[:], p)
			s.mu.Unlock()
		case tPing:
		default:
			return
		}
	}
}

// A player's JOIN: the hub must be in its room and play its link protocol (Settings.group) - or, with the hub's session
// code (protocol 4), in any room.
func (s *Server) join(c *net.TCPConn, proto uint16, room [16]byte, id uint64, code []byte) {
	s.mu.Lock()
	h := s.hubs[id]
	sep := s.separate.Load()
	invited := h != nil && code != nil && codeOK(code) && bytes.Equal(h.code[:], code)
	set := s.Settings()
	if h == nil || (!invited && meet(h.room, sep) != meet(room, sep)) || set.group(h.proto) != set.group(proto) {
		s.mu.Unlock()
		sendError(c, errNotFound)
		return
	}
	if h.joins >= maxNodes-1 {
		s.mu.Unlock()
		sendError(c, errBusy)
		return
	}
	var ticket uint64
	for ticket == 0 || s.pending[ticket] != nil {
		ticket = rand64()
	}
	pj := &pending{hub: h, host: make(chan *net.TCPConn, 1), finished: make(chan struct{})}
	s.pending[ticket] = pj
	h.joins++
	s.mu.Unlock()
	defer func() {
		s.mu.Lock()
		h.joins--
		delete(s.pending, ticket)
		s.mu.Unlock()
		close(pj.finished)
	}()
	select {
	case h.out <- frame(tIncoming, u64(ticket)):
	default:
		sendError(c, errBusy)
		return
	}
	var hc *net.TCPConn
	timeout := time.NewTimer(s.cfg.HandshakeTime)
	defer timeout.Stop()
	select {
	case hc = <-pj.host:
	case <-h.done:
	case <-timeout.C:
	}
	if hc == nil {
		s.mu.Lock()
		_, waiting := s.pending[ticket]
		delete(s.pending, ticket)
		s.mu.Unlock()
		if waiting { // (else the host's ATTACH took the ticket just now: its connection is on the way)
			sendError(c, errNoAnswer)
			return
		}
		hc = <-pj.host
	}
	p := &pair{secret: [2]uint64{rand64(), rand64()}, dkey: rand64(), lim: newBucket(s.cfg.Bandwidth, s.cfg.Bandwidth)}
	s.mu.Lock()
	for p.id == 0 || s.pairs[p.id] != nil {
		p.id = uint32(rand64())
	}
	s.pairs[p.id] = p
	s.mu.Unlock()
	defer func() {
		s.mu.Lock()
		delete(s.pairs, p.id)
		s.mu.Unlock()
	}()
	if writeFrame(hc, frame(tSpliced, u32(p.id), u64(p.secret[0]))) != nil ||
		writeFrame(c, frame(tSpliced, u32(p.id), u64(p.secret[1]))) != nil {
		hc.Close()
		return
	}
	s.vlog("hub %016x: player joined (pair %08x)", id, p.id)
	s.splice(hc, c)
	s.vlog("hub %016x: player left (pair %08x)", id, p.id)
}

// friendID: an install's friend code, from its secret.
func friendID(secret []byte) (id [friendLen]byte) {
	h := sha256.New()
	h.Write([]byte(friendSalt))
	h.Write(secret)
	copy(id[:], h.Sum(nil))
	return
}

// cleanName: a name for friend requests as the game sent it (UTF-8), without control characters or broken UTF-8, cut to
// at most nameMax bytes at a whole character.
func cleanName(b []byte) string {
	name := strings.Map(func(r rune) rune {
		if r < 0x20 || r == 0x7F {
			return -1
		}
		return r
	}, strings.ToValidUTF8(string(b), ""))
	for len(name) > nameMax {
		_, size := utf8.DecodeLastRuneInString(name)
		name = name[:len(name)-size]
	}
	return name
}

// PRESENCE (protocol 4): the asker is online (doing state, in or hosting the hub with code), until presenceTTL after
// this; the answer is which of the friends it lists are online and list it back, and what they do, then its friend
// requests: the players online that list it while it doesn't list them, with the names they go by (the newest
// maxRequests). Body: secret[32], u8 state, code[8] (zeros: none), u8 n, n x friend code[10], and optionally u8 len,
// name[len]: the name its own requests show; anything after that is ignored. What a newer game may send more of is
// taken as far as this server goes rather than refused: the first maxFriends friends, the name cut to nameMax bytes (and
// without control characters), a state it doesn't know as it came, and a code that isn't one as none.
func (s *Server) presence(c *net.TCPConn, body []byte) {
	const fixed = secretLen + 1 + codeLen + 1
	if len(body) < fixed {
		sendError(c, errBadReq)
		return
	}
	n := int(body[fixed-1])
	state, code := body[secretLen], body[secretLen+1:secretLen+1+codeLen]
	end := fixed + n*friendLen
	if len(body) < end { // (the friends cut short)
		sendError(c, errBadReq)
		return
	}
	var name string
	if len(body) > end { // (the name: optional; one that doesn't fit in the request is left out)
		if nl := int(body[end]); len(body) >= end+1+nl {
			name = cleanName(body[end+1 : end+1+nl])
		}
	}
	if !codeOK(code) {
		code = make([]byte, codeLen)
	}
	n = min(n, maxFriends)
	id := friendID(body[:secretLen])
	now := time.Now()
	me := &presence{friends: map[[friendLen]byte]bool{}, state: state, name: name, seen: now}
	copy(me.code[:], code)
	for i := 0; i < n; i++ {
		var f [friendLen]byte
		copy(f[:], body[fixed+i*friendLen:])
		if f != id {
			me.friends[f] = true
		}
	}
	var out []byte
	count := 0
	type request struct {
		id    [friendLen]byte
		name  string
		since time.Time
	}
	var reqs []request
	s.mu.Lock()
	old := s.online[id]
	if old == nil && len(s.online) >= s.cfg.MaxPresence {
		s.prunePresenceLocked()
		if len(s.online) >= s.cfg.MaxPresence {
			s.mu.Unlock()
			sendError(c, errBusy)
			return
		}
	}
	if old != nil {
		s.unlistLocked(id, old.friends, me.friends)
	}
	for f := range me.friends {
		m := s.listedBy[f]
		if m == nil {
			m = map[[friendLen]byte]time.Time{}
			s.listedBy[f] = m
		}
		if _, ok := m[id]; !ok {
			m[id] = now
		}
	}
	s.online[id] = me
	for f := range me.friends {
		o := s.online[f]
		if o == nil || time.Since(o.seen) > presenceTTL || !o.friends[id] {
			continue
		}
		out = append(out, f[:]...)
		out = append(out, o.state)
		out = append(out, o.code[:]...)
		count++
	}
	for r, since := range s.listedBy[id] {
		o := s.online[r]
		if me.friends[r] || o == nil || time.Since(o.seen) > presenceTTL {
			continue
		}
		reqs = append(reqs, request{r, o.name, since})
	}
	s.mu.Unlock()
	sort.Slice(reqs, func(i, j int) bool { return reqs[i].since.After(reqs[j].since) })
	if len(reqs) > maxRequests {
		reqs = reqs[:maxRequests]
	}
	tail := []byte{byte(len(reqs))}
	for _, r := range reqs {
		tail = append(tail, r.id[:]...)
		tail = append(tail, byte(len(r.name)))
		tail = append(tail, r.name...)
	}
	writeFrame(c, frame(tFriends, []byte{byte(count)}, out, tail))
}

// The host's connection for a ticket: handed to the waiting join, which splices it; it stays open until then.
func (s *Server) attach(c *net.TCPConn, ticket uint64) {
	s.mu.Lock()
	pj := s.pending[ticket]
	delete(s.pending, ticket)
	s.mu.Unlock()
	if pj == nil {
		sendError(c, errNotFound)
		return
	}
	pj.host <- c
	<-pj.finished
}

// Copies both ways until either end closes, at most Bandwidth bytes a second each way.
func (s *Server) splice(a, b *net.TCPConn) {
	a.SetDeadline(time.Time{})
	b.SetDeadline(time.Time{})
	done := make(chan struct{}, 2)
	pipe := func(dst, src *net.TCPConn) {
		lim := newBucket(s.cfg.Bandwidth, s.cfg.Bandwidth)
		buf := make([]byte, 16<<10)
		for {
			n, err := src.Read(buf)
			if n > 0 {
				if d := lim.wait(float64(n)); d > 0 {
					time.Sleep(d)
				}
				dst.SetWriteDeadline(time.Now().Add(60 * time.Second))
				if _, werr := dst.Write(buf[:n]); werr != nil {
					break
				}
			}
			if err != nil {
				break
			}
		}
		done <- struct{}{}
	}
	go pipe(a, b)
	go pipe(b, a)
	<-done
	a.Close()
	b.Close()
	<-done
}

// ---- UDP relay
func (s *Server) serveUDP() {
	buf := make([]byte, udpMax+1)
	for {
		n, from, err := s.udp.ReadFromUDP(buf)
		if err != nil {
			if errors.Is(err, net.ErrClosed) {
				return
			}
			continue
		}
		if n < udpHeader || n > udpMax || string(buf[:4]) != udpMagic {
			continue
		}
		id := binary.LittleEndian.Uint32(buf[4:])
		secret := binary.LittleEndian.Uint64(buf[8:])
		s.mu.Lock()
		p := s.pairs[id]
		s.mu.Unlock()
		if p == nil {
			continue
		}
		side := -1
		for i, sec := range p.secret {
			if sec == secret {
				side = i
			}
		}
		if side < 0 {
			continue
		}
		query := n == udpQuerySize && string(buf[udpHeader:udpHeader+4]) == peerQuery
		p.mu.Lock()
		p.addr[side] = from // (NAT may change it: the latest wins)
		to := p.addr[1-side]
		if query {
			p.direct[side] = true
		}
		both := p.direct[0] && p.direct[1]
		p.mu.Unlock()
		if query {
			// direct UDP (hole punching): the other end's address, once both ends asked for each other's; never bigger
			// than the query
			if both && to != nil && to.IP.To4() != nil && p.lim.take(float64(n)) {
				a := make([]byte, 0, udpQuerySize)
				a = append(a, peerAnswer...)
				a = binary.LittleEndian.AppendUint32(a, id)
				a = binary.LittleEndian.AppendUint64(a, secret)
				a = append(a, to.IP.To4()...)
				a = binary.BigEndian.AppendUint16(a, uint16(to.Port))
				a = append(a, 0, 0)
				a = binary.LittleEndian.AppendUint64(a, p.dkey)
				s.udp.WriteToUDP(a, from)
			}
			continue
		}
		// (an empty datagram only tells where its sender is)
		if n > udpHeader && to != nil && p.lim.take(float64(n)) {
			s.udp.WriteToUDP(buf[udpHeader:n], to)
		}
	}
}
