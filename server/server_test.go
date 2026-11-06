package main

import (
	"bytes"
	"encoding/binary"
	"net"
	"testing"
	"time"
)

func start(t *testing.T, edit func(*Config)) *Server {
	cfg := DefaultConfig()
	cfg.Addr = "127.0.0.1:0"
	cfg.HandshakeTime = 2 * time.Second
	if edit != nil {
		edit(&cfg)
	}
	s := NewServer(cfg)
	if err := s.Listen(); err != nil {
		t.Fatal(err)
	}
	go s.Serve()
	t.Cleanup(s.Close)
	return s
}

func dial(t *testing.T, s *Server) *net.TCPConn {
	c, err := net.DialTCP("tcp", nil, s.Addr())
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { c.Close() })
	c.SetDeadline(time.Now().Add(5 * time.Second))
	return c
}

func header(proto uint16, room byte) []byte {
	h := make([]byte, headerSize)
	copy(h, "MHGS")
	binary.LittleEndian.PutUint16(h[4:], serverProto)
	binary.LittleEndian.PutUint16(h[6:], proto)
	for i := 8; i < 24; i++ {
		h[i] = room
	}
	return h
}

func testInfo(players byte) []byte {
	i := make([]byte, netInfoSize)
	i[niNetType] = 2
	i[niSSID] = 10
	i[niNodeMax] = 4
	i[niNodeCount] = players
	return i
}

func send(t *testing.T, c net.Conn, typ byte, p ...[]byte) {
	if _, err := c.Write(frame(typ, p...)); err != nil {
		t.Fatal(err)
	}
}

func expect(t *testing.T, c net.Conn, typ byte) []byte {
	t.Helper()
	got, p, err := readFrame(c, 1<<20)
	if err != nil {
		t.Fatal(err)
	}
	if got != typ {
		t.Fatalf("frame %d (%x), want %d", got, p, typ)
	}
	return p
}

func host(t *testing.T, s *Server, room byte, players byte) (*net.TCPConn, uint64) {
	c := dial(t, s)
	send(t, c, tHost, header(2, room), testInfo(players))
	return c, binary.LittleEndian.Uint64(expect(t, c, tHosted))
}

func list(t *testing.T, s *Server, proto uint16, room byte) map[uint64][]byte {
	c := dial(t, s)
	send(t, c, tList, header(proto, room))
	p := expect(t, c, tHubs)
	n := binary.LittleEndian.Uint32(p)
	out := map[uint64][]byte{}
	for i := 0; i < int(n); i++ {
		e := p[4+i*(8+netInfoSize):]
		out[binary.LittleEndian.Uint64(e)] = e[8 : 8+netInfoSize]
	}
	return out
}

func TestKeysAndVersions(t *testing.T) {
	s := start(t, nil)
	_, public := host(t, s, 0, 1)
	_, friends := host(t, s, 7, 2)
	if l := list(t, s, 2, 0); len(l) != 1 || l[public] == nil {
		t.Fatalf("public list %v", l)
	}
	if l := list(t, s, 2, 7); len(l) != 1 || l[friends] == nil || l[friends][niNodeCount] != 2 {
		t.Fatalf("keyed list %v", l)
	}
	if l := list(t, s, 2, 8); len(l) != 0 {
		t.Fatal("another key sees the hub")
	}
	if l := list(t, s, 3, 0); len(l) != 0 {
		t.Fatal("another version sees the hub")
	}
	// a JOIN with the wrong key is told the hub doesn't exist
	c := dial(t, s)
	send(t, c, tJoin, header(2, 8), u64(friends))
	if code := binary.LittleEndian.Uint32(expect(t, c, tError)); code != errNotFound {
		t.Fatalf("error %d", code)
	}
}

