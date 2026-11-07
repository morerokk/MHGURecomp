// Mixed hubs (xbridge.h): room players join a hub this game hosts on a Ryujinx LDN server.
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "../config.h"
#include "../hle.h"
#include "../platform.h"
#include "room.h"
#include "ryuldn.h"
#include "xbridge.h"

#define NETINFO_SIZE 0x480
#define NODE_SIZE 0x40
#define MAX_NODES 8
#define MAXP 7
enum {
    NI_BSSID = 0x20, NI_SSID = 0x26, NI_SEC_MODE = 0x60, NI_NODE_MAX = 0x66, NI_NODE_COUNT = 0x67, NI_NODES = 0x68,
};
enum { NODE_IP = 0x00, NODE_MAC = 0x04, NODE_ID = 0x0A, NODE_CONNECTED = 0x0B, NODE_NAME = 0x0C, NODE_LCV = 0x2E };
#define NODE(info, i) ((uint8_t *)(info) + NI_NODES + (i) * NODE_SIZE)

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t ip_u32(const uint8_t b[4]) { return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]; }
static void ip_bytes(uint32_t ip, uint8_t b[4]) { b[0] = (uint8_t)(ip >> 24), b[1] = (uint8_t)(ip >> 16), b[2] = (uint8_t)(ip >> 8), b[3] = (uint8_t)ip; }
static uint32_t alias_ip(int slot) { return 0xC0A8FA00u | (uint32_t)slot; }  // 192.168.250.<slot>

typedef struct {
    bool used, joined, dead;
    uint8_t room_ip[4];
    uint8_t mac[6];       // the MAC the player announced
    uint8_t node[NODE_SIZE];
    uint32_t ryu_ip;      // its address in the hub on the server
    RyuPeer *rp;
    int busy;             // sends through rp in progress outside mu: rp is closed only once they are done
} Peer;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t idle = PTHREAD_COND_INITIALIZER;  // a peer's busy went back to 0
static bool active;
static uint8_t lobby[NETINFO_SIZE];  // the hub as the server sees it
static uint32_t host_ip, host_mask;
static Peer peers[MAXP];

// ---- address translation
typedef struct { uint8_t from[6], to[6]; int len; } Sub;
#define MAXSUB 64
static int add_ip(Sub *s, int n, uint32_t from, uint32_t to) {  // as the game may write an address: network order or reversed
    if (!from || !to || from == to || n + 2 > MAXSUB) return n;
    for (int i = 0; i < 4; i++) s[n].from[i] = (uint8_t)(from >> (24 - 8 * i)), s[n].to[i] = (uint8_t)(to >> (24 - 8 * i));
    s[n].len = 4;
    n++;
    for (int i = 0; i < 4; i++) s[n].from[i] = (uint8_t)(from >> (8 * i)), s[n].to[i] = (uint8_t)(to >> (8 * i));
    s[n].len = 4;
    return n + 1;
}
static int add_mac(Sub *s, int n, const uint8_t *from, const uint8_t *to) {
    static const uint8_t zero[6];
    if (n >= MAXSUB || !memcmp(from, to, 6) || !memcmp(from, zero, 6) || !memcmp(to, zero, 6)) return n;
    memcpy(s[n].from, from, 6), memcpy(s[n].to, to, 6), s[n].len = 6;
    return n + 1;
}
static void host_mac(uint8_t mac[6]) { uint8_t ip[4]; room_own_ip(ip); mac[0] = 0x02, mac[1] = 0, memcpy(mac + 2, ip, 4); }

// Caller holds mu. room view -> hub view when to_hub, else back.
static int build_subs(Sub *s) {
    uint8_t own[4], hm[6];
    room_own_ip(own);
    host_mac(hm);
    int n = 0;
    n = add_ip(s, n, ip_u32(own), host_ip);
    n = add_mac(s, n, hm, NODE(lobby, 0) + NODE_MAC);
    for (int i = 0; i < MAXP; i++) {
        Peer *p = &peers[i];
        if (!p->used || !p->joined) continue;
        n = add_ip(s, n, ip_u32(p->room_ip), p->ryu_ip);
        for (int k = 1; k < MAX_NODES; k++)
            if (NODE(lobby, k)[NODE_CONNECTED] && rd32(NODE(lobby, k) + NODE_IP) == p->ryu_ip)
                n = add_mac(s, n, p->mac, NODE(lobby, k) + NODE_MAC);
    }
    for (int k = 1; k < MAX_NODES; k++)
        if (NODE(lobby, k)[NODE_CONNECTED]) n = add_ip(s, n, alias_ip(k), rd32(NODE(lobby, k) + NODE_IP));
    return n;
}

