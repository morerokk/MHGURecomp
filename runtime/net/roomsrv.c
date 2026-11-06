// A suyu-family multiplayer room that runs inside the game (roomsrv.h). Written from the room protocol as the emulators'
// members speak it (the same wire format room.c uses as a member): ENet, one channel, every message reliable, the first byte
// is the message id, numbers are big-endian, a string is a u32 length and its bytes.
//
// The room does four things: lets members in (name, password, version, space, address), tells everybody who is in, passes the
// members' LDN packets and game packets on (to one member by its address, or to everybody else when broadcast), and sends a
// packet for an address nobody has to the host member (the "gateway": the host answers for the Ryujinx players' addresses).
#include <enet/enet.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "roomsrv.h"

enum {  // message ids (room.c has the same list from the member's side)
    M_JOIN_REQUEST = 1, M_JOIN_SUCCESS, M_ROOM_INFO, M_SET_GAME_INFO, M_PROXY_PACKET, M_LDN_PACKET, M_CHAT, M_NAME_COLLISION,
    M_IP_COLLISION, M_VERSION_MISMATCH, M_WRONG_PASSWORD, M_CLOSE_ROOM, M_ROOM_IS_FULL,
};
#define ROOM_VERSION 1u
#define MAX_MEMBERS 16
#define NAME_MAX_LEN 63
#define TEXT_MAX 96
#define MAX_MESSAGE 0x20000

typedef struct {
    ENetPeer *peer;
    char nick[NAME_MAX_LEN + 1];
    uint8_t ip[4];
    char game[TEXT_MAX], version[32];
    uint64_t game_id;
} Member;

static struct {
    ENetHost *host;
    pthread_t th;
    volatile bool run, up;
    char name[64], password[64], host_nick[NAME_MAX_LEN + 1];
    uint16_t port;
    Member m[MAX_MEMBERS];
    int n;
} S;

// ---- writing and reading
typedef struct { uint8_t *p; uint32_t n, cap; bool bad; } Wr;
static void wr_bytes(Wr *w, const void *d, uint32_t n) {
    if (w->n + n > w->cap) { w->bad = true; return; }
    memcpy(w->p + w->n, d, n);
    w->n += n;
}
static void wr_u8(Wr *w, uint8_t v) { wr_bytes(w, &v, 1); }
static void wr_u16(Wr *w, uint16_t v) { uint8_t b[2] = {(uint8_t)(v >> 8), (uint8_t)v}; wr_bytes(w, b, 2); }
static void wr_u32(Wr *w, uint32_t v) { uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v}; wr_bytes(w, b, 4); }
static void wr_u64(Wr *w, uint64_t v) { wr_u32(w, (uint32_t)(v >> 32)); wr_u32(w, (uint32_t)v); }
static void wr_str(Wr *w, const char *s) { uint32_t n = (uint32_t)strlen(s); wr_u32(w, n); wr_bytes(w, s, n); }

typedef struct { const uint8_t *p; uint32_t n, pos; bool bad; } Rd;
static const uint8_t *rd_bytes(Rd *r, uint32_t n) {
    if (r->bad || r->n - r->pos < n) { r->bad = true; return NULL; }
    const uint8_t *q = r->p + r->pos;
    r->pos += n;
    return q;
}
static uint8_t rd_u8(Rd *r) { const uint8_t *q = rd_bytes(r, 1); return q ? q[0] : 0; }
static uint32_t rd_u32(Rd *r) {
    const uint8_t *q = rd_bytes(r, 4);
    return q ? (uint32_t)q[0] << 24 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 8 | q[3] : 0;
}
static uint64_t rd_u64(Rd *r) { uint64_t hi = rd_u32(r); return hi << 32 | rd_u32(r); }
// A string into out (cut to max-1); false when the message is short.
static bool rd_str(Rd *r, char *out, size_t max) {
    uint32_t n = rd_u32(r);
    const uint8_t *q = rd_bytes(r, n);
    if (!q) { out[0] = 0; return false; }
    size_t k = n < max - 1 ? n : max - 1;
    memcpy(out, q, k), out[k] = 0;
    return true;
}

// ---- sending
static void send_to(ENetPeer *to, const void *d, uint32_t n) {
    ENetPacket *p = enet_packet_create(d, n, ENET_PACKET_FLAG_RELIABLE);
    if (p && enet_peer_send(to, 0, p) != 0) enet_packet_destroy(p);
}
static void send_id(ENetPeer *to, uint8_t id) { send_to(to, &id, 1); }

