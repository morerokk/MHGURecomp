// The lobby / relay server for MHGURecomp's Local Play over the internet (runtime/net/ldn.c "internet hubs").
//
// A hub's host keeps one connection here (HOST): its hub is listed for players with the same matchmaking key and
// port version (LIST). A player who picks it (JOIN) gets a ticket sent to the host, the host opens a second
// connection with that ticket (ATTACH), and from then on the two connections are spliced: the bytes are the same
// link the games speak over a direct connection (ldn.c link frames), which this server neither reads nor changes.
// Game packets go by UDP when that gets through: datagrams to this server's UDP port, prefixed with the pair's id
// and the sender's secret, are passed to the other end of the pair (without the prefix). Nobody needs a forwarded
// port, and players never see each other's IP addresses.
//
// Control frames: u32 size (of what follows), u8 type, payload; little endian. The first frame of a connection is a
// request, whose payload starts with a header: "MHGS", u16 server protocol, u16 link protocol (the port's
// PROTO_VERSION: only hubs of the same one are listed or joined), room[16] (the first 16 bytes of SHA-256 of the
// matchmaking key, all zero without a key).
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
	"sync"
	"time"
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
	tJoin   = 4 // header, u64 hub id -> SPLICED (the player's side) or ERROR
	// on a hub's connection
	tUpdate = 5 // NetworkInfo: the hub changed (players, advertise data)
	tPing   = 6 // keeps the connection alive (both ways)
	// answers
	tHubs     = 16 // u32 count, count x (u64 hub id, NetworkInfo)
	tHosted   = 17 // u64 hub id
	tIncoming = 18 // u64 ticket: a player joins, ATTACH with it
	tSpliced  = 19 // u32 pair id, u64 secret (for UDP); then the connection is the link
	tError    = 20 // u32 code

	errNotFound = 1 // no such hub (or not for this key / version)
	errNoAnswer = 2 // the host did not take the player
	errBusy     = 3 // a limit was reached
	errVersion  = 4 // server protocol mismatch, or a link protocol older than Config.MinProto
	errBadReq   = 5

	udpMagic  = "MHGR"
	udpHeader = 16 // magic, u32 pair, u64 secret
	udpMax    = udpHeader + 12 + 16 + 8192

	// NetworkInfo offsets (runtime/net/ldn.c)
	niSSID      = 0x26
	niNetType   = 0x4B
	niNodeMax   = 0x66
	niNodeCount = 0x67
	niAdvSize   = 0x26A
	advMax      = 0x180
	maxNodes    = 8
)

type Config struct {
	Addr          string
	MaxConns      int     // all connections
	MaxConnsPerIP int     // connections from one IP address
	MaxHubs       int     // hubs listed at once
	MaxHubsPerIP  int     // hubs one IP address hosts
	ListMax       int     // hubs in one LIST answer (the game's Join Hub list shows 18: 3 pages of 6)
	ReqRate       float64 // LIST / JOIN requests a second per IP address (burst: ReqBurst)
	ReqBurst      float64
	Bandwidth     float64       // bytes a second per pair and direction (TCP), and per pair (UDP)
	HandshakeTime time.Duration // for the first frame, and a joining player's wait for the host
	PingEvery     time.Duration
	MinProto      uint16 // the oldest link protocol (PROTO_VERSION) served; older games get errVersion (0: any)
	Verbose       bool
}

func DefaultConfig() Config {
	return Config{
		Addr: ":39510", MaxConns: 4000, MaxConnsPerIP: 24, MaxHubs: 2000, MaxHubsPerIP: 4, ListMax: 18,
		ReqRate: 2, ReqBurst: 15, Bandwidth: 1 << 20, HandshakeTime: 10 * time.Second, PingEvery: 15 * time.Second,
	}
}

type hub struct {
	id      uint64
	room    [16]byte
	proto   uint16
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
	lim    *bucket
}

type ipState struct {
	conns, hubs int
	req         *bucket
	seen        time.Time
}

type Server struct {
	cfg     Config
	mu      sync.Mutex
	hubs    map[uint64]*hub
	pending map[uint64]*pending
	pairs   map[uint32]*pair
	ips     map[string]*ipState
	conns   int
	ln      *net.TCPListener
	udp     *net.UDPConn
}

