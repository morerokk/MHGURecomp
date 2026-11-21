package main

import (
	"bytes"
	"encoding/binary"
	"errors"
	"net"
	"os"
	"testing"
	"time"
)

// Protocol 4: one pool with 3, HUBS4, session codes, FIND, JOIN with a code, PRESENCE.

func header4(proto uint16, room [16]byte) []byte {
	h := header(proto, 0)
	copy(h[8:24], room[:])
	return h
}

// infoWith: a NetworkInfo for one hub: its session id and BSSID from n.
func infoWith(n byte) []byte {
	i := testInfo(1)
	i[niSessionID] = n
	i[niBSSID] = n
	return i
}

type hub4 struct {
	id    uint64
	proto uint16
	code  string
	info  []byte
}

// closedByServer: the server closed the connection within 2 s (pings may come first).
func closedByServer(c *net.TCPConn) bool {
	c.SetReadDeadline(time.Now().Add(2 * time.Second))
	for {
		if _, _, err := readFrame(c, maxFrame); err != nil {
			return !errors.Is(err, os.ErrDeadlineExceeded)
		}
	}
}

func readHubs4(t *testing.T, c net.Conn) []hub4 {
	t.Helper()
	p := expect(t, c, tHubs4)
	n := int(binary.LittleEndian.Uint32(p))
	const entry = 8 + 2 + codeLen + netInfoSize
	if len(p) != 4+n*entry {
		t.Fatalf("HUBS4 of %d bytes for %d hubs", len(p), n)
	}
	var out []hub4
	for i := 0; i < n; i++ {
		e := p[4+i*entry:]
		code := string(bytes.TrimRight(e[10:10+codeLen], "\x00"))
		out = append(out, hub4{binary.LittleEndian.Uint64(e), binary.LittleEndian.Uint16(e[8:]), code, e[10+codeLen : entry]})
	}
	return out
}

// hostAs: a hub of this protocol in this room (code "" : none); its host connection and id (0: refused, with the error).
func hostAs(t *testing.T, s *Server, proto uint16, room [16]byte, info []byte, code string) (*net.TCPConn, uint64, uint32) {
	t.Helper()
	c := dial(t, s)
	if code != "" {
		send(t, c, tHost, header4(proto, room), info, []byte(code))
	} else {
		send(t, c, tHost, header4(proto, room), info)
	}
	typ, p, err := readFrame(c, 1<<20)
	if err != nil {
		t.Fatal(err)
	}
	if typ == tError {
		return c, 0, binary.LittleEndian.Uint32(p)
	}
	if typ != tHosted {
		t.Fatalf("frame %d", typ)
	}
	return c, binary.LittleEndian.Uint64(p), 0
}

func ids3(t *testing.T, s *Server, proto uint16, room [16]byte) map[uint64]bool {
	t.Helper()
	c := dial(t, s)
	send(t, c, tList, header4(proto, room))
	out := map[uint64]bool{}
	if proto >= protoSessions {
		for _, h := range readHubs4(t, c) {
			out[h.id] = true
		}
		return out
	}
	p := expect(t, c, tHubs)
	for i := 0; i < int(binary.LittleEndian.Uint32(p)); i++ {
		out[binary.LittleEndian.Uint64(p[4+i*(8+netInfoSize):])] = true
	}
	return out
}

// 3 and 4 share one pool: each lists and joins the other's hubs; 2 and 5 stay apart.
func TestPool34(t *testing.T) {
	s := start(t, nil)
	h3, id3, _ := hostAs(t, s, 3, [16]byte{}, infoWith(1), "")
	h4, id4, _ := hostAs(t, s, 4, [16]byte{}, infoWith(2), "ABCD2345")
	_, id2, _ := hostAs(t, s, 2, [16]byte{}, infoWith(3), "")
	_, id5, _ := hostAs(t, s, 5, [16]byte{}, infoWith(4), "")
	for _, p := range []uint16{3, 4} {
		l := ids3(t, s, p, [16]byte{})
		if len(l) != 2 || !l[id3] || !l[id4] {
			t.Fatalf("protocol %d sees %v", p, l)
		}
	}
	if l := ids3(t, s, 2, [16]byte{}); len(l) != 1 || !l[id2] {
		t.Fatalf("protocol 2 sees %v", l)
	}
	if l := ids3(t, s, 5, [16]byte{}); len(l) != 1 || !l[id5] {
		t.Fatalf("protocol 5 sees %v", l)
	}
	// a protocol 3 player joins the protocol 4 hub, and a protocol 4 player the protocol 3 hub
	for _, j := range []struct {
		proto uint16
		id    uint64
		host  *net.TCPConn
	}{{3, id4, h4}, {4, id3, h3}} {
		c := dial(t, s)
		send(t, c, tJoin, header4(j.proto, [16]byte{}), u64(j.id))
		expect(t, j.host, tIncoming)
	}
}