static void broadcast_info(void) {
    uint8_t buf[8192];
    Wr w = {buf, 0, sizeof buf, false};
    wr_u8(&w, M_ROOM_INFO), wr_str(&w, S.name), wr_str(&w, ""), wr_u32(&w, MAX_MEMBERS), wr_u16(&w, S.port);
    wr_str(&w, ""), wr_str(&w, S.host_nick);  // (preferred game: none; the host's name)
    wr_u32(&w, (uint32_t)S.n);
    for (int i = 0; i < S.n; i++) {
        const Member *m = &S.m[i];
        wr_str(&w, m->nick), wr_bytes(&w, m->ip, 4);
        wr_str(&w, m->game), wr_u64(&w, m->game_id), wr_str(&w, m->version);
        wr_str(&w, ""), wr_str(&w, ""), wr_str(&w, "");  // (no account name, display name or avatar)
    }
    if (w.bad) return;
    for (int i = 0; i < S.n; i++) send_to(S.m[i].peer, buf, w.n);
}

static Member *by_peer(const ENetPeer *p) {
    for (int i = 0; i < S.n; i++) if (S.m[i].peer == p) return &S.m[i];
    return NULL;
}
static Member *by_ip(const uint8_t ip[4]) {
    for (int i = 0; i < S.n; i++) if (!memcmp(S.m[i].ip, ip, 4)) return &S.m[i];
    return NULL;
}
static Member *by_nick(const char *nick) {
    for (int i = 0; i < S.n; i++) if (!strcmp(S.m[i].nick, nick)) return &S.m[i];
    return NULL;
}

// The next address nobody has: 10.255.1.1, 10.255.1.2, ... (inside 10.x: an emulator that sees a /8 mask on its own adapter
// still finds the broadcast of that mask in 10.x, which stays in the room).
static void free_ip(uint8_t ip[4]) {
    for (int i = 1; i < 255; i++)
        for (int j = 1; j < 255; j++) {
            uint8_t c[4] = {10, 255, (uint8_t)i, (uint8_t)j};
            if (!by_ip(c)) { memcpy(ip, c, 4); return; }
        }
    memcpy(ip, (uint8_t[4]){10, 255, 0, 0}, 4);
}

// ---- what a member sends
static void on_join(ENetPeer *peer, const uint8_t *d, uint32_t n) {
    if (by_peer(peer)) return;  // (already in)
    if (S.n >= MAX_MEMBERS) { send_id(peer, M_ROOM_IS_FULL); return; }
    Rd r = {d, n, 1, false};
    Member m;
    memset(&m, 0, sizeof m);
    char pass[64], token[64];
    rd_str(&r, m.nick, sizeof m.nick);
    const uint8_t *want = rd_bytes(&r, 4);
    uint32_t version = rd_u32(&r);
    rd_str(&r, pass, sizeof pass), rd_str(&r, token, sizeof token);
    if (r.bad) return;
    if (strcmp(pass, S.password)) { send_id(peer, M_WRONG_PASSWORD); return; }
    if (!m.nick[0] || by_nick(m.nick)) { send_id(peer, M_NAME_COLLISION); return; }
    if (memcmp(want, "\xFF\xFF\xFF\xFF", 4)) {
        if (by_ip(want)) { send_id(peer, M_IP_COLLISION); return; }
        memcpy(m.ip, want, 4);
    } else {
        free_ip(m.ip);
    }
    if (version != ROOM_VERSION) {
        uint8_t b[5] = {M_VERSION_MISMATCH, 0, 0, 0, ROOM_VERSION};
        send_to(peer, b, sizeof b);
        return;
    }
    m.peer = peer;
    S.m[S.n++] = m;
    uint8_t ok[5] = {M_JOIN_SUCCESS, m.ip[0], m.ip[1], m.ip[2], m.ip[3]};
    send_to(peer, ok, sizeof ok);
    broadcast_info();
}

static void on_game_info(ENetPeer *peer, const uint8_t *d, uint32_t n) {
    Member *m = by_peer(peer);
    if (!m) return;
    Rd r = {d, n, 1, false};
    char game[TEXT_MAX], version[32];
    rd_str(&r, game, sizeof game);
    uint64_t id = rd_u64(&r);
    rd_str(&r, version, sizeof version);
    if (r.bad) return;
    memcpy(m->game, game, sizeof m->game), memcpy(m->version, version, sizeof m->version), m->game_id = id;
    broadcast_info();
}

