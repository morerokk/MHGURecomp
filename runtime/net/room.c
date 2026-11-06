// A suyu-family multiplayer room, as a member (room.h). Wire format (suyu's src/network/room.cpp and room_member.cpp,
// Citra's room protocol): ENet, one channel, every message reliable; the first byte is the message id and numbers are
// big-endian, a string is a u32 length and its bytes, a vector the same. Written from the protocol.
#include <enet/enet.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#include "../config.h"
#include "../hle.h"
#include "room.h"

enum {
    M_JOIN_REQUEST = 1, M_JOIN_SUCCESS, M_ROOM_INFO, M_SET_GAME_INFO, M_PROXY_PACKET, M_LDN_PACKET, M_CHAT, M_NAME_COLLISION,
    M_IP_COLLISION, M_VERSION_MISMATCH, M_WRONG_PASSWORD, M_CLOSE_ROOM, M_ROOM_IS_FULL, M_STATUS, M_HOST_KICKED,
    M_HOST_BANNED, M_JOIN_SUCCESS_AS_MOD = 24,
};
#define NETWORK_VERSION 1u  // the room protocol's version (a mismatch is refused by the room)
#define DEFAULT_PORT 24872
#define CONNECT_MS 5000
#define MAX_MESSAGE 0x20000


// Room builds zstd-compress every proxy payload. We compress what we send, and open what arrives; a sender whose payloads
// came uncompressed (an older or other build) is answered the same way.
#define RAW_MAX 32
static struct { uint8_t ip[4]; bool raw; bool used; } raw_peers[RAW_MAX];
static pthread_mutex_t raw_mu = PTHREAD_MUTEX_INITIALIZER;
static void raw_note(const uint8_t ip[4], bool raw) {
    pthread_mutex_lock(&raw_mu);
    int free_i = -1;
    for (int i = 0; i < RAW_MAX; i++) {
        if (raw_peers[i].used && !memcmp(raw_peers[i].ip, ip, 4)) { raw_peers[i].raw = raw; pthread_mutex_unlock(&raw_mu); return; }
        if (!raw_peers[i].used && free_i < 0) free_i = i;
    }
    if (free_i < 0) free_i = (int)(ip[3] % RAW_MAX);
    memcpy(raw_peers[free_i].ip, ip, 4), raw_peers[free_i].raw = raw, raw_peers[free_i].used = true;
    pthread_mutex_unlock(&raw_mu);
}
static bool raw_is(const uint8_t ip[4]) {
    bool r = false;
    pthread_mutex_lock(&raw_mu);
    for (int i = 0; i < RAW_MAX; i++) if (raw_peers[i].used && !memcmp(raw_peers[i].ip, ip, 4)) r = raw_peers[i].raw;
    pthread_mutex_unlock(&raw_mu);
    return r;
}
static bool is_zstd(const uint8_t *d, uint32_t n) { return n >= 4 && d[0] == 0x28 && d[1] == 0xB5 && d[2] == 0x2F && d[3] == 0xFD; }

typedef struct Out { struct Out *next; uint32_t len; uint8_t data[]; } Out;

static struct {
    pthread_mutex_t mu;       // the state and the send queue
    ENetHost *host;
    ENetPeer *peer;
    pthread_t th;
    bool has_th;
    volatile bool run;
    volatile int state;       // 0 idle, 1 joining, 2 joined
    volatile int error;       // a refusal's message id
    uint8_t ip[4];
    Out *head, *tail;
} R = {.mu = PTHREAD_MUTEX_INITIALIZER};
static RoomHooks g_hooks;
static volatile bool g_leaving;  // room_leave() is ending the thread: not a lost connection

void room_set_hooks(const RoomHooks *h) { g_hooks = *h; }
static char s_self[48];
#define SELF_NICK "MHGU Host"
void room_set_self(const char *addr) { snprintf(s_self, sizeof s_self, "%s", addr ? addr : ""); }
const char *room_server(void) { return s_self[0] ? s_self : g_config.ldn_room_server; }
const char *room_nick(void) { return s_self[0] ? SELF_NICK : g_config.ldn_room_nick[0] ? g_config.ldn_room_nick : "MHGU Player"; }
bool room_hosting(void) { return s_self[0] || g_config.ldn_room_host; }
bool room_enabled(void) { return room_server()[0] != 0; }
bool room_connected(void) { return R.state == 2; }
void room_own_ip(uint8_t ip[4]) { memcpy(ip, R.ip, 4); }

// ---- packet building and reading
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
static void wr_vec(Wr *w, const void *d, uint32_t n) { wr_u32(w, n); wr_bytes(w, d, n); }