// HUBS4: each hub's link protocol and code, for a protocol 4 asker only; a protocol 3 asker gets plain HUBS.
func TestHubs4(t *testing.T) {
	s := start(t, nil)
	hostAs(t, s, 3, [16]byte{}, infoWith(1), "")
	_, id4, _ := hostAs(t, s, 4, [16]byte{}, infoWith(2), "WXYZ0789")
	c := dial(t, s)
	send(t, c, tList, header4(4, [16]byte{}))
	got := map[uint64]hub4{}
	for _, h := range readHubs4(t, c) {
		got[h.id] = h
	}
	if h := got[id4]; h.proto != 4 || h.code != "WXYZ0789" || !bytes.Equal(h.info, infoWith(2)) {
		t.Fatalf("protocol 4 hub as %+v", h)
	}
	for id, h := range got {
		if id != id4 && (h.proto != 3 || h.code != "") {
			t.Fatalf("protocol 3 hub as %+v", h)
		}
	}
	c = dial(t, s)
	send(t, c, tList, header4(3, [16]byte{}))
	expect(t, c, tHubs)
}

// Session codes: only the alphabet's; a code another hub holds is refused; the same hub listing again takes it back
// (and its old listing is closed); a gone hub frees its code.
func TestSessionCodes(t *testing.T) {
	s := start(t, nil)
	for _, bad := range []string{"abcd2345", "ABCDI345", "ABC", "ABCD23456"} {
		c := dial(t, s)
		send(t, c, tHost, header4(4, [16]byte{}), infoWith(1), []byte(bad))
		if typ, _, _ := readFrame(c, 1<<20); typ != tError && len(bad) == codeLen {
			t.Errorf("code %q taken", bad)
		}
	}
	old, id1, _ := hostAs(t, s, 4, [16]byte{}, infoWith(1), "QQQQ1111")
	if id1 == 0 {
		t.Fatal("refused")
	}
	if _, _, e := hostAs(t, s, 4, [16]byte{}, infoWith(2), "QQQQ1111"); e != errCodeTaken {
		t.Fatalf("another hub got a taken code: error %d", e)
	}
	again, id2, _ := hostAs(t, s, 4, [16]byte{}, infoWith(1), "QQQQ1111")
	if id2 == 0 || id2 == id1 {
		t.Fatalf("the same hub listing again: %x (was %x)", id2, id1)
	}
	if !closedByServer(old) {
		t.Fatal("the old listing stays")
	}
	f := dial(t, s)
	send(t, f, tFind, header4(4, [16]byte{}), []byte("QQQQ1111"))
	if l := readHubs4(t, f); len(l) != 1 || l[0].id != id2 {
		t.Fatalf("find after relisting: %+v", l)
	}
	again.Close()
	time.Sleep(100 * time.Millisecond)
	if _, id3, e := hostAs(t, s, 4, [16]byte{}, infoWith(2), "QQQQ1111"); id3 == 0 {
		t.Fatalf("a gone hub's code stays taken: error %d", e)
	}
	// a protocol 3 game never sends a code: the same bytes are a bad request for it
	c := dial(t, s)
	send(t, c, tHost, header4(3, [16]byte{}), infoWith(5), []byte("ABCD2345"))
	if typ, p, _ := readFrame(c, 1<<20); typ != tError || binary.LittleEndian.Uint32(p) != errBadReq {
		t.Fatal("protocol 3 HOST with a code accepted")
	}
}

// FIND and JOIN with a code: any room; without the code a room must match; protocol 3 can't FIND.
func TestFindAndInvite(t *testing.T) {
	s := start(t, nil)
	var keyed [16]byte
	keyed[0] = 9
	h, id, _ := hostAs(t, s, 4, keyed, infoWith(1), "KEYD0001")
	f := dial(t, s)
	send(t, f, tFind, header4(4, [16]byte{}), []byte("KEYD0001"))
	if l := readHubs4(t, f); len(l) != 1 || l[0].id != id || l[0].code != "KEYD0001" {
		t.Fatalf("find: %+v", l)
	}
	f = dial(t, s)
	send(t, f, tFind, header4(4, [16]byte{}), []byte("NONE0000"))
	if l := readHubs4(t, f); len(l) != 0 {
		t.Fatalf("find of no hub: %+v", l)
	}
	f = dial(t, s)
	send(t, f, tFind, header4(3, [16]byte{}), []byte("KEYD0001"))
	if typ, _, _ := readFrame(f, 1<<20); typ != tError {
		t.Fatal("protocol 3 FIND answered")
	}
	// from the public room: refused without the code, let in with it (also from protocol 3? no: 3 sends no code)
	j := dial(t, s)
	send(t, j, tJoin, header4(4, [16]byte{}), u64(id))
	if typ, p, _ := readFrame(j, 1<<20); typ != tError || binary.LittleEndian.Uint32(p) != errNotFound {
		t.Fatal("joined another room's hub without its code")
	}
	j = dial(t, s)
	send(t, j, tJoin, header4(4, [16]byte{}), u64(id), []byte("WRONG000"))
	if typ, _, _ := readFrame(j, 1<<20); typ != tError {
		t.Fatal("joined with a wrong code")
	}
	j = dial(t, s)
	send(t, j, tJoin, header4(4, [16]byte{}), u64(id), []byte("KEYD0001"))
	expect(t, h, tIncoming)
}