// MinProto: older games are refused for every request (errVersion); that version and newer ones are served.
func TestMinProto(t *testing.T) {
	s := start(t, func(c *Config) { c.MinProto = 3 })
	for _, r := range []struct {
		typ  byte
		body []byte
	}{{tList, nil}, {tHost, testInfo(1)}, {tJoin, u64(1)}, {tAttach, u64(1)}} {
		c := dial(t, s)
		send(t, c, r.typ, header(2, 0), r.body)
		if code := binary.LittleEndian.Uint32(expect(t, c, tError)); code != errVersion {
			t.Fatalf("request %d: error %d", r.typ, code)
		}
	}
	c := dial(t, s)
	send(t, c, tHost, header(3, 0), testInfo(1))
	id := binary.LittleEndian.Uint64(expect(t, c, tHosted))
	if l := list(t, s, 3, 0); len(l) != 1 || l[id] == nil {
		t.Fatalf("protocol 3 list %v", l)
	}
	if l := list(t, s, 4, 0); len(l) != 0 {
		t.Fatal("protocol 4 sees a protocol 3 hub")
	}
}

// More hubs than ListMax: each answer is a random choice of hubs with a free place (full ones only when those run
// out), sorted (the fullest first), and repeated searches see more than ListMax of them.
func TestListRandomChoice(t *testing.T) {
	s := start(t, func(c *Config) {
		c.ListMax, c.MaxHubsPerIP, c.MaxConnsPerIP, c.ReqRate, c.ReqBurst = 4, 20, 100, 1000, 1000
	})
	full := map[uint64]bool{}
	for i := 0; i < 8; i++ {
		host(t, s, 0, byte(1+i%3))
	}
	for i := 0; i < 3; i++ {
		_, id := host(t, s, 0, 4)
		full[id] = true
	}
	seen := map[uint64]bool{}
	for n := 0; n < 30; n++ {
		c := dial(t, s)
		send(t, c, tList, header(2, 0))
		p := expect(t, c, tHubs)
		c.Close()
		if k := binary.LittleEndian.Uint32(p); k != 4 {
			t.Fatalf("%d hubs in an answer, want 4", k)
		}
		last := byte(255)
		for i := 0; i < 4; i++ {
			e := p[4+i*(8+netInfoSize):]
			id, players := binary.LittleEndian.Uint64(e), e[8+niNodeCount]
			if full[id] {
				t.Fatal("a full hub was listed while hubs with a free place were left out")
			}
			if players > last {
				t.Fatalf("answer not sorted: %d players after %d", players, last)
			}
			last = players
			seen[id] = true
		}
	}
	if len(seen) <= 4 {
		t.Fatalf("30 searches saw only %d hubs", len(seen))
	}
}

func TestUpdateAndClose(t *testing.T) {
	s := start(t, nil)
	hc, id := host(t, s, 0, 1)
	send(t, hc, tUpdate, testInfo(3))
	time.Sleep(100 * time.Millisecond)
	if l := list(t, s, 2, 0); l[id] == nil || l[id][niNodeCount] != 3 {
		t.Fatal("update not listed")
	}
	bad := testInfo(1)
	bad[niNodeCount] = 9
	send(t, hc, tUpdate, bad) // (the server drops the hub)
	time.Sleep(100 * time.Millisecond)
	if l := list(t, s, 2, 0); len(l) != 0 {
		t.Fatal("hub with a bad NetworkInfo still listed")
	}
}

func TestBadRequests(t *testing.T) {
	s := start(t, nil)
	c := dial(t, s)
	send(t, c, tHost, header(2, 0), testInfo(5)) // more players than the maximum
	expect(t, c, tError)
	c = dial(t, s)
	c.Write([]byte{0xff, 0xff, 0xff, 0x7f, 1}) // huge frame
	if _, _, err := readFrame(c, 100); err == nil {
		t.Fatal("connection still open")
	}
}