typedef struct { const uint8_t *p; uint32_t n, pos; bool bad; } Rd;
static const uint8_t *rd_bytes(Rd *r, uint32_t n) {
    if (r->bad || r->n - r->pos < n) { r->bad = true; return NULL; }
    const uint8_t *q = r->p + r->pos;
    r->pos += n;
    return q;
}
static uint8_t rd_u8(Rd *r) { const uint8_t *q = rd_bytes(r, 1); return q ? q[0] : 0; }
static uint16_t rd_u16(Rd *r) { const uint8_t *q = rd_bytes(r, 2); return q ? (uint16_t)(q[0] << 8 | q[1]) : 0; }
static uint32_t rd_u32(Rd *r) {
    const uint8_t *q = rd_bytes(r, 4);
    return q ? (uint32_t)q[0] << 24 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 8 | q[3] : 0;
}
static void rd_u64_skip(Rd *r) { rd_bytes(r, 8); }
static void rd_skip_str(Rd *r) { uint32_t n = rd_u32(r); rd_bytes(r, n); }

// Queues a message for the room's thread (ENet is used from one thread only).
static void queue(const uint8_t *d, uint32_t n) {
    Out *o = malloc(sizeof *o + n);
    if (!o) return;
    o->next = NULL, o->len = n;
    memcpy(o->data, d, n);
    pthread_mutex_lock(&R.mu);
    if (R.state == 0 && !R.run) { pthread_mutex_unlock(&R.mu); free(o); return; }
    if (R.tail) R.tail->next = o; else R.head = o;
    R.tail = o;
    pthread_mutex_unlock(&R.mu);
}

void room_send_ldn(uint8_t type, const uint8_t local_ip[4], const uint8_t remote_ip[4], bool broadcast, const void *data,
                   uint32_t len) {
    if (R.state != 2 || len > MAX_MESSAGE) return;
    uint8_t *buf = malloc(len + 32);
    if (!buf) return;
    Wr w = {buf, 0, len + 32, false};
    wr_u8(&w, M_LDN_PACKET), wr_u8(&w, type), wr_bytes(&w, local_ip, 4), wr_bytes(&w, remote_ip, 4);
    wr_u8(&w, broadcast), wr_vec(&w, data, len);
    if (!w.bad) queue(buf, w.n);
    free(buf);
}

void room_send_proxy(const uint8_t src_ip[4], uint16_t src_port, const uint8_t dst_ip[4], uint16_t dst_port,
                     uint8_t proto, bool broadcast, const void *data, uint32_t len) {
    if (R.state != 2 || len > MAX_MESSAGE) return;
    uint8_t *z = NULL;
    if (len && (broadcast || !raw_is(dst_ip))) {
        size_t cap = ZSTD_compressBound(len);
        z = malloc(cap);
        if (!z) return;
        size_t zn = ZSTD_compress(z, cap, data, len, ZSTD_CLEVEL_DEFAULT);
        if (ZSTD_isError(zn)) { free(z); return; }
        data = z, len = (uint32_t)zn;
        if (len > MAX_MESSAGE) { free(z); return; }
    }
    uint8_t *buf = malloc(len + 48);
    if (!buf) { free(z); return; }
    Wr w = {buf, 0, len + 48, false};
    wr_u8(&w, M_PROXY_PACKET);
    wr_u8(&w, 1), wr_bytes(&w, src_ip, 4), wr_u16(&w, src_port);  // Domain::INET = 1
    wr_u8(&w, 1), wr_bytes(&w, dst_ip, 4), wr_u16(&w, dst_port);
    wr_u8(&w, proto), wr_u8(&w, broadcast), wr_vec(&w, data, len);
    if (!w.bad) queue(buf, w.n);
    free(buf);
    free(z);
}