// A member's packet (LDN or game): the message goes on as it came. Its destination address sits at `off` in it, the
// broadcast flag at `bc_off`. Broadcast: every member but the sender. Otherwise the member with that address; for an address
// nobody has, the host member (when the sender is not that member).
static void relay(ENetPeer *from, const uint8_t *d, uint32_t n, uint32_t off, uint32_t bc_off) {
    if (!by_peer(from) || n < bc_off + 1) return;
    if (d[bc_off]) {
        for (int i = 0; i < S.n; i++) if (S.m[i].peer != from) send_to(S.m[i].peer, d, n);
        return;
    }
    Member *to = by_ip(d + off);
    if (!to && S.host_nick[0]) {
        Member *h = by_nick(S.host_nick);
        if (h && h->peer != from) to = h;
    }
    if (to) send_to(to->peer, d, n);
}

static void on_message(ENetPeer *peer, const uint8_t *d, uint32_t n) {
    if (!n) return;
    switch (d[0]) {
    case M_JOIN_REQUEST: on_join(peer, d, n); break;
    case M_SET_GAME_INFO: on_game_info(peer, d, n); break;
    case M_PROXY_PACKET:  // id, [domain, ip(4), port(2)] from, [domain, ip(4), port(2)] to, protocol, broadcast, data
        relay(peer, d, n, 1 + 1 + 4 + 2 + 1, 1 + 7 + 7 + 1);
        break;
    case M_LDN_PACKET:  // id, type, local ip(4), remote ip(4), broadcast, data
        relay(peer, d, n, 1 + 1 + 4, 1 + 1 + 4 + 4);
        break;
    default: break;  // (chat and moderation are not offered)
    }
}

static void on_leave(ENetPeer *peer) {
    for (int i = 0; i < S.n; i++)
        if (S.m[i].peer == peer) {
            memmove(&S.m[i], &S.m[i + 1], (size_t)(S.n - i - 1) * sizeof S.m[0]);
            S.n--;
            broadcast_info();
            return;
        }
}

static void *loop(void *arg) {
    (void)arg;
    while (S.run) {
        ENetEvent ev;
        while (enet_host_service(S.host, &ev, 5) > 0) {
            if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                if (ev.packet->dataLength <= MAX_MESSAGE) on_message(ev.peer, ev.packet->data, (uint32_t)ev.packet->dataLength);
                enet_packet_destroy(ev.packet);
            } else if (ev.type == ENET_EVENT_TYPE_DISCONNECT) {
                on_leave(ev.peer);
            }
        }
    }
    for (int i = 0; i < S.n; i++) send_id(S.m[i].peer, M_CLOSE_ROOM);
    enet_host_flush(S.host);
    for (int i = 0; i < S.n; i++) enet_peer_disconnect_now(S.m[i].peer, 0);
    S.n = 0;
    return NULL;
}

bool roomsrv_start(int port, const char *name, const char *password, const char *host_nick, char *err, int err_size) {
    if (S.up) return true;
    static bool inited;
    if (!inited) {
        if (enet_initialize() != 0) {
            if (err && err_size > 0) snprintf(err, (size_t)err_size, "ENet did not start");
            return false;
        }
        inited = true;
    }
    memset(&S.m, 0, sizeof S.m);
    S.n = 0;
    snprintf(S.name, sizeof S.name, "%s", name ? name : "MHGU");
    snprintf(S.password, sizeof S.password, "%s", password ? password : "");
    snprintf(S.host_nick, sizeof S.host_nick, "%s", host_nick ? host_nick : "");
    S.port = (uint16_t)port;
    ENetAddress addr = {ENET_HOST_ANY, (enet_uint16)port};
    S.host = enet_host_create(&addr, MAX_MEMBERS + 1, 1, 0, 0);  // (one more than the room takes, so a full room can say so)
    if (!S.host) {
        if (err && err_size > 0) snprintf(err, (size_t)err_size, "the port may be in use");
        return false;
    }
    S.run = true;
    if (pthread_create(&S.th, NULL, loop, NULL) != 0) {
        enet_host_destroy(S.host);
        S.host = NULL, S.run = false;
        if (err && err_size > 0) snprintf(err, (size_t)err_size, "could not start the room thread");
        return false;
    }
    S.up = true;
    return true;
}

void roomsrv_stop(void) {
    if (!S.up) return;
    S.run = false;
    pthread_join(S.th, NULL);
    enet_host_destroy(S.host);
    S.host = NULL, S.up = false;
}

bool roomsrv_running(void) { return S.up; }