static void translate(uint8_t *d, uint32_t len, const Sub *s, int n, bool to_hub) {
    for (int j = 0; j < n; j++) {
        const uint8_t *from = to_hub ? s[j].from : s[j].to, *to = to_hub ? s[j].to : s[j].from;
        for (uint32_t i = 0; i + (uint32_t)s[j].len <= len; i++)
            if (!memcmp(d + i, from, (size_t)s[j].len)) memcpy(d + i, to, (size_t)s[j].len), i += (uint32_t)s[j].len - 1;
    }
}

// The hub as a room player sees it: the host at our room address, the room players as themselves, the rest at stand-ins.
// Caller holds mu.
static void room_view(uint8_t *out) {
    memcpy(out, lobby, NETINFO_SIZE);
    uint8_t own[4], hm[6];
    room_own_ip(own);
    host_mac(hm);
    memcpy(out + NI_BSSID, hm, 6);
    memcpy(out + NI_SSID, "\x12YuzuFakeSsidForLdn", 19);  // (as the emulators' own rooms name a hub)
    for (int i = 0; i < MAX_NODES; i++) {
        uint8_t *nd = NODE(out, i);
        if (!nd[NODE_CONNECTED]) continue;
        if (i == 0) {
            wr32(nd + NODE_IP, ip_u32(own));
            memcpy(nd + NODE_MAC, hm, 6);
            continue;
        }
        uint32_t mapped = alias_ip(i);
        uint32_t ryu = rd32(NODE(lobby, i) + NODE_IP);
        for (int k = 0; k < MAXP; k++)
            if (peers[k].used && peers[k].joined && peers[k].ryu_ip == ryu) {
                mapped = ip_u32(peers[k].room_ip);
                memcpy(nd + NODE_MAC, peers[k].mac, 6);  // a player finds its own node by the MAC it announced
            }
        wr32(nd + NODE_IP, mapped);
    }
}

static void send_ldn(uint8_t type, const uint8_t to[4], const void *d, uint32_t n) {
    uint8_t own[4];
    room_own_ip(own);
    room_send_ldn(type, own, to, false, d, n);
}

static void sync_all(void) {
    uint8_t view[NETINFO_SIZE], to[MAXP][4];
    int n = 0;
    pthread_mutex_lock(&mu);
    if (!active) { pthread_mutex_unlock(&mu); return; }
    room_view(view);
    for (int i = 0; i < MAXP; i++) if (peers[i].used && peers[i].joined) memcpy(to[n++], peers[i].room_ip, 4);
    pthread_mutex_unlock(&mu);
    for (int i = 0; i < n; i++) send_ldn(RLDN_SYNC_NETWORK, to[i], view, NETINFO_SIZE);
}

// ---- peers
static Peer *peer_by_room(const uint8_t ip[4]) {
    for (int i = 0; i < MAXP; i++) if (peers[i].used && !memcmp(peers[i].room_ip, ip, 4)) return &peers[i];
    return NULL;
}
static Peer *peer_by_ryu(uint32_t ip) {
    for (int i = 0; i < MAXP; i++) if (peers[i].used && peers[i].joined && peers[i].ryu_ip == ip) return &peers[i];
    return NULL;
}

static void peer_lobby(void *ud, const uint8_t *info);
static void peer_data(void *ud, const RyuProxy *r, const void *data, uint32_t len);