// ---- receiving
static void got(const uint8_t *d, uint32_t n) {
    Rd r = {d, n, 0, false};
    switch (rd_u8(&r)) {
    case M_JOIN_SUCCESS:
    case M_JOIN_SUCCESS_AS_MOD: {
        const uint8_t *ip = rd_bytes(&r, 4);
        if (ip) { memcpy(R.ip, ip, 4); R.state = 2; }
        break;
    }
    case M_ROOM_INFO: {  // name, description, slots, port, preferred game name, host, then the members
        rd_skip_str(&r), rd_skip_str(&r), rd_u32(&r), rd_u16(&r), rd_skip_str(&r), rd_skip_str(&r);
        uint32_t n = rd_u32(&r);
        if (r.bad || n > 256 || !g_hooks.members) break;
        uint8_t (*ips)[4] = malloc(n * 4 + 4);
        if (!ips) break;
        uint32_t k = 0;
        for (; k < n && !r.bad; k++) {
            rd_skip_str(&r);  // nickname
            const uint8_t *ip = rd_bytes(&r, 4);
            rd_skip_str(&r), rd_u64_skip(&r), rd_skip_str(&r), rd_skip_str(&r), rd_skip_str(&r), rd_skip_str(&r);  // game, user
            if (ip && !r.bad) memcpy(ips[k], ip, 4);
        }
        if (!r.bad) g_hooks.members((const uint8_t (*)[4])ips, (int)n);
        free(ips);
        break;
    }
    case M_LDN_PACKET: {
        uint8_t type = rd_u8(&r);
        const uint8_t *lip = rd_bytes(&r, 4), *rip = rd_bytes(&r, 4);
        bool bc = rd_u8(&r) != 0;
        uint32_t len = rd_u32(&r);
        const uint8_t *data = rd_bytes(&r, len);
        if (lip && rip && (data || !len) && g_hooks.ldn) g_hooks.ldn(type, lip, rip, bc, data, len);
        break;
    }
    case M_PROXY_PACKET: {
        rd_u8(&r);
        const uint8_t *sip = rd_bytes(&r, 4);
        uint16_t sport = rd_u16(&r);
        rd_u8(&r);
        const uint8_t *dip = rd_bytes(&r, 4);
        uint16_t dport = rd_u16(&r);
        uint8_t proto = rd_u8(&r);
        bool bc = rd_u8(&r) != 0;
        uint32_t len = rd_u32(&r);
        const uint8_t *data = rd_bytes(&r, len);
        if (sip && dip && (data || !len) && g_hooks.proxy) {
            if (len && is_zstd(data, len)) {
                unsigned long long n = ZSTD_getFrameContentSize(data, len);
                if (n == ZSTD_CONTENTSIZE_ERROR || n == ZSTD_CONTENTSIZE_UNKNOWN || n > MAX_MESSAGE) break;
                uint8_t *o = malloc(n ? (size_t)n : 1);
                if (!o) break;
                size_t got_n = ZSTD_decompress(o, (size_t)n, data, len);
                if (!ZSTD_isError(got_n)) {
                    raw_note(sip, false);
                    g_hooks.proxy(sip, sport, dip, dport, proto, bc, o, (uint32_t)got_n);
                }
                free(o);
            } else {
                raw_note(sip, true);
                g_hooks.proxy(sip, sport, dip, dport, proto, bc, data, len);
            }
        }
        break;
    }
    case M_NAME_COLLISION: case M_IP_COLLISION: case M_VERSION_MISMATCH: case M_WRONG_PASSWORD: case M_ROOM_IS_FULL:
    case M_HOST_KICKED: case M_HOST_BANNED:
        R.error = d[0];
        R.state = 0;
        R.run = false;
        break;
    case M_CLOSE_ROOM:
        R.error = M_CLOSE_ROOM;
        R.state = 0;
        R.run = false;
        break;
    default:
        break;  // room information, chat, status messages
    }
}

static void *loop(void *arg) {
    (void)arg;
    bool was_joined = false;
    while (R.run) {
        ENetEvent ev;
        if (enet_host_service(R.host, &ev, 5) > 0) {
            if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                if (ev.packet->dataLength > 0) got(ev.packet->data, (uint32_t)ev.packet->dataLength);
                enet_packet_destroy(ev.packet);
            } else if (ev.type == ENET_EVENT_TYPE_DISCONNECT) {
                if (R.state == 2) R.error = M_CLOSE_ROOM;
                R.state = 0, R.run = false;
            }
        }
        if (R.state == 2) was_joined = true;
        pthread_mutex_lock(&R.mu);
        Out *o = R.head;
        R.head = R.tail = NULL;
        pthread_mutex_unlock(&R.mu);
        bool sent = false;
        while (o) {
            Out *next = o->next;
            ENetPacket *p = enet_packet_create(o->data, o->len, ENET_PACKET_FLAG_RELIABLE);
            if (p && R.peer && enet_peer_send(R.peer, 0, p) == 0) sent = true;
            else if (p) enet_packet_destroy(p);
            free(o);
            o = next;
        }
        if (sent) enet_host_flush(R.host);
        if (R.state == 0 && R.run) R.run = false;
    }
    R.run = false;
    if (was_joined && !g_leaving && g_hooks.lost) g_hooks.lost();
    return NULL;
}