func presenceBody(secret byte, state byte, code string, friends ...[friendLen]byte) []byte {
	b := bytes.Repeat([]byte{secret}, secretLen)
	b = append(b, state)
	c := make([]byte, codeLen)
	copy(c, code)
	b = append(b, c...)
	b = append(b, byte(len(friends)))
	for _, f := range friends {
		b = append(b, f[:]...)
	}
	return b
}

func idOf(secret byte) [friendLen]byte { return friendID(bytes.Repeat([]byte{secret}, secretLen)) }

type friendState struct {
	state byte
	code  string
}

func askPresence(t *testing.T, s *Server, body []byte) map[[friendLen]byte]friendState {
	t.Helper()
	friends, _ := askPresence2(t, s, body)
	return friends
}

type friendRequest struct {
	id   [friendLen]byte
	name string
}

// askPresence2: the answer's friends and its friend requests (in their order).
func askPresence2(t *testing.T, s *Server, body []byte) (map[[friendLen]byte]friendState, []friendRequest) {
	t.Helper()
	c := dial(t, s)
	send(t, c, tPresence, header4(4, [16]byte{}), body)
	p := expect(t, c, tFriends)
	const entry = friendLen + 1 + codeLen
	if len(p) < 2+int(p[0])*entry {
		t.Fatalf("FRIENDS of %d bytes for %d", len(p), p[0])
	}
	out := map[[friendLen]byte]friendState{}
	for i := 0; i < int(p[0]); i++ {
		e := p[1+i*entry:]
		var f [friendLen]byte
		copy(f[:], e)
		out[f] = friendState{e[friendLen], string(bytes.TrimRight(e[friendLen+1:entry], "\x00"))}
	}
	r := p[1+int(p[0])*entry:]
	var reqs []friendRequest
	n, at := int(r[0]), 1
	for i := 0; i < n; i++ {
		var f [friendLen]byte
		copy(f[:], r[at:])
		nl := int(r[at+friendLen])
		reqs = append(reqs, friendRequest{f, string(r[at+friendLen+1 : at+friendLen+1+nl])})
		at += friendLen + 1 + nl
	}
	if at != len(r) {
		t.Fatalf("FRIENDS: %d bytes after the requests", len(r)-at)
	}
	return out, reqs
}

func named(body []byte, name string) []byte { return append(append(body, byte(len(name))), name...) }

// Friend requests: a player online that lists you while you don't list it, with the name it sent; gone once you
// list it back (a friend then), when it stops listing you, or when it goes offline; the newest maxRequests.
func TestFriendRequests(t *testing.T) {
	s := start(t, func(c *Config) { c.ReqRate, c.ReqBurst = 1000, 1000 })
	a, b := idOf(1), idOf(2)
	askPresence(t, s, presenceBody(2, stateOnline, "")) // b online, listing nobody
	if _, reqs := askPresence2(t, s, named(presenceBody(1, stateOnline, "", b), "Mia")); len(reqs) != 0 {
		t.Fatalf("a has requests: %v", reqs)
	}
	_, reqs := askPresence2(t, s, presenceBody(2, stateOnline, ""))
	if len(reqs) != 1 || reqs[0] != (friendRequest{a, "Mia"}) {
		t.Fatalf("b's requests: %v", reqs)
	}
	// b accepts (lists a): a friend now, no request
	friends, reqs := askPresence2(t, s, presenceBody(2, stateOnline, "", a))
	if len(reqs) != 0 || len(friends) != 1 {
		t.Fatalf("after accepting: friends %v, requests %v", friends, reqs)
	}
	// a stops listing b: b's listing of a is a request to a now
	askPresence(t, s, named(presenceBody(1, stateOnline, ""), "Mia"))
	if _, reqs := askPresence2(t, s, presenceBody(2, stateOnline, "", a)); len(reqs) != 0 {
		t.Fatalf("b still gets a's request after a removed b: %v", reqs)
	}
	if _, reqs := askPresence2(t, s, presenceBody(1, stateOnline, "")); len(reqs) != 1 || reqs[0].id != b || reqs[0].name != "" {
		t.Fatalf("a's requests: %v", reqs)
	}
	// offline requesters aren't told
	s.mu.Lock()
	s.online[b].seen = time.Now().Add(-2 * presenceTTL)
	s.mu.Unlock()
	if _, reqs := askPresence2(t, s, presenceBody(1, stateOnline, "")); len(reqs) != 0 {
		t.Fatalf("an offline player's request told: %v", reqs)
	}
	// the newest maxRequests of many
	for i := 0; i < maxRequests+4; i++ {
		askPresence(t, s, named(presenceBody(byte(10+i), stateOnline, "", idOf(3)), "p"))
		time.Sleep(2 * time.Millisecond)
	}
	_, reqs = askPresence2(t, s, presenceBody(3, stateOnline, ""))
	if len(reqs) != maxRequests || reqs[0].id != idOf(byte(10+maxRequests+3)) {
		t.Fatalf("%d requests, first %x", len(reqs), reqs[0].id)
	}
	// expired players leave the index
	s.mu.Lock()
	for _, p := range s.online {
		p.seen = time.Now().Add(-2 * presenceTTL)
	}
	s.prunePresenceLocked()
	left := len(s.listedBy)
	s.mu.Unlock()
	if left != 0 {
		t.Fatalf("listedBy keeps %d entries after everyone left", left)
	}
	// names: at most nameMax bytes, no control characters
	for _, bad := range [][]byte{named(presenceBody(1, stateOnline, ""), string(make([]byte, nameMax+1))),
		named(presenceBody(1, stateOnline, ""), "a\nb"), append(presenceBody(1, stateOnline, ""), 5, 'x')} {
		c := dial(t, s)
		send(t, c, tPresence, header4(4, [16]byte{}), bad)
		if typ, _, _ := readFrame(c, 1<<20); typ != tError {
			t.Errorf("bad name %q answered", bad[len(bad)-3:])
		}
	}
}