typedef struct { Peer *p; RyuPeer *rp; } Reap;
static void *reaper(void *arg) {
    Reap *r = arg;
    pthread_mutex_lock(&mu);
    while (r->p->busy) pthread_cond_wait(&idle, &mu);  // (a room packet on its way through rp)
    pthread_mutex_unlock(&mu);
    ryu_peer_close(r->rp);
    pthread_mutex_lock(&mu);
    r->p->used = false;
    pthread_mutex_unlock(&mu);
    free(r);
    return NULL;
}
// Closes a peer (not from its own reader thread: that one is told in peer_lobby and gets a thread here).
static void retire(Peer *p, bool tell) {
    pthread_mutex_lock(&mu);
    if (!p->used || p->dead) { pthread_mutex_unlock(&mu); return; }
    p->dead = true;
    uint8_t to[4];
    memcpy(to, p->room_ip, 4);
    RyuPeer *rp = p->rp;
    p->rp = NULL, p->joined = false;
    pthread_mutex_unlock(&mu);
    if (tell) send_ldn(RLDN_DESTROY_NETWORK, to, NULL, 0);
    Reap *r = malloc(sizeof *r);
    if (!r) return;
    r->p = p, r->rp = rp;
    pthread_t t;
    if (pthread_create(&t, NULL, reaper, r)) { reaper(r); return; }
    pthread_detach(t);
}