bool room_join(const char *server, const char *nick, const char *password, uint8_t fake_ip[4], char *err, int err_size) {
    static bool inited;
    char e[96] = "";
#define FAIL(...) do { snprintf(e, sizeof e, __VA_ARGS__); goto fail; } while (0)
    room_leave();
    if (!inited) {
        if (enet_initialize() != 0) FAIL("ENet did not start");
        inited = true;
    }
    char host[128];
    int port = DEFAULT_PORT;
    snprintf(host, sizeof host, "%s", server);
    char *colon = strrchr(host, ':');
    if (colon && strchr(host, ':') == colon) { port = atoi(colon + 1); *colon = 0; }
    if (!*host || port <= 0 || port > 65535) FAIL("bad room address");
    R.host = enet_host_create(NULL, 1, 1, 0, 0);
    if (!R.host) FAIL("could not create the client");
    ENetAddress addr = {0};
    if (enet_address_set_host(&addr, host) != 0) FAIL("room address not found");
    addr.port = (enet_uint16)port;
    R.peer = enet_host_connect(R.host, &addr, 1, 0);
    if (!R.peer) FAIL("could not connect");
    ENetEvent ev;
    if (enet_host_service(R.host, &ev, CONNECT_MS) <= 0 || ev.type != ENET_EVENT_TYPE_CONNECT) FAIL("no answer from the room");
    R.state = 1, R.error = 0, R.run = true;
    memset(R.ip, 0, 4);
    // the join request and the game info go out first, from here (the thread's queue is for the others)
    uint8_t buf[512];
    Wr w = {buf, 0, sizeof buf, false};
    wr_u8(&w, M_JOIN_REQUEST), wr_str(&w, nick), wr_bytes(&w, "\xFF\xFF\xFF\xFF", 4), wr_u32(&w, NETWORK_VERSION);
    wr_str(&w, password), wr_str(&w, "");
    if (w.bad) FAIL("nickname or password too long");
    enet_peer_send(R.peer, 0, enet_packet_create(buf, w.n, ENET_PACKET_FLAG_RELIABLE));
    w.n = 0;
    wr_u8(&w, M_SET_GAME_INFO), wr_str(&w, "Monster Hunter Generations Ultimate"), wr_u64(&w, 0x0100770008DD8000ull), wr_str(&w, "1.4.0");
    enet_peer_send(R.peer, 0, enet_packet_create(buf, w.n, ENET_PACKET_FLAG_RELIABLE));
    enet_host_flush(R.host);
    R.has_th = pthread_create(&R.th, NULL, loop, NULL) == 0;
    if (!R.has_th) FAIL("could not start the room thread");
    for (int i = 0; i < 100 && R.state == 1 && R.run; i++) plat_sleep_until_ns(plat_now_ns() + 50000000ull);
    if (R.state != 2) {
        switch (R.error) {
        case M_NAME_COLLISION: FAIL("the name is taken in the room"); break;
        case M_IP_COLLISION: FAIL("address collision in the room"); break;
        case M_VERSION_MISMATCH: FAIL("the room runs another protocol version"); break;
        case M_WRONG_PASSWORD: FAIL("wrong room password"); break;
        case M_ROOM_IS_FULL: FAIL("the room is full"); break;
        case M_HOST_BANNED: FAIL("banned from the room"); break;
        default: FAIL("the room did not answer the join");
        }
    }
    memcpy(fake_ip, R.ip, 4);
    LOG("[room] joined %s as %u.%u.%u.%u\n", server, R.ip[0], R.ip[1], R.ip[2], R.ip[3]);
    return true;
fail:
    if (err && err_size) snprintf(err, (size_t)err_size, "%s", e);
    LOG("[room] can't join %s: %s\n", server, e);
    room_leave();
    return false;
#undef FAIL
}

void room_leave(void) {
    g_leaving = true;
    R.run = false;
    if (R.has_th) { pthread_join(R.th, NULL); R.has_th = false; }
    g_leaving = false;
    if (R.peer && R.host) {
        enet_peer_disconnect(R.peer, 0);
        ENetEvent ev;
        for (int i = 0; i < 20 && enet_host_service(R.host, &ev, 20) > 0;)
            if (ev.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(ev.packet);
            else if (ev.type == ENET_EVENT_TYPE_DISCONNECT) break;
            else i++;
    }
    if (R.host) enet_host_destroy(R.host);
    R.host = NULL, R.peer = NULL, R.state = 0;
    pthread_mutex_lock(&R.mu);
    for (Out *o = R.head; o;) { Out *n = o->next; free(o); o = n; }
    R.head = R.tail = NULL;
    pthread_mutex_unlock(&R.mu);
}