func NewServer(cfg Config) *Server {
	return &Server{cfg: cfg, hubs: map[uint64]*hub{}, pending: map[uint64]*pending{}, pairs: map[uint32]*pair{},
		ips: map[string]*ipState{}}
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

// Forgets the request budgets of addresses not seen for a while.
func (s *Server) janitor() {
	for range time.Tick(time.Minute) {
		s.mu.Lock()
		for ip, st := range s.ips {
			if st.conns == 0 && st.hubs == 0 && time.Since(st.seen) > 5*time.Minute {
				delete(s.ips, ip)
			}
		}
		s.mu.Unlock()
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
	if proto < s.cfg.MinProto { // (the game: "the hub server runs another version", LAN / Join hubs only)
		if t == tHost {
			s.vlog("hub refused (protocol %d, %s)", proto, roomName(room))
		}
		sendError(c, errVersion)
		return
	}
	body := p[headerSize:]
	switch t {
	case tList:
		if !s.allowRequest(ip) {
			sendError(c, errBusy)
			return
		}
		s.list(c, proto, room)
	case tHost:
		if len(body) != netInfoSize || !infoOK(body) {
			sendError(c, errBadReq)
			return
		}
		s.host(c, ip, proto, room, body)
	case tAttach:
		if len(body) != 8 {
			return
		}
		s.attach(c, binary.LittleEndian.Uint64(body))
	case tJoin:
		if len(body) != 8 {
			return
		}
		if !s.allowRequest(ip) {
			sendError(c, errBusy)
			return
		}
		s.join(c, proto, room, binary.LittleEndian.Uint64(body))
	}
}

// The hubs a player with this key and version sees: the ones with room for more players first, then the fullest,
// then the newest.
func (s *Server) list(c *net.TCPConn, proto uint16, room [16]byte) {
	type entry struct {
		id      uint64
		info    [netInfoSize]byte
		created time.Time
	}
	var es []entry
	s.mu.Lock()
	for _, h := range s.hubs {
		if h.proto == proto && h.room == room {
			es = append(es, entry{h.id, h.info, h.created})
		}
	}
	s.mu.Unlock()
	free := func(e *entry) bool { return e.info[niNodeCount] < e.info[niNodeMax] }
	// more hubs than one answer holds: a random ListMax of them, those with a free place before full ones, so each
	// search (the game's Retry) can show others
	if len(es) > s.cfg.ListMax {
		mrand.Shuffle(len(es), func(i, j int) { es[i], es[j] = es[j], es[i] })
		sort.SliceStable(es, func(i, j int) bool { return free(&es[i]) && !free(&es[j]) })
		es = es[:s.cfg.ListMax]
	}
	sort.Slice(es, func(i, j int) bool {
		a, b := &es[i], &es[j]
		if free(a) != free(b) {
			return free(a)
		}
		if a.info[niNodeCount] != b.info[niNodeCount] {
			return a.info[niNodeCount] > b.info[niNodeCount]
		}
		return a.created.After(b.created)
	})
	out := u32(uint32(len(es)))
	for i := range es {
		out = append(out, u64(es[i].id)...)
		out = append(out, es[i].info[:]...)
	}
	writeFrame(c, frame(tHubs, out))
}

func (s *Server) host(c *net.TCPConn, ip string, proto uint16, room [16]byte, info []byte) {
	h := &hub{room: room, proto: proto, created: time.Now(), ip: ip, out: make(chan []byte, 16),
		done: make(chan struct{})}
	copy(h.info[:], info)
	s.mu.Lock()
	st := s.ips[ip]
	if len(s.hubs) >= s.cfg.MaxHubs || st.hubs >= s.cfg.MaxHubsPerIP {
		s.mu.Unlock()
		sendError(c, errBusy)
		return
	}
	for h.id == 0 || s.hubs[h.id] != nil {
		h.id = rand64()
	}
	s.hubs[h.id] = h
	st.hubs++
	s.mu.Unlock()
	s.vlog("hub %016x up (%d players, protocol %d, %s)", h.id, h.info[niNodeCount], proto, roomName(room))
	defer func() {
		s.mu.Lock()
		delete(s.hubs, h.id)
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

func (s *Server) join(c *net.TCPConn, proto uint16, room [16]byte, id uint64) {
	s.mu.Lock()
	h := s.hubs[id]
	if h == nil || h.room != room || h.proto != proto {
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
	p := &pair{secret: [2]uint64{rand64(), rand64()}, lim: newBucket(s.cfg.Bandwidth, s.cfg.Bandwidth)}
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
		p.mu.Lock()
		p.addr[side] = from // (NAT may change it: the latest wins)
		to := p.addr[1-side]
		p.mu.Unlock()
		// (an empty datagram only tells where its sender is)
		if n > udpHeader && to != nil && p.lim.take(float64(n)) {
			s.udp.WriteToUDP(buf[udpHeader:n], to)
		}
	}
}