typedef struct { Peer *p; uint8_t netinfo[NETINFO_SIZE]; } Join;
static void *join_worker(void *arg) {
    Join *j = arg;
    Peer *p = j->p;
    uint8_t sec[0x44] = {0}, user[0x30] = {0}, out[NETINFO_SIZE];
    memcpy(sec, j->netinfo + NI_SEC_MODE, 2);
    memcpy(user, p->node + NODE_NAME, 0x20);
    uint32_t ip = 0, mask = 0, res = 0;
    uint16_t lcv = rd16(p->node + NODE_LCV);
    if (!lcv) lcv = rd16(NODE(j->netinfo, 0) + NODE_LCV);  // (older suyu builds send 0: the host's version, as their own hubs take them)
    RyuPeer *rp = ryu_peer_join(j->netinfo, sec, user, lcv, &(RyuPeerHooks){peer_lobby, peer_data}, p, out, &ip, &mask, &res);
    pthread_mutex_lock(&mu);
    bool keep = rp && active && !p->dead;
    if (keep) {
        p->rp = rp, p->ryu_ip = ip, p->joined = true;
        memcpy(lobby, out, NETINFO_SIZE);  // (the join's answer is the newest view)
    }
    uint8_t to[4];
    memcpy(to, p->room_ip, 4);
    pthread_mutex_unlock(&mu);
    if (keep) {
        LOG("[xbridge] room player %.32s joined the hub on the server as %u.%u.%u.%u\n", (const char *)p->node + NODE_NAME,
            ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
        sync_all();
    } else {
        LOG("[xbridge] a room player could not be joined to the hub (result %#x)\n", res);
        if (rp) { ryu_peer_close(rp); }
        pthread_mutex_lock(&mu);
        p->used = false;
        pthread_mutex_unlock(&mu);
        send_ldn(RLDN_DESTROY_NETWORK, to, NULL, 0);
    }
    free(j);
    return NULL;
}

// ---- the hub's side
void xb_begin(const uint8_t *info, uint32_t hip, uint32_t mask) {
    pthread_mutex_lock(&mu);
    memcpy(lobby, info, NETINFO_SIZE);
    host_ip = hip, host_mask = mask;
    memset(peers, 0, sizeof peers);
    active = true;
    pthread_mutex_unlock(&mu);
    LOG("[xbridge] the hub is also offered to the room's players\n");
}

void xb_end(void) {
    pthread_mutex_lock(&mu);
    if (!active) { pthread_mutex_unlock(&mu); return; }
    active = false;
    pthread_mutex_unlock(&mu);
    for (int i = 0; i < MAXP; i++) {
        bool u;
        pthread_mutex_lock(&mu);
        u = peers[i].used && !peers[i].dead;
        pthread_mutex_unlock(&mu);
        if (u) retire(&peers[i], true);
    }
    LOG("[xbridge] the hub is no longer offered to the room\n");
}

bool xb_active(void) { return active; }

int xb_players(void) {
    int n = 0;
    pthread_mutex_lock(&mu);
    for (int i = 0; i < MAXP; i++) n += peers[i].used && peers[i].joined;
    pthread_mutex_unlock(&mu);
    return n;
}

void xb_lobby(const uint8_t *info) {
    if (!info) { xb_end(); return; }
    pthread_mutex_lock(&mu);
    if (active) memcpy(lobby, info, NETINFO_SIZE);
    pthread_mutex_unlock(&mu);
    sync_all();
}

// A peer's own connection: the server says it is out of the lobby (the host closed it, or it was kicked).
static void peer_lobby(void *ud, const uint8_t *info) {
    Peer *p = ud;
    (void)info;
    if (!info) retire(p, true);
}

// Game data for a room player, from the server (a peer's reader thread).
static void peer_data(void *ud, const RyuProxy *r, const void *data, uint32_t len) {
    Peer *p = ud;
    if (r->proto != 17 || len > 0x10000) return;
    uint8_t buf[0x10000], to[4], src[4], own[4];
    Sub subs[MAXSUB];
    int ns;
    uint32_t bcast;
    pthread_mutex_lock(&mu);
    if (!active || !p->used || !p->joined) { pthread_mutex_unlock(&mu); return; }
    bcast = (host_ip & host_mask) | ~host_mask;
    bool mine = r->dst_ip == p->ryu_ip || r->dst_ip == bcast || r->dst_ip == 0xFFFFFFFFu;
    if (!mine || peer_by_ryu(r->src_ip)) { pthread_mutex_unlock(&mu); return; }  // (the room carries a room player's own packets)
    room_own_ip(own);
    uint32_t sip;
    if (r->src_ip == host_ip) sip = ip_u32(own);
    else {
        sip = 0;
        for (int k = 1; k < MAX_NODES; k++)
            if (NODE(lobby, k)[NODE_CONNECTED] && rd32(NODE(lobby, k) + NODE_IP) == r->src_ip) sip = alias_ip(k);
        if (!sip) { pthread_mutex_unlock(&mu); return; }
    }
    memcpy(to, p->room_ip, 4);
    ns = build_subs(subs);
    pthread_mutex_unlock(&mu);
    memcpy(buf, data, len);
    translate(buf, len, subs, ns, false);
    ip_bytes(sip, src);
    room_send_proxy(src, r->src_port, to, r->dst_port, 4, false, buf, len);
}

// ---- the room's side
void xb_room_ldn(uint8_t type, const uint8_t lip[4], const uint8_t rip[4], bool bc, const void *data, uint32_t len) {
    (void)bc, (void)rip;
    switch (type) {
    case RLDN_SCAN: {
        LOG("[xbridge] a room player scans (%u.%u.%u.%u)\n", lip[0], lip[1], lip[2], lip[3]);
        uint8_t view[NETINFO_SIZE];
        pthread_mutex_lock(&mu);
        bool ok = active && lobby[NI_NODE_COUNT] && lobby[NI_NODE_COUNT] < lobby[NI_NODE_MAX] && lobby[0x62] != 1;  // (a closed or full hub is not offered)
        if (ok) room_view(view);
        pthread_mutex_unlock(&mu);
        if (ok) send_ldn(RLDN_SCAN_RESP, lip, view, NETINFO_SIZE);
        break;
    }
    case RLDN_CONNECT: {
        LOG("[xbridge] a room player wants to join (%u.%u.%u.%u, %u bytes)\n", lip[0], lip[1], lip[2], lip[3], len);
        if (len < NODE_SIZE) break;
        pthread_mutex_lock(&mu);
        Peer *p = peer_by_room(lip);
        if (p) {  // (asking again: the same view)
            pthread_mutex_unlock(&mu);
            sync_all();
            break;
        }
        int k = 0;
        while (k < MAXP && peers[k].used) k++;
        if (!active || k == MAXP || lobby[NI_NODE_COUNT] >= lobby[NI_NODE_MAX]) {
            pthread_mutex_unlock(&mu);
            send_ldn(RLDN_DESTROY_NETWORK, lip, NULL, 0);  // (as the emulators' own hubs refuse: the player is not left waiting)
            break;
        }
        p = &peers[k];
        memset(p, 0, sizeof *p);
        p->used = true;
        memcpy(p->room_ip, lip, 4);
        memcpy(p->node, data, NODE_SIZE);
        memcpy(p->mac, (const uint8_t *)data + NODE_MAC, 6);
        Join *j = malloc(sizeof *j);
        if (!j) { p->used = false; pthread_mutex_unlock(&mu); break; }
        j->p = p;
        memcpy(j->netinfo, lobby, NETINFO_SIZE);
        pthread_mutex_unlock(&mu);
        pthread_t t;
        if (pthread_create(&t, NULL, join_worker, j)) { pthread_mutex_lock(&mu); p->used = false; pthread_mutex_unlock(&mu); free(j); break; }
        pthread_detach(t);
        break;
    }
    case RLDN_DISCONNECT: {
        pthread_mutex_lock(&mu);
        Peer *p = peer_by_room(lip);
        pthread_mutex_unlock(&mu);
        if (p) retire(p, false);
        break;
    }
    default:
        break;
    }
}

void xb_room_members(const uint8_t (*ips)[4], int n) {
    for (int i = 0; i < MAXP; i++) {
        pthread_mutex_lock(&mu);
        Peer *p = active && peers[i].used && !peers[i].dead ? &peers[i] : NULL;
        bool here = false;
        for (int k = 0; p && k < n && !here; k++) here = memcmp(ips[k], p->room_ip, 4) == 0;
        pthread_mutex_unlock(&mu);
        if (p && !here) {
            LOG("[xbridge] room player %u.%u.%u.%u left the room: let go of its place in the hub\n", p->room_ip[0], p->room_ip[1], p->room_ip[2], p->room_ip[3]);
            retire(p, false);
        }
    }
}

void xb_room_proxy(const uint8_t sip[4], uint16_t sport, const uint8_t dip[4], uint16_t dport, uint8_t proto, bool bc, const void *data,
                   uint32_t len) {
    static int seen;
    if (seen < 40 && ENV("MHGU_LOG_NET")) {
        seen++;
        LOG("[xbridge] room packet %u.%u.%u.%u:%u -> %u.%u.%u.%u:%u proto %u%s, %u bytes\n", sip[0], sip[1], sip[2], sip[3], sport, dip[0], dip[1],
            dip[2], dip[3], dport, proto, bc ? " (broadcast)" : "", len);
    }
    if (proto != 4 || len > 0x10000) return;
    uint8_t buf[0x10000], own[4];
    Sub subs[MAXSUB];
    pthread_mutex_lock(&mu);
    Peer *p = active ? peer_by_room(sip) : NULL;
    if (!p || !p->joined || !p->rp) {
        pthread_mutex_unlock(&mu);
        if (seen < 40 && ENV("MHGU_LOG_NET")) LOG("[xbridge]   (not from a joined room player: dropped)\n");
        return;
    }
    room_own_ip(own);
    uint32_t d = ip_u32(dip), dst = 0;
    if (bc || d == 0xFFFFFFFFu || (d & 0xFF) == 0xFF) dst = (host_ip & host_mask) | ~host_mask;
    else if (d == ip_u32(own)) dst = host_ip;
    else if ((d & 0xFFFFFF00u) == 0xC0A8FA00u) {
        int k = (int)(d & 0xFF);
        if (k >= 1 && k < MAX_NODES && NODE(lobby, k)[NODE_CONNECTED]) dst = rd32(NODE(lobby, k) + NODE_IP);
    } else {
        Peer *o = peer_by_room(dip);  // another room player: the room delivers those itself
        (void)o;
    }
    if (!dst) {
        pthread_mutex_unlock(&mu);
        if (seen < 40 && ENV("MHGU_LOG_NET")) LOG("[xbridge]   (no hub address for it: dropped)\n");
        return;
    }
    RyuProxy r = {p->ryu_ip, dst, sport, dport, 17};
    RyuPeer *rp = p->rp;
    p->busy++;  // (retire() can run meanwhile: its reaper waits for this send before closing rp)
    int ns = build_subs(subs);
    pthread_mutex_unlock(&mu);
    memcpy(buf, data, len);
    translate(buf, len, subs, ns, true);
    ryu_peer_send(rp, &r, buf, len);
    pthread_mutex_lock(&mu);
    if (!--p->busy) pthread_cond_broadcast(&idle);
    pthread_mutex_unlock(&mu);
}