// Presence: told only between players who list each other; state and code as last sent; stale ones are not told.
func TestPresence(t *testing.T) {
	s := start(t, nil)
	a, b, c := idOf(1), idOf(2), idOf(3)
	if got := askPresence(t, s, presenceBody(1, stateOnline, "", b, c)); len(got) != 0 {
		t.Fatalf("nobody online yet: %v", got)
	}
	// b lists a back, hosting; c is online but doesn't list a
	if got := askPresence(t, s, presenceBody(2, stateHosting, "BBBB2222", a)); len(got) != 1 || got[a] != (friendState{stateOnline, ""}) {
		t.Fatalf("b sees %v", got)
	}
	askPresence(t, s, presenceBody(3, stateOnline, "", b))
	got := askPresence(t, s, presenceBody(1, stateInHub, "BBBB2222", b, c))
	if len(got) != 1 || got[b] != (friendState{stateHosting, "BBBB2222"}) {
		t.Fatalf("a sees %v (c doesn't list a: not told)", got)
	}
	// stale: b's last request long ago
	s.mu.Lock()
	s.online[b].seen = time.Now().Add(-2 * presenceTTL)
	s.mu.Unlock()
	if got := askPresence(t, s, presenceBody(1, stateOnline, "", b, c)); len(got) != 0 {
		t.Fatalf("a stale friend told: %v", got)
	}
	// bad bodies
	for _, bad := range [][]byte{
		presenceBody(1, 9, "", b),                         // no such state
		presenceBody(1, stateHosting, "bad!", b),          // not a code
		presenceBody(1, stateOnline, "")[:secretLen],      // cut short
		append(presenceBody(1, stateOnline, "", b), 0, 0), // a byte after an empty name
	} {
		cn := dial(t, s)
		send(t, cn, tPresence, header4(4, [16]byte{}), bad)
		if typ, _, _ := readFrame(cn, 1<<20); typ != tError {
			t.Errorf("bad presence %x answered", bad)
		}
	}
	// protocol 3 has no presence
	cn := dial(t, s)
	send(t, cn, tPresence, header4(3, [16]byte{}), presenceBody(1, stateOnline, ""))
	if typ, _, _ := readFrame(cn, 1<<20); typ != tError {
		t.Fatal("protocol 3 presence answered")
	}
}

// A full presence table refuses new players (busy) but keeps answering the ones in it.
func TestPresenceFull(t *testing.T) {
	s := start(t, func(c *Config) { c.MaxPresence = 2 })
	askPresence(t, s, presenceBody(1, stateOnline, ""))
	askPresence(t, s, presenceBody(2, stateOnline, ""))
	c := dial(t, s)
	send(t, c, tPresence, header4(4, [16]byte{}), presenceBody(3, stateOnline, ""))
	if typ, p, _ := readFrame(c, 1<<20); typ != tError || binary.LittleEndian.Uint32(p) != errBusy {
		t.Fatal("a third player taken")
	}
	askPresence(t, s, presenceBody(1, stateOnline, ""))
}