func TestJoinSpliceRelay(t *testing.T) {
	s := start(t, nil)
	hc, id := host(t, s, 0, 1)
	pc := dial(t, s)
	send(t, pc, tJoin, header(2, 0), u64(id))
	ticket := expect(t, hc, tIncoming)
	ac := dial(t, s)
	send(t, ac, tAttach, header(2, 0), ticket)
	hs := expect(t, ac, tSpliced)
	ps := expect(t, pc, tSpliced)
	if !bytes.Equal(hs[:4], ps[:4]) || bytes.Equal(hs[4:], ps[4:]) {
		t.Fatal("pair ids differ or secrets are the same")
	}
	// the link: bytes both ways, untouched
	ac.Write([]byte("from host"))
	pc.Write([]byte("from player"))
	b := make([]byte, 64)
	if n, _ := pc.Read(b); string(b[:n]) != "from host" {
		t.Fatalf("player got %q", b[:n])
	}
	if n, _ := ac.Read(b); string(b[:n]) != "from player" {
		t.Fatalf("host got %q", b[:n])
	}
	// UDP: the host announces itself, the player's datagram reaches it without the prefix
	ua := &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: s.Addr().Port}
	hu, _ := net.DialUDP("udp", nil, ua)
	pu, _ := net.DialUDP("udp", nil, ua)
	defer hu.Close()
	defer pu.Close()
	dgram := func(sec []byte, payload string) []byte {
		return append(append(append([]byte(udpMagic), ps[:4]...), sec...), payload...)
	}
	hu.Write(dgram(hs[4:12], ""))
	time.Sleep(50 * time.Millisecond)
	pu.Write(dgram(ps[4:12], "MHGDhello"))
	pu.Write(dgram(make([]byte, 8), "forged")) // wrong secret: dropped
	hu.SetReadDeadline(time.Now().Add(2 * time.Second))
	if n, _ := hu.Read(b); string(b[:n]) != "MHGDhello" {
		t.Fatalf("host got %q by UDP", b[:n])
	}
	hu.Write(dgram(hs[4:12], "MHGDack"))
	pu.SetReadDeadline(time.Now().Add(2 * time.Second))
	if n, _ := pu.Read(b); string(b[:n]) != "MHGDack" {
		t.Fatalf("player got %q by UDP", b[:n])
	}
	hu.SetReadDeadline(time.Now().Add(200 * time.Millisecond))
	if n, err := hu.Read(b); err == nil {
		t.Fatalf("forged datagram relayed: %q", b[:n])
	}
	// the player leaves: the host's side of the link closes
	pc.Close()
	ac.SetReadDeadline(time.Now().Add(2 * time.Second))
	if _, err := ac.Read(b); err == nil {
		t.Fatal("host's link still open")
	}
}

func TestHostDoesNotAnswer(t *testing.T) {
	s := start(t, func(c *Config) { c.HandshakeTime = 300 * time.Millisecond })
	hc, id := host(t, s, 0, 1)
	pc := dial(t, s)
	send(t, pc, tJoin, header(2, 0), u64(id))
	ticket := expect(t, hc, tIncoming)
	if code := binary.LittleEndian.Uint32(expect(t, pc, tError)); code != errNoAnswer {
		t.Fatalf("error %d", code)
	}
	ac := dial(t, s) // too late: the ticket is gone
	send(t, ac, tAttach, header(2, 0), ticket)
	expect(t, ac, tError)
}

func TestRateLimit(t *testing.T) {
	s := start(t, func(c *Config) { c.ReqRate, c.ReqBurst = 0.1, 3 })
	for i := 0; i < 3; i++ {
		list(t, s, 2, 0)
	}
	c := dial(t, s)
	send(t, c, tList, header(2, 0))
	if code := binary.LittleEndian.Uint32(expect(t, c, tError)); code != errBusy {
		t.Fatalf("error %d", code)
	}
}

func TestHubLimitPerIP(t *testing.T) {
	s := start(t, func(c *Config) { c.MaxHubsPerIP = 2 })
	host(t, s, 0, 1)
	host(t, s, 0, 1)
	c := dial(t, s)
	send(t, c, tHost, header(2, 0), testInfo(1))
	expect(t, c, tError)
}
