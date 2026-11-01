// nn::ldn (local wireless play) over IP. The game that creates a network (the hub's host, an "access point") listens
// on a TCP port (ini [Multiplayer] Port); the others ("stations") find it with Scan - a LAN broadcast plus the
// addresses in ini [Multiplayer] Join - and Connect to it. Each station keeps that one connection: the host hands
// out node numbers and addresses (169.254.x.n, the host n = 1), sends the network's NetworkInfo whenever it changes
// (players joining or leaving, the hub's advertise data), and relays the game's packets between the players
// (net.h, vsock.c). Only the host needs a reachable port.
//
// Internet hubs (ini [Multiplayer] Server, server/ in the repository): with a hub server, a created hub is also listed
// there, for players with the same matchmaking key (ini Key, empty: public) and port version, and Scan adds the
// server's hubs to the LAN's. Joining one goes through the server, which splices the player's connection to one the
// host opens for it: the same link as a direct connection, so nobody forwards a port. Game packets go by UDP through
// the server too ("MHGR" relay prefix), else through the link.
//
// Link frames: u32 size (of what follows), u8 type, payload. Structures are the SDK's (sizes and offsets below).
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <string.h>

#include "../../updater/updater.h"  // (sha256)
#include "../config.h"
#include "../hle.h"
#include "../kernel.h"
#include "../overlay.h"
#include "../settings_locale.h"
#include "net.h"

#ifdef _WIN32
typedef SOCKET hsock;
#define BADSOCK INVALID_SOCKET
#define hclose closesocket
static void hnet_init(void) {
    static int done;
    if (!done) { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); done = 1; }
}
static void hset_nonblock(hsock s, bool on) { u_long v = on; ioctlsocket(s, FIONBIO, &v); }
static void hset_timeout(hsock s, int ms) {
    DWORD v = (DWORD)ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&v, sizeof v);
}
#else
typedef int hsock;
#define BADSOCK (-1)
#define hclose close
static void hnet_init(void) {}
static void hset_nonblock(hsock s, bool on) {
    int f = fcntl(s, F_GETFL);
    fcntl(s, F_SETFL, on ? f | O_NONBLOCK : f & ~O_NONBLOCK);
}
static void hset_timeout(hsock s, int ms) {
    struct timeval tv = {ms / 1000, (ms % 1000) * 1000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}
#endif

// ---- SDK structures (bytes)
#define NETINFO_SIZE 0x480
enum {
    NI_LOCAL_COMM_ID = 0x00, NI_SCENE_ID = 0x0A, NI_SESSION_ID = 0x10, NI_BSSID = 0x20, NI_SSID = 0x26,
    NI_CHANNEL = 0x48, NI_LINK_LEVEL = 0x4A, NI_NETWORK_TYPE = 0x4B, NI_SEC_PARAM = 0x50, NI_SEC_MODE = 0x60,
    NI_ACCEPT_POLICY = 0x62, NI_NODE_MAX = 0x66, NI_NODE_COUNT = 0x67, NI_NODES = 0x68, NI_ADV_SIZE = 0x26A,
    NI_ADV = 0x26C, NI_AUTH_ID = 0x478,
};
#define NODE_SIZE 0x40
enum { NODE_IP = 0x00, NODE_MAC = 0x04, NODE_ID = 0x0A, NODE_CONNECTED = 0x0B, NODE_NAME = 0x0C, NODE_LCV = 0x2E };
#define MAX_NODES 8
#define ADV_MAX 0x180
// NetworkConfig (0x20): IntentId 0x10, u16 channel, u8 node count max, u8 -, u16 local communication version
// SecurityConfig (0x44): u16 mode, u16 passphrase size, passphrase[0x40]; UserConfig (0x30): user name[0x21]
// SecurityParameter (0x20): data[0x10], session id[0x10]
// ScanFilter (0x60): NetworkId 0x20, u32 network type, bssid[6], ssid 0x22, -[0x10], u32 flags
enum { SF_LOCAL_COMM_ID = 1, SF_SESSION_ID = 2, SF_NETWORK_TYPE = 4, SF_BSSID = 8, SF_SSID = 0x10, SF_SCENE_ID = 0x20 };

enum { ST_NONE, ST_INITIALIZED, ST_ACCESS_POINT, ST_AP_CREATED, ST_STATION, ST_STATION_CONNECTED, ST_ERROR };
enum { DR_NONE, DR_BY_USER, DR_BY_SYSTEM, DR_DESTROYED_BY_USER, DR_DESTROYED_BY_SYSTEM, DR_REJECTED, DR_SIGNAL_LOST };
#define LDN_RESULT(d) MAKE_RESULT(203, d)
#define R_INVALID_STATE LDN_RESULT(32)
#define R_CONNECT_FAILURE LDN_RESULT(64)
#define R_CONNECT_NOT_FOUND LDN_RESULT(65)
#define R_CONNECT_TIMEOUT LDN_RESULT(66)
#define R_CONNECT_REJECTED LDN_RESULT(67)
#define R_VERSION_TOO_LOW LDN_RESULT(113)
#define R_VERSION_TOO_HIGH LDN_RESULT(114)
#define R_TOO_MANY_PLAYERS LDN_RESULT(144)

// ---- link protocol
#define PROTO_MAGIC "MHGL"
#define PROTO_VERSION 2
enum { F_HELLO = 1, F_INFO, F_WELCOME, F_REFUSE, F_PACKET, F_DESTROY, F_AUX };  // F_AUX: the port's own messages (emsync.c, hle/hpscale.c)
typedef struct {
    char magic[4];
    uint16_t version;
    uint8_t scan_only, pad;
    uint16_t lcv;
    uint8_t mac[6];
    char name[0x21];
    uint8_t pad2[3];
} Hello;
#define MAX_FRAME (0x10000 + 64)

// The game's packets go by UDP when it gets through (Pia, the game's network library, expects datagrams: over TCP
// one lost packet would hold up everything behind it): datagrams "MHGD" + token + kind [+ VPacket + data] between
// a station and the host's port. A station says hello every 2 s (keeping NAT mappings open), the host answers;
// without UDP (blocked, not forwarded) or after 15 s without any, the packets go through the TCP link instead.
#define UDP_MAGIC "MHGD"
enum { U_DATA = 0, U_HELLO, U_HELLO_ACK };
typedef struct { char magic[4]; uint32_t token; uint8_t kind, pad[3]; } UHdr;
#define UDP_MAX_PAYLOAD 8192
#define UDP_SILENCE_NS 15000000000ull

// The hub server (server/server.go has the protocol): control frames like the link's; requests start with a header
// "MHGS", u16 LOBBY_PROTO, u16 PROTO_VERSION, room[16] (the matchmaking key's hash).
#define LOBBY_PROTO 1
#define LOBBY_PORT 39510
#define LOBBY_HEADER 24
enum { S_LIST = 1, S_HOST, S_ATTACH, S_JOIN, S_UPDATE, S_PING, S_HUBS = 16, S_HOSTED, S_INCOMING, S_SPLICED, S_ERROR };
enum { SE_NOT_FOUND = 1, SE_NO_ANSWER, SE_BUSY, SE_VERSION };
#define LOBBY_PING_NS 15000000000ull
#define RELAY_MAGIC "MHGR"
#define RELAY_PREFIX 16  // magic, u32 pair, u64 secret

typedef struct {
    hsock s;
    pthread_mutex_t wmu;  // one writer at a time; s is closed only with it held (alive cleared first)
    pthread_t th;
    bool alive, has_thread;
    uint32_t ip;
    uint32_t token;               // UDP: identifies the station's datagrams
    struct sockaddr_in uaddr;     // host side: where the station's datagrams come from
    volatile bool udp_up;
    volatile uint64_t udp_rx_ns;  // last datagram from the other end
    uint32_t relay_pair;          // through the hub server (0: direct): its pair id and our secret for UDP
    uint64_t relay_secret;
} Link;

static struct {
    pthread_mutex_t mu;
    int state, reason;
    uint8_t info[NETINFO_SIZE];
    uint32_t own_ip, subnet;
    uint8_t mac[6];
    uint8_t adv[ADV_MAX];
    uint32_t adv_size;
    int accept_policy;
    uint16_t lcv;
    KEvent *ev;
    uint32_t ev_r;
    // host
    hsock listen_s, disc_s;
    pthread_t host_th;
    bool hosting;
    Link nodes[MAX_NODES];  // [0] unused (the host itself)
    // station
    Link up;
    hsock us;  // UDP socket connected to the host
    pthread_t uth;
    volatile bool uth_run;
    // host: the hub's listing on the hub server
    pthread_t lobby_th;
    volatile bool lobby_run, lobby_dirty;  // dirty: the NetworkInfo changed, send it
    struct sockaddr_in lobby_addr;
} L = {.mu = PTHREAD_MUTEX_INITIALIZER, .listen_s = BADSOCK, .disc_s = BADSOCK, .us = BADSOCK};

// Scan results: where each found network's host is (hub: on the hub server at addr, 0: the host's own address).
#define MAX_FOUND 32
static struct { uint8_t bssid[6], session[16]; struct sockaddr_in addr; uint64_t hub; } found[MAX_FOUND];
static int nfound;

static void signal_change(void) {
    if (!L.ev) return;
    pthread_mutex_lock(&g_klock);
    kernel_signal(L.ev);
    pthread_mutex_unlock(&g_klock);
}

static void rand_bytes(void *p, size_t n) {
    for (size_t i = 0; i < n; i++) ((uint8_t *)p)[i] = (uint8_t)arc4random();
}
static const char *ipstr(uint32_t ip) {
    static __thread char b[2][20];
    static __thread int k;
    char *s = b[k++ & 1];
    snprintf(s, 20, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    return s;
}
static uint8_t *node_at(uint8_t *info, int i) { return info + NI_NODES + i * NODE_SIZE; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void wr16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

// A NetworkInfo from the network (LAN, hub server, host), before the game sees it: the game trusts it as it came from
// the system. Counts and sizes in range, strings terminated, node ids = their index (free entries all zero, as ours).
static bool info_sane(uint8_t *info) {
    uint8_t max = info[NI_NODE_MAX], n = info[NI_NODE_COUNT];
    if (info[NI_SSID] > 32 || max < 1 || max > MAX_NODES || n < 1 || n > max || rd16(info + NI_ADV_SIZE) > ADV_MAX)
        return false;
    info[NI_SSID + 0x21] = 0;
    for (int i = 0; i < MAX_NODES; i++) {
        uint8_t *nd = node_at(info, i);
        if (!nd[NODE_CONNECTED]) { memset(nd, 0, NODE_SIZE); continue; }
        nd[NODE_CONNECTED] = 1;
        nd[NODE_ID] = (uint8_t)i;
        nd[NODE_NAME + 0x20] = 0;
    }
    return true;
}

#define ONCE(...) do { static int once_; if (!once_++) LOG(__VA_ARGS__); } while (0)

// ---- frames
static bool send_all(hsock s, const void *b, size_t n) {
    for (const char *p = b; n;) {
        int k = send(s, p, (int)n, 0);
        if (k <= 0) return false;
        p += k, n -= (size_t)k;
    }
    return true;
}
static bool recv_all(hsock s, void *b, size_t n) {
    for (char *p = b; n;) {
        int k = recv(s, p, (int)n, 0);
        if (k <= 0) return false;
        p += k, n -= (size_t)k;
    }
    return true;
}
static bool frame_send(hsock s, uint8_t type, const void *a, uint32_t alen, const void *b, uint32_t blen) {
    uint8_t h[5];
    uint32_t size = 1 + alen + blen;
    memcpy(h, &size, 4);
    h[4] = type;
    return send_all(s, h, 5) && (!alen || send_all(s, a, alen)) && (!blen || send_all(s, b, blen));
}
static bool link_send(Link *l, uint8_t type, const void *a, uint32_t alen, const void *b, uint32_t blen) {
    pthread_mutex_lock(&l->wmu);
    bool ok = l->alive && frame_send(l->s, type, a, alen, b, blen);
    pthread_mutex_unlock(&l->wmu);
    return ok;
}
// -1: closed / error; else the payload size (buf holds MAX_FRAME bytes)
static int frame_recv(hsock s, uint8_t *type, uint8_t *buf) {
    uint32_t size;
    if (!recv_all(s, &size, 4) || size < 1 || size > MAX_FRAME + 1 || !recv_all(s, type, 1)) return -1;
    return recv_all(s, buf, size - 1) ? (int)size - 1 : -1;
}
static void link_close(Link *l) {  // (caller: not holding L.mu if l has a thread to join)
    pthread_mutex_lock(&l->wmu);
    if (l->alive) {
        l->alive = false;
        shutdown(l->s, 2);
        hclose(l->s);
    }
    pthread_mutex_unlock(&l->wmu);
}

static hsock connect_to(const struct sockaddr_in *a, int timeout_ms) {
    hsock s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == BADSOCK) return BADSOCK;
    hset_nonblock(s, true);
    connect(s, (const struct sockaddr *)a, sizeof *a);
    fd_set w, e;
    FD_ZERO(&w);
    FD_ZERO(&e);
    FD_SET(s, &w);
    FD_SET(s, &e);
    struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    int err = 1;
    socklen_t el = sizeof err;
    if (select((int)s + 1, NULL, &w, &e, &tv) <= 0 || !FD_ISSET(s, &w) ||
        getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &el) || err) {
        hclose(s);
        return BADSOCK;
    }
    hset_nonblock(s, false);
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    return s;
}

// Through the hub server, a datagram starts with "MHGR", the pair and our secret; the server passes on the rest.
static uint32_t relay_prefix(uint8_t *buf, const Link *l) {
    if (!l->relay_pair) return 0;
    memcpy(buf, RELAY_MAGIC, 4);
    wr32(buf + 4, l->relay_pair);
    memcpy(buf + 8, &l->relay_secret, 8);
    return RELAY_PREFIX;
}
// The host tells the server where its end of a relayed link is (until the station's datagrams come through).
static void relay_register(hsock s, const Link *l) {
    uint8_t buf[RELAY_PREFIX];
    sendto(s, (const char *)buf, (int)relay_prefix(buf, l), 0, (const struct sockaddr *)&l->uaddr, sizeof l->uaddr);
}

// "host[:port]" (port: def without one) -> its IPv4 address
static bool resolve_host(const char *text, int def, struct sockaddr_in *out) {
    char host[256];
    int port = def;
    snprintf(host, sizeof host, "%s", text);
    char *colon = strrchr(host, ':');
    if (colon) { *colon = 0; port = atoi(colon + 1); }
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (port < 1 || port > 65535 || getaddrinfo(host, NULL, &hints, &res) || !res) return false;
    *out = *(struct sockaddr_in *)res->ai_addr;
    out->sin_port = htons((uint16_t)port);
    freeaddrinfo(res);
    return true;
}

// One datagram on link l: header [+ packet]. to = NULL: s is connected (a station's socket).
static bool udp_send(hsock s, const struct sockaddr_in *to, const Link *l, uint8_t kind, const VPacket *p,
                     const void *data, uint32_t len) {
    uint8_t buf[RELAY_PREFIX + sizeof(UHdr) + sizeof(VPacket) + UDP_MAX_PAYLOAD];
    if (len > UDP_MAX_PAYLOAD) return false;
    UHdr h = {{0}, l->token, kind, {0}};
    memcpy(h.magic, UDP_MAGIC, 4);
    uint32_t n = relay_prefix(buf, l);
    memcpy(buf + n, &h, sizeof h);
    n += sizeof h;
    if (p) {
        memcpy(buf + n, p, sizeof *p);
        n += sizeof *p;
        if (len) memcpy(buf + n, data, len);
        n += len;
    }
    int k = to ? sendto(s, (const char *)buf, (int)n, 0, (const struct sockaddr *)to, sizeof *to)
               : send(s, (const char *)buf, (int)n, 0);
    return k == (int)n;
}

static Hello make_hello(bool scan_only, const uint8_t *user_name) {
    Hello h = {0};
    memcpy(h.magic, PROTO_MAGIC, 4);
    h.version = PROTO_VERSION;
    h.scan_only = scan_only;
    h.lcv = L.lcv;
    memcpy(h.mac, L.mac, 6);
    if (user_name) memcpy(h.name, user_name, 0x20);
    return h;
}

// ---- the virtual network's packets (vsock.c -> here)
uint32_t ldn_own_ip(void) { return L.own_ip; }
uint32_t ldn_broadcast_ip(void) { return L.subnet ? L.subnet | 0xFF : 0xFFFFFFFFu; }
bool ldn_net_up(void) { return L.state == ST_AP_CREATED || L.state == ST_STATION_CONNECTED; }
bool ldn_hosting(void) { return L.state == ST_AP_CREATED; }
bool ldn_party(int *n, int *max, char *id, size_t size) {
    pthread_mutex_lock(&L.mu);
    bool up = ldn_net_up();
    if (up) {
        *n = L.info[NI_NODE_COUNT], *max = L.info[NI_NODE_MAX];
        Sha256 h;  // (a hash of the session id: Discord shows the party id to others, the network's own id stays here)
        char hex[65];
        sha256_init(&h);
        sha256_update(&h, "mhgu-party", 10);
        sha256_update(&h, L.info + NI_SESSION_ID, 16);
        sha256_hex(&h, hex);
        snprintf(id, size, "%.32s", hex);
    }
    pthread_mutex_unlock(&L.mu);
    return up;
}

static bool udp_usable(const Link *l) { return l->udp_up && plat_now_ns() - l->udp_rx_ns < UDP_SILENCE_NS; }

// Host: deliver to the host's own sockets and/or forward to the stations (from: the sending node, 0 = the host).
static void host_route(int from, const VPacket *p, const void *data, uint32_t len) {
    bool all = p->dst_ip == ldn_broadcast_ip() || p->dst_ip == 0xFFFFFFFFu;
    if (all || p->dst_ip == L.own_ip) vsock_deliver(p, data, len);
    for (int i = 1; i < MAX_NODES; i++) {
        Link *l = &L.nodes[i];
        if (i == from || !l->alive || !(all || l->ip == p->dst_ip)) continue;
        hsock ds = L.disc_s;
        if (!(ds != BADSOCK && udp_usable(l) && udp_send(ds, &l->uaddr, l, U_DATA, p, data, len)))
            link_send(l, F_PACKET, p, sizeof *p, data, len);
    }
}

void ldn_send(const VPacket *p, const void *data, uint32_t len) {
    if (L.state == ST_AP_CREATED) { host_route(0, p, data, len); return; }
    if (L.state != ST_STATION_CONNECTED) return;
    bool all = p->dst_ip == ldn_broadcast_ip() || p->dst_ip == 0xFFFFFFFFu;
    if (all || p->dst_ip == L.own_ip) vsock_deliver(p, data, len);  // (a broadcast reaches the sender too)
    if (p->dst_ip == L.own_ip) return;
    hsock us = L.us;
    if (!(us != BADSOCK && udp_usable(&L.up) && udp_send(us, NULL, &L.up, U_DATA, p, data, len)))
        link_send(&L.up, F_PACKET, p, sizeof *p, data, len);
}

// The port's own messages to every other game in the network (reliable, in order: the TCP link).
void ldn_send_aux(const void *data, uint32_t len) {
    if (L.state == ST_AP_CREATED) {
        for (int i = 1; i < MAX_NODES; i++)
            if (L.nodes[i].alive) link_send(&L.nodes[i], F_AUX, data, len, NULL, 0);
    } else if (L.state == ST_STATION_CONNECTED) {
        link_send(&L.up, F_AUX, data, len, NULL, 0);
    }
}

// ---- MHGU_LDN_LAG=<ms>: received packets wait that long before they go on (testing high ping: set it on both
// ends, the round trip grows by twice the value). Kept in arrival order by one delivery thread.
typedef struct Delayed { struct Delayed *next; uint64_t due; int from; VPacket p; uint32_t len; uint8_t data[]; } Delayed;
static Delayed *lag_head, *lag_tail;
static pthread_mutex_t lag_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t lag_cv = PTHREAD_COND_INITIALIZER;

static void *lag_thread(void *arg) {
    (void)arg;
    pthread_mutex_lock(&lag_mu);
    for (;;) {
        while (!lag_head) pthread_cond_wait(&lag_cv, &lag_mu);
        Delayed *d = lag_head;
        uint64_t now = plat_now_ns();
        if (now < d->due) {
            pthread_mutex_unlock(&lag_mu);
            plat_sleep_until_ns(d->due);
            pthread_mutex_lock(&lag_mu);
            continue;
        }
        lag_head = d->next;
        if (!lag_head) lag_tail = NULL;
        pthread_mutex_unlock(&lag_mu);
        if (d->from > 0) host_route(d->from, &d->p, d->data, d->len);
        else vsock_deliver(&d->p, d->data, d->len);
        free(d);
        pthread_mutex_lock(&lag_mu);
    }
    return NULL;
}

// A packet from the link: from = the sending station (host side) or 0 (a station: from the host).
// MHGU_LDN_LOSS=<percent>: that share of the received game packets is dropped (testing a lossy connection).
static void link_received(int from, const VPacket *p, const void *data, uint32_t len) {
    static int lag_ms = -1, loss_pct;
    if (lag_ms < 0) {
        const char *e = getenv("MHGU_LDN_LAG");
        lag_ms = e ? atoi(e) : 0;
        if (lag_ms > 0) {
            pthread_t t;
            pthread_create(&t, NULL, lag_thread, NULL);
            LOG("[ldn] MHGU_LDN_LAG: received packets are held back %d ms\n", lag_ms);
        }
        if ((e = getenv("MHGU_LDN_LOSS")) && atoi(e) > 0) {
            loss_pct = atoi(e);
            LOG("[ldn] MHGU_LDN_LOSS: dropping %d%% of the received packets\n", loss_pct);
        }
    }
    if (loss_pct && (int)(arc4random() % 100) < loss_pct) return;
    if (lag_ms <= 0) {
        if (from > 0) host_route(from, p, data, len);
        else vsock_deliver(p, data, len);
        return;
    }
    Delayed *d = malloc(sizeof *d + len);
    if (!d) return;
    d->next = NULL, d->due = plat_now_ns() + (uint64_t)lag_ms * 1000000ull, d->from = from, d->p = *p, d->len = len;
    memcpy(d->data, data, len);
    pthread_mutex_lock(&lag_mu);
    if (lag_tail) lag_tail->next = d; else lag_head = d;
    lag_tail = d;
    pthread_cond_signal(&lag_cv);
    pthread_mutex_unlock(&lag_mu);
}

// ---- host
static void push_info_locked(int except) {  // to every station (and the hub server's list): the network changed
    for (int i = 1; i < MAX_NODES; i++)
        if (i != except && L.nodes[i].alive) link_send(&L.nodes[i], F_INFO, L.info, NETINFO_SIZE, NULL, 0);
    L.lobby_dirty = true;
}

// A station's connection: accepted here, or opened to the hub server for a player it announced (relay_pair != 0).
typedef struct { hsock s; struct sockaddr_in from; uint32_t relay_pair; uint64_t relay_secret; } Accepted;

static void *station_thread(void *arg) {
    Accepted acc = *(Accepted *)arg;
    free(arg);
    static __thread uint8_t buf[MAX_FRAME];
    uint8_t type;
    hset_timeout(acc.s, 5000);
    int n = frame_recv(acc.s, &type, buf);
    Hello h;
    if (n < (int)sizeof h || type != F_HELLO) { hclose(acc.s); return NULL; }
    memcpy(&h, buf, sizeof h);
    if (memcmp(h.magic, PROTO_MAGIC, 4) || h.version != PROTO_VERSION) {
        uint32_t r = R_CONNECT_REJECTED;
        frame_send(acc.s, F_REFUSE, &r, 4, NULL, 0);
        hclose(acc.s);
        return NULL;
    }
    pthread_mutex_lock(&L.mu);
    if (h.scan_only || L.state != ST_AP_CREATED) {
        uint8_t info[NETINFO_SIZE];
        memcpy(info, L.info, sizeof info);
        bool up = L.state == ST_AP_CREATED;
        pthread_mutex_unlock(&L.mu);
        if (up) frame_send(acc.s, F_INFO, info, sizeof info, NULL, 0);
        hclose(acc.s);
        return NULL;
    }
    uint32_t refuse = 0;
    int slot = 0;
    if (h.lcv < L.lcv) refuse = R_VERSION_TOO_LOW;
    else if (h.lcv > L.lcv) refuse = R_VERSION_TOO_HIGH;
    else if (L.accept_policy == 1) refuse = R_CONNECT_REJECTED;  // RejectAll
    else if (L.info[NI_NODE_COUNT] >= L.info[NI_NODE_MAX]) refuse = R_TOO_MANY_PLAYERS;
    else {
        for (slot = 1; slot < MAX_NODES && (L.nodes[slot].alive || L.nodes[slot].has_thread); slot++) {}
        if (slot >= MAX_NODES || slot >= L.info[NI_NODE_MAX]) refuse = R_TOO_MANY_PLAYERS;
    }
    if (refuse) {
        pthread_mutex_unlock(&L.mu);
        frame_send(acc.s, F_REFUSE, &refuse, 4, NULL, 0);
        hclose(acc.s);
        LOG("[ldn] refused a player%s (result %#x)\n", acc.relay_pair ? " through the hub server" : "", refuse);
        return NULL;
    }
    Link *l = &L.nodes[slot];
    l->s = acc.s, l->ip = L.subnet | (uint32_t)(slot + 1), l->alive = true, l->has_thread = true;
    l->th = pthread_self();
    l->token = arc4random() | 1, l->udp_up = false, l->udp_rx_ns = 0;
    l->relay_pair = acc.relay_pair, l->relay_secret = acc.relay_secret;
    l->uaddr = acc.from;  // (relayed: the server, whose UDP port is its TCP port)
    if (l->relay_pair && L.disc_s != BADSOCK) relay_register(L.disc_s, l);
    uint8_t *nd = node_at(L.info, slot);
    memset(nd, 0, NODE_SIZE);
    wr32(nd + NODE_IP, l->ip);
    memcpy(nd + NODE_MAC, h.mac, 6);
    nd[NODE_ID] = (uint8_t)slot;
    nd[NODE_CONNECTED] = 1;
    memcpy(nd + NODE_NAME, h.name, 0x20);
    wr16(nd + NODE_LCV, h.lcv);
    L.info[NI_NODE_COUNT]++;
    uint8_t welcome[8 + NETINFO_SIZE] = {(uint8_t)slot};  // node, -, -, -, u32 UDP token, NetworkInfo
    wr32(welcome + 4, l->token);
    memcpy(welcome + 8, L.info, NETINFO_SIZE);
    link_send(l, F_WELCOME, welcome, sizeof welcome, NULL, 0);
    push_info_locked(slot);
    pthread_mutex_unlock(&L.mu);
    hset_timeout(acc.s, 0);
    LOG("[ldn] %.32s joined %s as %s (node %d)\n", h.name, acc.relay_pair ? "through the hub server" : "from the LAN / by address",
        ipstr(l->ip), slot);
    signal_change();

    while ((n = frame_recv(acc.s, &type, buf)) >= 0) {
        if (type == F_PACKET && n >= (int)sizeof(VPacket)) {
            VPacket p;
            memcpy(&p, buf, sizeof p);
            p.src_ip = l->ip;  // (a station speaks for itself only)
            link_received(slot, &p, buf + sizeof p, (uint32_t)n - sizeof p);
        } else if (type == F_AUX && n > 0) {  // to the host's own game and on to the other stations
            emsync_aux_received(buf, (uint32_t)n);
            hpscale_aux_received(buf, (uint32_t)n);
            for (int i = 1; i < MAX_NODES; i++)
                if (i != slot && L.nodes[i].alive) link_send(&L.nodes[i], F_AUX, buf, (uint32_t)n, NULL, 0);
        }
    }

    pthread_mutex_lock(&L.mu);
    bool was = l->alive;
    link_close(l);
    if (was && L.state == ST_AP_CREATED) {
        memset(node_at(L.info, slot), 0, NODE_SIZE);
        if (L.info[NI_NODE_COUNT] > 1) L.info[NI_NODE_COUNT]--;
        push_info_locked(0);
    }
    l->has_thread = false;
    pthread_mutex_unlock(&L.mu);
    LOG("[ldn] node %d (%s) left\n", slot, ipstr(l->ip));
    vsock_peer_gone(l->ip);
    signal_change();
    return NULL;
}

// Accepts stations and answers LAN discovery probes until the listening sockets close.
static void *host_thread(void *arg) {
    (void)arg;
    uint64_t next_register = 0;
    for (;;) {
        hsock ls = L.listen_s, ds = L.disc_s;
        if (ls == BADSOCK) break;
        uint64_t now = plat_now_ns();
        if (ds != BADSOCK && now >= next_register) {  // relayed links: until UDP from the station gets through
            next_register = now + 1000000000ull;
            for (int i = 1; i < MAX_NODES; i++)
                if (L.nodes[i].alive && L.nodes[i].relay_pair && !udp_usable(&L.nodes[i])) relay_register(ds, &L.nodes[i]);
        }
        fd_set r;
        FD_ZERO(&r);
        FD_SET(ls, &r);
        if (ds != BADSOCK) FD_SET(ds, &r);
        struct timeval tv = {0, 200000};
        int k = select((int)(ls > ds || ds == BADSOCK ? ls : ds) + 1, &r, NULL, NULL, &tv);
        if (!L.hosting) break;
        if (k <= 0) continue;
        if (FD_ISSET(ls, &r)) {
            Accepted *a = calloc(1, sizeof *a);
            socklen_t al = sizeof a->from;
            a->s = accept(ls, (struct sockaddr *)&a->from, &al);
            if (a->s == BADSOCK) { free(a); continue; }
            int one = 1;
            setsockopt(a->s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
            pthread_t t;
            if (pthread_create(&t, NULL, station_thread, a)) { hclose(a->s); free(a); }
            else pthread_detach(t);
        }
        if (ds != BADSOCK && FD_ISSET(ds, &r)) {
            static uint8_t probe[sizeof(UHdr) + sizeof(VPacket) + UDP_MAX_PAYLOAD];
            struct sockaddr_in from;
            socklen_t fl = sizeof from;
            int n = recvfrom(ds, (char *)probe, sizeof probe, 0, (struct sockaddr *)&from, &fl);
            if (n >= (int)sizeof(UHdr) && !memcmp(probe, UDP_MAGIC, 4)) {  // a station's datagram
                UHdr h;
                memcpy(&h, probe, sizeof h);
                int slot = 1;
                while (slot < MAX_NODES && !(L.nodes[slot].alive && L.nodes[slot].token == h.token)) slot++;
                if (slot == MAX_NODES) continue;
                Link *l = &L.nodes[slot];
                l->uaddr = from;  // (NAT may change it: the latest wins)
                l->udp_rx_ns = plat_now_ns();
                if (h.kind == U_HELLO) {
                    if (!l->udp_up) LOG("[ldn] node %d: game packets go by UDP\n", slot);
                    l->udp_up = true;
                    udp_send(ds, &from, l, U_HELLO_ACK, NULL, NULL, 0);
                } else if (h.kind == U_DATA && n >= (int)(sizeof h + sizeof(VPacket))) {
                    VPacket p;
                    memcpy(&p, probe + sizeof h, sizeof p);
                    p.src_ip = l->ip;
                    link_received(slot, &p, probe + sizeof h + sizeof p, (uint32_t)n - sizeof h - sizeof p);
                }
                continue;
            }
            if (n >= 6 && !memcmp(probe, PROTO_MAGIC, 4) && rd16(probe + 4) == PROTO_VERSION) {
                uint8_t reply[8 + NETINFO_SIZE];
                memcpy(reply, PROTO_MAGIC, 4);
                wr16(reply + 4, PROTO_VERSION);
                wr16(reply + 6, (uint16_t)g_config.ldn_port);
                pthread_mutex_lock(&L.mu);
                memcpy(reply + 8, L.info, NETINFO_SIZE);
                pthread_mutex_unlock(&L.mu);
                sendto(ds, (const char *)reply, sizeof reply, 0, (struct sockaddr *)&from, fl);
            }
        }
    }
    return NULL;
}

// ---- the hub server (internet hubs)
// ini Server: empty = the default server (LDN_DEFAULT_SERVER, config.h), "Off" = none. Its name into out
// (LOBBY_NAME bytes); false: no internet hubs. Server and Key are live settings (config_live_ldn): a change applies
// from the next search, join or connection to the server on.
#define LOBBY_NAME sizeof g_config.ldn_server
static bool lobby_server_of(const char *s, char *out) {
    if ((s[0] | 0x20) == 'o' && (s[1] | 0x20) == 'f' && (s[2] | 0x20) == 'f' && !s[3]) s = "";
    else if (!*s) s = LDN_DEFAULT_SERVER;
    snprintf(out, LOBBY_NAME, "%s", s);
    return *out;
}
static bool lobby_server(char *out) {
    char s[LOBBY_NAME];
    config_live_ldn(s, sizeof s, NULL, 0);
    return lobby_server_of(s, out);
}

// A request's header. The room is a hash of the matchmaking key (key NULL: ini Key; spaces around it ignored): hubs
// are listed only for players whose key matches, all of them without one. The server never sees the key itself.
static bool lobby_header(uint8_t *h, const char *key) {
    static const char salt[] = "MHGURecomp matchmaking key:";
    char live[sizeof g_config.ldn_key];
    if (!key) config_live_ldn(NULL, 0, live, sizeof live), key = live;
    memcpy(h, "MHGS", 4);
    wr16(h + 4, LOBBY_PROTO);
    wr16(h + 6, PROTO_VERSION);
    memset(h + 8, 0, 16);
    const char *k = key;
    while (*k == ' ') k++;
    size_t n = strlen(k);
    while (n && k[n - 1] == ' ') n--;
    if (!n) return false;
    Sha256 s;
    char hex[65];
    sha256_init(&s);
    sha256_update(&s, salt, sizeof salt - 1);
    sha256_update(&s, k, n);
    sha256_hex(&s, hex);
    for (int i = 0; i < 16; i++) {
        unsigned v;
        sscanf(hex + 2 * i, "%2x", &v);
        h[8 + i] = (uint8_t)v;
    }
    return true;
}

static const char *lobby_error(uint32_t code) {
    switch (code) {
    case SE_NOT_FOUND: return "the hub is gone";
    case SE_NO_ANSWER: return "the host did not answer";
    case SE_BUSY: return "the server is busy (too many hubs or requests from this address)";
    case SE_VERSION: return "the server speaks another protocol version";
    default: return "refused";
    }
}
static int lobby_state(uint32_t code) {  // a server's error as a HUB_* state (net.h)
    return code == SE_VERSION ? HUB_VERSION : code == SE_BUSY ? HUB_BUSY : HUB_REFUSED;
}

// What the player sees of the hub server: its last known state, for the settings menu (ldn_hub_status), and a notice
// on screen (overlay_notice) when it fails: hosting (the hub is not listed online), searching (only the LAN's hubs
// are found) or joining. Hosting notices come when the state changes; a search's every time, but not twice within
// 10 s. Texts in UI_TEXT are translated as the settings menu's (settings_locale.h,
// tests/test_settings_locale.py).
#define UI_TEXT(s) s
enum { NOTE_HOST, NOTE_SEARCH, NOTE_JOIN };
static pthread_mutex_t hub_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { int state, ms, hubs; char server[LOBBY_NAME], key[sizeof g_config.ldn_key]; uint32_t check; } hub_now;
static int note_last[3];  // HUB_*: HUB_UNKNOWN and HUB_ONLINE are fine
static uint64_t note_at[3];

static void hub_set_locked(const char *srv, const char *key, int state, int ms, int hubs) {
    hub_now.state = state, hub_now.ms = ms, hub_now.hubs = hubs;
    snprintf(hub_now.server, sizeof hub_now.server, "%s", srv);
    snprintf(hub_now.key, sizeof hub_now.key, "%s", key);
}

static void hub_report(int what, const char *srv, int state, int ms, int hubs) {
    char key[sizeof g_config.ldn_key];
    config_live_ldn(NULL, 0, key, sizeof key);
    uint64_t now = plat_now_ns();
    pthread_mutex_lock(&hub_mu);
    if (hub_now.state != HUB_CHECKING && what != NOTE_JOIN) hub_set_locked(srv, key, state, ms, hubs);
    bool show = state == HUB_ONLINE ? what == NOTE_HOST && note_last[what] > HUB_ONLINE
                                    : what == NOTE_JOIN || note_last[what] != state || now - note_at[what] > 10000000000ull;
    note_last[what] = state;
    if (show) note_at[what] = now;
    pthread_mutex_unlock(&hub_mu);
    if (!show) return;
    const char *title;
    switch (state) {
    case HUB_ONLINE: title = UI_TEXT("Connected to the hub server %s"); break;
    case HUB_NOT_FOUND: title = UI_TEXT("Hub server %s not found"); break;
    case HUB_UNREACHABLE: title = UI_TEXT("Can't reach the hub server %s"); break;
    case HUB_LOST: title = UI_TEXT("Lost the connection to the hub server %s"); break;
    case HUB_BUSY: title = UI_TEXT("The hub server %s is busy"); break;
    case HUB_VERSION: title = UI_TEXT("The hub server %s runs another version"); break;
    case HUB_REFUSED: title = UI_TEXT("The hub server %s refused the request"); break;
    default: title = UI_TEXT("The hub server %s did not answer"); break;
    }
    static const char *const next[] = {UI_TEXT("Your hub is not listed online. Trying again..."),
                                       UI_TEXT("Only hubs on your local network are shown."),
                                       UI_TEXT("Could not join the hub.")};
    int lang = config_language();
    char line[LOBBY_NAME + 128];
    snprintf(line, sizeof line, settings_text(lang, title), srv);
    const char *text = state == HUB_ONLINE ? UI_TEXT("Your hub is listed online again.") : next[what];
    if (what == NOTE_SEARCH && g_config.ldn_join[0]) text = UI_TEXT("Only hubs on your local network and Join addresses are shown.");
    overlay_notice(line, settings_text(lang, text));
}

// Host: one thread per created network keeps it listed and opens a connection for each player the server announces.
// Threads of a network that is gone (lobby_gen moved on) end by themselves; nobody waits for them.
static volatile uint32_t lobby_gen;
typedef struct { uint32_t gen; struct sockaddr_in addr; uint64_t ticket; } LobbyTicket;

static bool lobby_wait(uint32_t gen, int ms) {  // false: the network is gone
    for (uint64_t end = plat_now_ns() + (uint64_t)ms * 1000000ull; plat_now_ns() < end && lobby_gen == gen;)
        plat_sleep_until_ns(plat_now_ns() + 100000000ull);
    return lobby_gen == gen;
}

static void *lobby_attach_thread(void *arg) {
    LobbyTicket t = *(LobbyTicket *)arg;
    free(arg);
    static __thread uint8_t buf[MAX_FRAME];
    hsock s = connect_to(&t.addr, 3000);
    if (s == BADSOCK) return NULL;
    uint8_t req[LOBBY_HEADER + 8], type;
    lobby_header(req, NULL);
    memcpy(req + LOBBY_HEADER, &t.ticket, 8);
    hset_timeout(s, 5000);
    int n;
    if (!frame_send(s, S_ATTACH, req, sizeof req, NULL, 0) || (n = frame_recv(s, &type, buf)) < 12 || type != S_SPLICED ||
        lobby_gen != t.gen) {
        LOG("[ldn] a player's connection through the hub server failed\n");
        hclose(s);
        return NULL;
    }
    Accepted *a = calloc(1, sizeof *a);
    a->s = s, a->from = t.addr, a->relay_pair = rd32(buf), a->relay_secret = rd64(buf + 4);
    return station_thread(a);
}

static void *lobby_thread(void *arg) {
    uint32_t gen = (uint32_t)(uintptr_t)arg;
    static __thread uint8_t buf[MAX_FRAME];
    uint8_t req[LOBBY_HEADER + NETINFO_SIZE], type;
    while (lobby_gen == gen) {
        char srv[LOBBY_NAME];
        struct sockaddr_in addr;
        hsock s = BADSOCK;
        if (!lobby_server(srv)) break;
        bool named = resolve_host(srv, LOBBY_PORT, &addr);
        if (named) s = connect_to(&addr, 3000);
        if (s == BADSOCK) {
            LOG("[ldn] hub server %s %s: this hub is not listed online (trying again in 30 s)\n", srv,
                named ? "unreachable" : "not found");
            if (lobby_gen == gen) hub_report(NOTE_HOST, srv, named ? HUB_UNREACHABLE : HUB_NOT_FOUND, -1, -1);
            if (!lobby_wait(gen, 30000)) break;
            continue;
        }
        bool keyed = lobby_header(req, NULL);
        pthread_mutex_lock(&L.mu);
        memcpy(req + LOBBY_HEADER, L.info, NETINFO_SIZE);
        L.lobby_dirty = false;
        pthread_mutex_unlock(&L.mu);
        hset_timeout(s, 5000);
        int n = -1;
        uint64_t asked = plat_now_ns();
        if (frame_send(s, S_HOST, req, sizeof req, NULL, 0)) n = frame_recv(s, &type, buf);
        if (n < 8 || type != S_HOSTED) {
            bool error = n >= 4 && type == S_ERROR;
            LOG("[ldn] hub server %s: can't list this hub (%s); trying again in 30 s\n", srv,
                error ? lobby_error(rd32(buf)) : "no answer");
            hclose(s);
            if (lobby_gen == gen) hub_report(NOTE_HOST, srv, error ? lobby_state(rd32(buf)) : HUB_NO_ANSWER, -1, -1);
            if (!lobby_wait(gen, 30000)) break;
            continue;
        }
        LOG("[ldn] hub listed on the hub server %s: %s\n", srv,
            keyed ? "players with the same matchmaking key see it" : "public, every player sees it");
        hub_report(NOTE_HOST, srv, HUB_ONLINE, (int)((plat_now_ns() - asked) / 1000000), -1);
        uint64_t now = plat_now_ns(), last_rx = now, next_ping = now + LOBBY_PING_NS;
        while (lobby_gen == gen) {
            now = plat_now_ns();
            if (L.lobby_dirty) {
                uint8_t info[NETINFO_SIZE];
                pthread_mutex_lock(&L.mu);
                memcpy(info, L.info, NETINFO_SIZE);
                L.lobby_dirty = false;
                pthread_mutex_unlock(&L.mu);
                if (!frame_send(s, S_UPDATE, info, NETINFO_SIZE, NULL, 0)) break;
            }
            if (now >= next_ping) {
                if (!frame_send(s, S_PING, NULL, 0, NULL, 0)) break;
                next_ping = now + LOBBY_PING_NS;
            }
            if (now - last_rx > 3 * LOBBY_PING_NS) break;
            fd_set r;
            FD_ZERO(&r);
            FD_SET(s, &r);
            struct timeval tv = {0, 200000};
            if (select((int)s + 1, &r, NULL, NULL, &tv) <= 0) continue;
            if ((n = frame_recv(s, &type, buf)) < 0) break;
            last_rx = plat_now_ns();
            if (type == S_INCOMING && n >= 8) {
                LobbyTicket *t = malloc(sizeof *t);
                pthread_t th;
                t->gen = gen, t->addr = addr, t->ticket = rd64(buf);
                if (pthread_create(&th, NULL, lobby_attach_thread, t)) free(t);
                else pthread_detach(th);
            }
        }
        hclose(s);
        if (lobby_gen != gen) break;
        LOG("[ldn] lost the hub server: this hub is not listed online (trying again in 10 s)\n");
        hub_report(NOTE_HOST, srv, HUB_LOST, -1, -1);
        if (!lobby_wait(gen, 10000)) break;
    }
    return NULL;
}

static void lobby_host_start(void) {
    uint32_t gen = ++lobby_gen;
    pthread_t th;
    char srv[LOBBY_NAME];
    pthread_mutex_lock(&hub_mu);
    note_last[NOTE_HOST] = HUB_ONLINE;  // (each hub tells of its own first failure)
    pthread_mutex_unlock(&hub_mu);
    if (lobby_server(srv) && !pthread_create(&th, NULL, lobby_thread, (void *)(uintptr_t)gen)) pthread_detach(th);
}
static void lobby_host_stop(void) { lobby_gen++; }

// Station: the hubs the server lists for a key (NULL: ini Key). A HUB_* state; ms: the request's round trip.
#define LOBBY_LIST_MAX 24
typedef struct {
    uint8_t infos[LOBBY_LIST_MAX][NETINFO_SIZE];
    uint64_t ids[LOBBY_LIST_MAX];
    int n, state, ms;
    struct sockaddr_in addr;
    char server[LOBBY_NAME];
} LobbyList;

static int lobby_list(const char *srv, const char *key, int connect_ms, LobbyList *r) {
    static __thread uint8_t buf[MAX_FRAME];
    r->n = 0, r->ms = -1;
    if (!resolve_host(srv, LOBBY_PORT, &r->addr)) return HUB_NOT_FOUND;
    hsock s = connect_to(&r->addr, connect_ms);
    if (s == BADSOCK) return HUB_UNREACHABLE;
    uint8_t req[LOBBY_HEADER], type;
    lobby_header(req, key);
    hset_timeout(s, 2000);
    uint64_t asked = plat_now_ns();
    int n = frame_send(s, S_LIST, req, sizeof req, NULL, 0) ? frame_recv(s, &type, buf) : -1;
    r->ms = (int)((plat_now_ns() - asked) / 1000000);
    hclose(s);
    if (n >= 4 && type == S_ERROR) return lobby_state(rd32(buf));
    if (n < 4 || type != S_HUBS) return HUB_NO_ANSWER;
    uint32_t count = rd32(buf);
    for (uint32_t i = 0; i < count && r->n < LOBBY_LIST_MAX && 4 + (i + 1) * (8 + NETINFO_SIZE) <= (uint32_t)n; i++) {
        const uint8_t *e = buf + 4 + i * (8 + NETINFO_SIZE);
        r->ids[r->n] = rd64(e);
        memcpy(r->infos[r->n++], e + 8, NETINFO_SIZE);
    }
    return HUB_ONLINE;
}

static void *lobby_list_thread(void *arg) {  // (while the LAN is searched; r->server set)
    LobbyList *r = arg;
    static int last = HUB_ONLINE;
    r->state = lobby_list(r->server, NULL, 1500, r);
    if (r->state != last && r->state != HUB_ONLINE)
        LOG("[ldn] hub server %s: %s: only LAN / Join hubs are searched\n", r->server,
            r->state == HUB_NOT_FOUND ? "not found" : r->state == HUB_UNREACHABLE ? "unreachable" : "no list");
    last = r->state;
    hub_report(NOTE_SEARCH, r->server, r->state, r->ms, r->state == HUB_ONLINE ? r->n : -1);
    return NULL;
}

// The settings menu's check (net.h): the server and key being edited, in a thread of its own.
typedef struct { uint32_t check; char server[LOBBY_NAME], key[sizeof g_config.ldn_key]; } HubCheck;

static void *hub_check_thread(void *arg) {
    HubCheck *c = arg;
    LobbyList *r = malloc(sizeof *r);
    int state = r ? lobby_list(c->server, c->key, 3000, r) : HUB_UNKNOWN;
    int ms = r ? r->ms : -1, hubs = state == HUB_ONLINE ? r->n : -1;
    pthread_mutex_lock(&hub_mu);
    if (hub_now.check == c->check) hub_set_locked(c->server, c->key, state, ms, hubs);
    pthread_mutex_unlock(&hub_mu);
    LOG("[ldn] hub server check: %s: state %d, %d ms, %d hub(s) for this key\n", c->server, state, ms, hubs);
    free(r);
    free(c);
    return NULL;
}

void ldn_hub_check(const char *server, const char *key) {
    HubCheck *c = calloc(1, sizeof *c);
    pthread_t th;
    if (!c) return;
    hnet_init();
    bool on = lobby_server_of(server, c->server);
    snprintf(c->key, sizeof c->key, "%s", key);
    pthread_mutex_lock(&hub_mu);
    c->check = ++hub_now.check;
    hub_set_locked(c->server, c->key, on ? HUB_CHECKING : HUB_OFF, -1, -1);
    pthread_mutex_unlock(&hub_mu);
    if (on && !pthread_create(&th, NULL, hub_check_thread, c)) {
        pthread_detach(th);
        return;
    }
    if (on) {
        pthread_mutex_lock(&hub_mu);
        hub_set_locked(c->server, c->key, HUB_UNKNOWN, -1, -1);
        pthread_mutex_unlock(&hub_mu);
    }
    free(c);
}

int ldn_hub_status(const char *server, const char *key, int *ms, int *hubs) {
    char name[LOBBY_NAME];
    if (!lobby_server_of(server, name)) return HUB_OFF;
    pthread_mutex_lock(&hub_mu);
    // (the server's state holds whatever the key; the hub count only for the key it was for)
    int state = strcmp(hub_now.server, name) ? HUB_UNKNOWN : hub_now.state;
    if (ms) *ms = hub_now.ms;
    if (hubs) *hubs = strcmp(hub_now.key, key) ? -1 : hub_now.hubs;
    pthread_mutex_unlock(&hub_mu);
    return state;
}

// Joining a listed hub: the server asks its host to connect and splices the two. 0, else an nn::ldn result.
static uint32_t lobby_join(const struct sockaddr_in *addr, uint64_t hub, hsock *out, uint32_t *pair, uint64_t *secret) {
    static uint8_t buf[MAX_FRAME];
    char srv[LOBBY_NAME];
    if (!lobby_server(srv)) snprintf(srv, sizeof srv, "%s", inet_ntoa(addr->sin_addr));
    hsock s = connect_to(addr, 3000);
    if (s == BADSOCK) {
        LOG("[ldn] can't reach the hub server\n");
        hub_report(NOTE_JOIN, srv, HUB_UNREACHABLE, -1, -1);
        return R_CONNECT_TIMEOUT;
    }
    uint8_t req[LOBBY_HEADER + 8], type;
    lobby_header(req, NULL);
    memcpy(req + LOBBY_HEADER, &hub, 8);
    hset_timeout(s, 12000);  // (the server waits up to 10 s for the host)
    int n = frame_send(s, S_JOIN, req, sizeof req, NULL, 0) ? frame_recv(s, &type, buf) : -1;
    if (n >= 12 && type == S_SPLICED) {
        *out = s, *pair = rd32(buf), *secret = rd64(buf + 4);
        return 0;
    }
    hclose(s);
    uint32_t code = n >= 4 && type == S_ERROR ? rd32(buf) : 0;
    LOG("[ldn] joining through the hub server failed: %s\n", code ? lobby_error(code) : "no answer");
    if (code == SE_NO_ANSWER) {  // (a hub that is gone: the game says so itself)
        int lang = config_language();
        overlay_notice(settings_text(lang, UI_TEXT("Could not join the hub.")),
                       settings_text(lang, UI_TEXT("The host did not answer through the hub server.")));
    } else if (code != SE_NOT_FOUND) {
        hub_report(NOTE_JOIN, srv, code ? lobby_state(code) : HUB_NO_ANSWER, -1, -1);
    }
    return code == SE_NOT_FOUND ? R_CONNECT_NOT_FOUND : code == SE_BUSY ? R_CONNECT_REJECTED : R_CONNECT_TIMEOUT;
}

static bool host_start(void) {
    hnet_init();
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)g_config.ldn_port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    hsock ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    bool bound = ls != BADSOCK && !bind(ls, (struct sockaddr *)&a, sizeof a);
    bool any_port = false;  // the port is taken: still hosting through the hub server, on a port of the system's choice
    char srv[LOBBY_NAME];
    bool online = lobby_server(srv);
    if (ls != BADSOCK && !bound && online) {
        a.sin_port = 0;
        any_port = bound = !bind(ls, (struct sockaddr *)&a, sizeof a);
    }
    if (!bound || listen(ls, 8)) {
        LOG("[ldn] can't listen on TCP port %d (in use by another program?): nobody can join this hub\n", g_config.ldn_port);
        if (ls != BADSOCK) hclose(ls);
        return false;
    }
    hsock ds = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (ds != BADSOCK && bind(ds, (struct sockaddr *)&a, sizeof a)) {
        a.sin_port = 0;  // (for the hub server's relay; LAN search needs the port)
        if (!online || bind(ds, (struct sockaddr *)&a, sizeof a)) { hclose(ds); ds = BADSOCK; }
        if (!any_port) LOG("[ldn] UDP port %d is in use: no LAN search (only Join addresses)\n", g_config.ldn_port);
    }
    L.listen_s = ls, L.disc_s = ds, L.hosting = true;
    pthread_create(&L.host_th, NULL, host_thread, NULL);
    if (any_port) LOG("[ldn] port %d is in use: hosting only through the hub server\n", g_config.ldn_port);
    else LOG("[ldn] hosting: players join this computer's address, port %d (TCP, and UDP for LAN search and game packets)\n",
             g_config.ldn_port);
    lobby_host_start();
    return true;
}

static void host_stop(uint32_t reason) {  // (caller: not holding L.mu)
    lobby_host_stop();
    if (L.hosting) {
        L.hosting = false;
        pthread_join(L.host_th, NULL);
        hclose(L.listen_s);
        if (L.disc_s != BADSOCK) hclose(L.disc_s);
        L.listen_s = L.disc_s = BADSOCK;
    }
    for (int i = 1; i < MAX_NODES; i++) {
        Link *l = &L.nodes[i];
        if (l->alive) link_send(l, F_DESTROY, &reason, 4, NULL, 0);
        link_close(l);
    }
    for (int i = 1; i < MAX_NODES; i++)  // (their threads see the closed sockets and end)
        for (int t = 0; t < 200 && L.nodes[i].has_thread; t++) plat_sleep_until_ns(plat_now_ns() + 5000000);
}

// ---- station
static void *uplink_thread(void *arg) {
    (void)arg;
    static __thread uint8_t buf[MAX_FRAME];
    uint8_t type;
    int n, reason = DR_SIGNAL_LOST;
    while ((n = frame_recv(L.up.s, &type, buf)) >= 0) {
        if (type == F_PACKET && n >= (int)sizeof(VPacket)) {
            VPacket p;
            memcpy(&p, buf, sizeof p);
            link_received(0, &p, buf + sizeof p, (uint32_t)n - sizeof p);
        } else if (type == F_INFO && n >= NETINFO_SIZE && info_sane(buf)) {
            pthread_mutex_lock(&L.mu);
            memcpy(L.info, buf, NETINFO_SIZE);
            pthread_mutex_unlock(&L.mu);
            signal_change();
        } else if (type == F_DESTROY && n >= 4) {
            reason = (int)rd32(buf);
        } else if (type == F_AUX && n > 0) {
            emsync_aux_received(buf, (uint32_t)n);
            hpscale_aux_received(buf, (uint32_t)n);
        }
    }
    L.uth_run = false;
    pthread_mutex_lock(&L.mu);
    bool lost = L.up.alive;  // (else: we disconnected ourselves)
    link_close(&L.up);
    if (lost && L.state == ST_STATION_CONNECTED) {
        L.state = ST_ERROR, L.reason = reason;
        LOG("[ldn] connection to the host ended (reason %d)\n", reason);
    }
    pthread_mutex_unlock(&L.mu);
    if (lost) { vsock_network_down(); signal_change(); }
    return NULL;
}

// The station's datagrams: hello (every 2 s once answered, else every 0.5 s) and the host's packets.
static void *station_udp_thread(void *arg) {
    (void)arg;
    static __thread uint8_t buf[sizeof(UHdr) + sizeof(VPacket) + UDP_MAX_PAYLOAD];
    hsock us = L.us;
    uint64_t next_hello = 0;
    hset_timeout(us, 250);
    while (L.uth_run) {
        uint64_t now = plat_now_ns();
        if (now >= next_hello) {
            udp_send(us, NULL, &L.up, U_HELLO, NULL, NULL, 0);
            next_hello = now + (L.up.udp_up ? 2000000000ull : 500000000ull);
        }
        int n = recv(us, (char *)buf, sizeof buf, 0);
        if (n < 0) { plat_sleep_until_ns(plat_now_ns() + 20000000); continue; }  // (timeout, or ICMP unreachable)
        UHdr h;
        if (n < (int)sizeof h || memcmp(buf, UDP_MAGIC, 4)) continue;
        memcpy(&h, buf, sizeof h);
        if (h.token != L.up.token) continue;
        L.up.udp_rx_ns = plat_now_ns();
        if (h.kind == U_HELLO_ACK && !L.up.udp_up) {
            L.up.udp_up = true;
            LOG("[ldn] UDP to the host works: game packets go by UDP\n");
        } else if (h.kind == U_DATA && n >= (int)(sizeof h + sizeof(VPacket))) {
            VPacket p;
            memcpy(&p, buf + sizeof h, sizeof p);
            link_received(0, &p, buf + sizeof h + sizeof p, (uint32_t)n - sizeof h - sizeof p);
        }
        if (L.up.udp_up && plat_now_ns() - L.up.udp_rx_ns > UDP_SILENCE_NS) {
            L.up.udp_up = false;
            LOG("[ldn] nothing by UDP from the host for 15 s: game packets go by TCP\n");
        }
    }
    return NULL;
}

static void station_leave(void) {  // (caller: not holding L.mu)
    if (L.up.has_thread) {
        link_close(&L.up);
        pthread_join(L.up.th, NULL);
        L.up.has_thread = false;
    }
    if (L.us != BADSOCK) {
        L.uth_run = false;
        pthread_join(L.uth, NULL);
        hclose(L.us);
        L.us = BADSOCK;
    }
    L.up.udp_up = false;
    vsock_network_down();
}

// Hosts to ask: ini Join (comma separated host[:port]) and this computer.
static int scan_addresses(struct sockaddr_in *out, int max) {
    int n = 0;
    char list[sizeof g_config.ldn_join];
    snprintf(list, sizeof list, "%s", g_config.ldn_join);
    for (char *t = strtok(list, ", ;"); t && n < max; t = strtok(NULL, ", ;")) {
        if (resolve_host(t, g_config.ldn_port, &out[n])) n++;
        else LOG("[ldn] Join address \"%s\" not found\n", t);
    }
    return n;
}

// Collects the networks of hosts that answer: by address (TCP) and by LAN broadcast (UDP).
static int discover(uint8_t (*infos)[NETINFO_SIZE], struct sockaddr_in *addrs, int max) {
    hnet_init();
    int n = 0;
    static __thread uint8_t buf[MAX_FRAME];
    struct sockaddr_in targets[8];
    int nt = scan_addresses(targets, 8);
    Hello h = make_hello(true, NULL);
    for (int i = 0; i < nt && n < max; i++) {
        hsock s = connect_to(&targets[i], 1500);
        if (s == BADSOCK) continue;
        hset_timeout(s, 1500);
        uint8_t type;
        int k;
        if (frame_send(s, F_HELLO, &h, sizeof h, NULL, 0) && (k = frame_recv(s, &type, buf)) >= NETINFO_SIZE && type == F_INFO) {
            memcpy(infos[n], buf, NETINFO_SIZE);
            addrs[n++] = targets[i];
        }
        hclose(s);
    }
    hsock u = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (u == BADSOCK) return n;
    int one = 1;
    setsockopt(u, SOL_SOCKET, SO_BROADCAST, (const char *)&one, sizeof one);
    uint8_t probe[8];
    memcpy(probe, PROTO_MAGIC, 4);
    wr16(probe + 4, PROTO_VERSION);
    wr16(probe + 6, 0);
    struct sockaddr_in to = {0};
    to.sin_family = AF_INET;
    to.sin_port = htons((uint16_t)g_config.ldn_port);
    to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sendto(u, (const char *)probe, sizeof probe, 0, (struct sockaddr *)&to, sizeof to);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sendto(u, (const char *)probe, sizeof probe, 0, (struct sockaddr *)&to, sizeof to);
    uint64_t end = plat_now_ns() + 400000000ull;
    for (uint64_t now; (now = plat_now_ns()) < end && n < max;) {
        fd_set r;
        FD_ZERO(&r);
        FD_SET(u, &r);
        uint64_t left = (end - now) / 1000;
        struct timeval tv = {(long)(left / 1000000), (long)(left % 1000000)};
        if (select((int)u + 1, &r, NULL, NULL, &tv) <= 0) break;
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        int k = recvfrom(u, (char *)buf, MAX_FRAME, 0, (struct sockaddr *)&from, &fl);
        if (k < 8 + NETINFO_SIZE || memcmp(buf, PROTO_MAGIC, 4) || rd16(buf + 4) != PROTO_VERSION) continue;
        bool dup = false;
        for (int i = 0; i < n; i++) dup |= !memcmp(infos[i] + NI_BSSID, buf + 8 + NI_BSSID, 6);
        if (dup) continue;
        memcpy(infos[n], buf + 8, NETINFO_SIZE);
        from.sin_port = htons(rd16(buf + 6));
        addrs[n++] = from;
    }
    hclose(u);
    return n;
}

// ---- nn::ldn

HLE_FUNC("_ZN2nn3ldn10InitializeEv", ldn_initialize) { NTRACE();
    pthread_mutex_lock(&L.mu);
    if (!L.ev) L.ev = kernel_create_event_pair(&(uint32_t){0}, &L.ev_r);
    if (L.state == ST_NONE) {
        L.state = ST_INITIALIZED, L.reason = DR_NONE;
        rand_bytes(L.mac, 6);
        L.mac[0] = (uint8_t)((L.mac[0] & 0xFC) | 0x02);  // locally administered, unicast
    }
    pthread_mutex_unlock(&L.mu);
    ONCE("[ldn] local play over IP: host port %d, join %s (state event %x)\n", g_config.ldn_port,
         g_config.ldn_join[0] ? g_config.ldn_join : "(LAN search only)", L.ev_r);
    RET(0);
}

static void to_initialized(void) {
    int st = L.state;
    if (st == ST_AP_CREATED || st == ST_ACCESS_POINT) host_stop(DR_DESTROYED_BY_USER);
    if (st == ST_STATION_CONNECTED || st == ST_STATION || st == ST_ERROR) station_leave();
    if (st == ST_AP_CREATED) vsock_network_down();
    L.own_ip = L.subnet = 0;
}

HLE_FUNC("_ZN2nn3ldn8FinalizeEv", ldn_finalize) { NTRACE();
    to_initialized();
    L.state = ST_NONE;
    (void)c;
}
HLE_FUNC("_ZN2nn3ldn8GetStateEv", ldn_get_state) { NTRACE(); RET(L.state); }
HLE_FUNC("_ZN2nn3ldn19GetDisconnectReasonEv", ldn_get_reason) { NTRACE(); RET(L.reason); }

// AttachStateChangeEvent(SystemEventType *): the event fires on every change of the state or the network
HLE_FUNC("_ZN2nn3ldn22AttachStateChangeEventEPNS_2os15SystemEventTypeE", ldn_attach_event) { NTRACE();
    if (!L.ev) L.ev = kernel_create_event_pair(&(uint32_t){0}, &L.ev_r);
    c->r[1] = L.ev_r;
    c->r[2] = 0;  // not managed: the handle stays ours
    c->r[3] = 1;  // auto clear
    guest_call(c, guest_sym("_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE"));
}

HLE_FUNC("_ZN2nn3ldn14GetNetworkInfoEPNS0_11NetworkInfoE", ldn_get_network_info) { NTRACE();
    if (!ldn_net_up()) { RET(R_INVALID_STATE); return; }
    pthread_mutex_lock(&L.mu);
    memcpy(ARGP(0), L.info, NETINFO_SIZE);
    pthread_mutex_unlock(&L.mu);
    RET(0);
}

HLE_FUNC("_ZN2nn3ldn14GetIpv4AddressEPNS0_11Ipv4AddressEPNS0_10SubnetMaskE", ldn_get_ipv4) { NTRACE();
    if (!ldn_net_up()) { RET(R_INVALID_STATE); return; }
    if (ARG(0)) ST32(ARG(0), L.own_ip);
    if (ARG(1)) ST32(ARG(1), 0xFFFFFF00u);
    RET(0);
}

HLE_FUNC("_ZN2nn3ldn20GetSecurityParameterEPNS0_17SecurityParameterE", ldn_get_sec_param) { NTRACE();
    if (!ldn_net_up()) { RET(R_INVALID_STATE); return; }
    memcpy(ARGP(0), L.info + NI_SEC_PARAM, 0x10);
    memcpy((uint8_t *)ARGP(0) + 0x10, L.info + NI_SESSION_ID, 0x10);
    RET(0);
}

HLE_FUNC("_ZN2nn3ldn16GetNetworkConfigEPNS0_13NetworkConfigE", ldn_get_net_config) { NTRACE();
    if (!ldn_net_up()) { RET(R_INVALID_STATE); return; }
    uint8_t *o = ARGP(0);
    memset(o, 0, 0x20);
    memcpy(o, L.info, 0x10);  // IntentId
    wr16(o + 0x10, rd16(L.info + NI_CHANNEL));
    o[0x12] = L.info[NI_NODE_MAX];
    wr16(o + 0x14, L.lcv);
    RET(0);
}

// ---- access point (host)
HLE_FUNC("_ZN2nn3ldn15OpenAccessPointEv", ldn_open_ap) { NTRACE();
    if (L.state != ST_INITIALIZED) { RET(R_INVALID_STATE); return; }
    L.state = ST_ACCESS_POINT;
    signal_change();
    RET(0);
}
HLE_FUNC("_ZN2nn3ldn16CloseAccessPointEv", ldn_close_ap) { NTRACE();
    if (L.state != ST_ACCESS_POINT && L.state != ST_AP_CREATED) { RET(R_INVALID_STATE); return; }
    to_initialized();
    L.state = ST_INITIALIZED;
    signal_change();
    RET(0);
}

static uint32_t create_network(const uint8_t *cfg, const uint8_t *sec, const uint8_t *sec_param, const uint8_t *user) {
    if (L.state != ST_ACCESS_POINT) return R_INVALID_STATE;
    pthread_mutex_lock(&L.mu);
    memset(L.info, 0, NETINFO_SIZE);
    memcpy(L.info, cfg, 0x10);  // IntentId
    if (sec_param) {
        memcpy(L.info + NI_SEC_PARAM, sec_param, 0x10);
        memcpy(L.info + NI_SESSION_ID, sec_param + 0x10, 0x10);
    } else {
        rand_bytes(L.info + NI_SEC_PARAM, 0x10);
        rand_bytes(L.info + NI_SESSION_ID, 0x10);
    }
    memcpy(L.info + NI_BSSID, L.mac, 6);
    char ssid[33];
    snprintf(ssid, sizeof ssid, "MHGURecomp%08x%08x", arc4random(), arc4random());
    L.info[NI_SSID] = (uint8_t)strlen(ssid);
    memcpy(L.info + NI_SSID + 1, ssid, strlen(ssid));
    uint16_t ch = rd16(cfg + 0x10);
    wr16(L.info + NI_CHANNEL, ch ? ch : 6);
    L.info[NI_LINK_LEVEL] = 3;
    L.info[NI_NETWORK_TYPE] = 2;  // ldn
    wr16(L.info + NI_SEC_MODE, rd16(sec));
    L.info[NI_ACCEPT_POLICY] = (uint8_t)L.accept_policy;
    uint8_t max = cfg[0x12];
    L.info[NI_NODE_MAX] = max >= 1 && max <= MAX_NODES ? max : MAX_NODES;
    L.info[NI_NODE_COUNT] = 1;
    L.lcv = rd16(cfg + 0x14);
    L.subnet = 0xA9FE0000u | (uint32_t)(1 + arc4random() % 254) << 8;  // 169.254.x.0/24
    L.own_ip = L.subnet | 1;
    uint8_t *nd = node_at(L.info, 0);
    wr32(nd + NODE_IP, L.own_ip);
    memcpy(nd + NODE_MAC, L.mac, 6);
    nd[NODE_ID] = 0;
    nd[NODE_CONNECTED] = 1;
    memcpy(nd + NODE_NAME, user, 0x20);
    wr16(nd + NODE_LCV, L.lcv);
    wr16(L.info + NI_ADV_SIZE, (uint16_t)L.adv_size);
    memcpy(L.info + NI_ADV, L.adv, L.adv_size);
    uint64_t auth = ((uint64_t)arc4random() << 32) | arc4random();
    memcpy(L.info + NI_AUTH_ID, &auth, 8);
    memset(L.nodes, 0, sizeof L.nodes);
    for (int i = 0; i < MAX_NODES; i++) pthread_mutex_init(&L.nodes[i].wmu, NULL);
    pthread_mutex_unlock(&L.mu);
    host_start();
    L.state = ST_AP_CREATED;
    LOG("[ldn] network created: %s, up to %d players, version %u\n", ipstr(L.own_ip), L.info[NI_NODE_MAX], L.lcv);
    signal_change();
    return 0;
}

// CreateNetwork(const NetworkConfig &, const SecurityConfig &, const UserConfig &)
HLE_FUNC("_ZN2nn3ldn13CreateNetworkERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_10UserConfigE", ldn_create) { NTRACE();
    RET(create_network(ARGP(0), ARGP(1), NULL, ARGP(2)));
}
// CreateNetworkPrivate(config, security, SecurityParameter, user, address entry count, const AddressEntry *)
HLE_FUNC("_ZN2nn3ldn20CreateNetworkPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiPKNS0_12AddressEntryE",
         ldn_create_private) {
    RET(create_network(ARGP(0), ARGP(1), ARGP(2), ARGP(3)));
}

HLE_FUNC("_ZN2nn3ldn14DestroyNetworkEv", ldn_destroy) { NTRACE();
    if (L.state != ST_AP_CREATED) { RET(R_INVALID_STATE); return; }
    host_stop(DR_DESTROYED_BY_USER);
    vsock_network_down();
    L.state = ST_ACCESS_POINT;
    L.own_ip = L.subnet = 0;
    LOG("[ldn] network destroyed\n");
    signal_change();
    RET(0);
}

HLE_FUNC("_ZN2nn3ldn16SetAdvertiseDataEPKvj", ldn_set_advertise) { NTRACE();
    uint32_t n = ARG(1) < ADV_MAX ? ARG(1) : ADV_MAX;
    pthread_mutex_lock(&L.mu);
    memcpy(L.adv, ARGP(0), n);
    L.adv_size = n;
    if (L.state == ST_AP_CREATED) {
        wr16(L.info + NI_ADV_SIZE, (uint16_t)n);
        memset(L.info + NI_ADV, 0, ADV_MAX);
        memcpy(L.info + NI_ADV, L.adv, n);
        push_info_locked(0);
    }
    pthread_mutex_unlock(&L.mu);
    RET(0);
}

HLE_FUNC("_ZN2nn3ldn22SetStationAcceptPolicyENS0_12AcceptPolicyE", ldn_set_accept) { NTRACE();
    pthread_mutex_lock(&L.mu);
    L.accept_policy = (int)ARG(0);
    if (L.state == ST_AP_CREATED) { L.info[NI_ACCEPT_POLICY] = (uint8_t)ARG(0); push_info_locked(0); }
    pthread_mutex_unlock(&L.mu);
    RET(0);
}

// Reject(Ipv4Address): the host sends a player away
HLE_FUNC("_ZN2nn3ldn6RejectENS0_11Ipv4AddressE", ldn_reject) { NTRACE();
    uint32_t r = DR_REJECTED;
    for (int i = 1; i < MAX_NODES; i++)
        if (L.nodes[i].alive && L.nodes[i].ip == ARG(0)) {
            link_send(&L.nodes[i], F_DESTROY, &r, 4, NULL, 0);
            link_close(&L.nodes[i]);
        }
    RET(0);
}

// ---- station (joining)
HLE_FUNC("_ZN2nn3ldn11OpenStationEv", ldn_open_station) { NTRACE();
    if (L.state != ST_INITIALIZED) { RET(R_INVALID_STATE); return; }
    L.state = ST_STATION;
    signal_change();
    RET(0);
}
HLE_FUNC("_ZN2nn3ldn12CloseStationEv", ldn_close_station) { NTRACE();
    if (L.state != ST_STATION && L.state != ST_STATION_CONNECTED && L.state != ST_ERROR) { RET(R_INVALID_STATE); return; }
    to_initialized();
    L.state = ST_INITIALIZED;
    signal_change();
    RET(0);
}

static bool filter_ok(const uint8_t *info, const uint8_t *f) {
    uint32_t flags = rd32(f + 0x5C);
    if ((flags & SF_LOCAL_COMM_ID) && memcmp(info + NI_LOCAL_COMM_ID, f, 8)) return false;
    if ((flags & SF_SCENE_ID) && rd16(info + NI_SCENE_ID) != rd16(f + 0x0A)) return false;
    if ((flags & SF_SESSION_ID) && memcmp(info + NI_SESSION_ID, f + 0x10, 0x10)) return false;
    if ((flags & SF_BSSID) && memcmp(info + NI_BSSID, f + 0x24, 6)) return false;
    return true;
}

// Scan(NetworkInfo *out, int *count, int max, const ScanFilter &, int channel)
HLE_FUNC("_ZN2nn3ldn4ScanEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", ldn_scan) { NTRACE();
    uint32_t out = ARG(0), pcount = ARG(1);
    int max = (int)ARG(2);
    const uint8_t *filter = ARGP(3);
    if (L.state != ST_STATION && L.state != ST_ACCESS_POINT) { RET(R_INVALID_STATE); return; }
    static uint8_t infos[MAX_FOUND][NETINFO_SIZE];
    static LobbyList online;
    struct sockaddr_in addrs[MAX_FOUND];
    uint64_t hubs[MAX_FOUND] = {0};
    pthread_t lt;
    hnet_init();  // (before the thread's first socket call)
    bool asked = lobby_server(online.server) && !pthread_create(&lt, NULL, lobby_list_thread, &online);
    int n = discover(infos, addrs, 16), k = 0;
    if (asked) {  // the server's hubs after the LAN's (a hub found both ways is joined directly)
        pthread_join(lt, NULL);
        for (int j = 0; j < online.n && n < MAX_FOUND; j++) {
            bool dup = false;
            for (int i = 0; i < n; i++) dup |= !memcmp(infos[i] + NI_BSSID, online.infos[j] + NI_BSSID, 6);
            if (dup) continue;
            memcpy(infos[n], online.infos[j], NETINFO_SIZE);
            addrs[n] = online.addr;
            hubs[n++] = online.ids[j];
        }
    }
    nfound = 0;
    for (int i = 0; i < n; i++) {
        if (!info_sane(infos[i]) || !filter_ok(infos[i], filter)) continue;
        memcpy(found[nfound].bssid, infos[i] + NI_BSSID, 6);
        memcpy(found[nfound].session, infos[i] + NI_SESSION_ID, 16);
        found[nfound].hub = hubs[i];
        found[nfound++].addr = addrs[i];
        if (k < max) memcpy((uint8_t *)G2H(out) + (size_t)k++ * NETINFO_SIZE, infos[i], NETINFO_SIZE);
    }
    if (pcount) ST32(pcount, k);
    static int last = -1, last_online = -1, last_max = -1;
    int listed = asked && online.state == HUB_ONLINE ? online.n : 0;
    if (nfound != last || listed != last_online || max != last_max) {
        LOG("[ldn] scan: %d network(s) found (%d listed on the hub server), %d given to the game (room for %d)\n",
            nfound, listed, k, max);
        last = nfound, last_online = listed, last_max = max;
    }
    RET(0);
}

static uint32_t connect_network(const uint8_t *info, const uint8_t *user, uint16_t lcv) {
    if (L.state != ST_STATION) return R_INVALID_STATE;
    int i = 0;
    while (i < nfound && memcmp(found[i].bssid, info + NI_BSSID, 6)) i++;
    if (i == nfound) return R_CONNECT_NOT_FOUND;
    hsock s;
    uint32_t pair = 0;
    uint64_t secret = 0;
    if (found[i].hub) {
        uint32_t r = lobby_join(&found[i].addr, found[i].hub, &s, &pair, &secret);
        if (r) return r;
    } else if ((s = connect_to(&found[i].addr, 3000)) == BADSOCK) {
        LOG("[ldn] can't reach the host at %s\n", inet_ntoa(found[i].addr.sin_addr));
        return R_CONNECT_TIMEOUT;
    }
    L.lcv = lcv;
    Hello h = make_hello(false, user);
    static uint8_t buf[MAX_FRAME];
    uint8_t type;
    int n;
    hset_timeout(s, 5000);
    if (!frame_send(s, F_HELLO, &h, sizeof h, NULL, 0) || (n = frame_recv(s, &type, buf)) < 4) {
        hclose(s);
        return R_CONNECT_TIMEOUT;
    }
    if (type == F_REFUSE) {
        hclose(s);
        uint32_t r = rd32(buf);
        LOG("[ldn] the host refused (result %#x)\n", r);
        return (r & 0x1FF) == 203 ? r : R_CONNECT_REJECTED;  // (an nn::ldn result only)
    }
    int node = buf[0];
    if (type != F_WELCOME || n < 8 + NETINFO_SIZE || node < 1 || node >= MAX_NODES || !info_sane(buf + 8) ||
        !node_at(buf + 8, node)[NODE_CONNECTED]) {
        hclose(s);
        return R_CONNECT_FAILURE;
    }
    hset_timeout(s, 0);
    pthread_mutex_lock(&L.mu);
    memcpy(L.info, buf + 8, NETINFO_SIZE);
    L.own_ip = rd32(node_at(L.info, node) + NODE_IP);
    L.subnet = L.own_ip & 0xFFFFFF00u;
    pthread_mutex_init(&L.up.wmu, NULL);
    L.up.s = s, L.up.alive = true, L.up.ip = L.subnet | 1;
    L.up.token = rd32(buf + 4), L.up.udp_up = false, L.up.udp_rx_ns = 0;
    L.up.relay_pair = pair, L.up.relay_secret = secret;
    L.state = ST_STATION_CONNECTED, L.reason = DR_NONE;
    pthread_mutex_unlock(&L.mu);
    L.up.has_thread = pthread_create(&L.up.th, NULL, uplink_thread, NULL) == 0;
    // UDP to the same address and port as the TCP connection (the host's hello answers tell whether it gets through)
    L.us = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (L.us != BADSOCK && !connect(L.us, (const struct sockaddr *)&found[i].addr, sizeof found[i].addr)) {
        L.uth_run = true;
        if (pthread_create(&L.uth, NULL, station_udp_thread, NULL)) { hclose(L.us); L.us = BADSOCK; }
    } else if (L.us != BADSOCK) {
        hclose(L.us);
        L.us = BADSOCK;
    }
    if (pair) LOG("[ldn] joined a hub through the hub server as %s (node %d)\n", ipstr(L.own_ip), node);
    else LOG("[ldn] joined %s's network as %s (node %d)\n", inet_ntoa(found[i].addr.sin_addr), ipstr(L.own_ip), node);
    signal_change();
    return 0;
}

// Connect(const NetworkInfo &, const SecurityConfig &, const UserConfig &, int version, ConnectOption)
HLE_FUNC("_ZN2nn3ldn7ConnectERKNS0_11NetworkInfoERKNS0_14SecurityConfigERKNS0_10UserConfigEiNS0_13ConnectOptionE", ldn_connect) { NTRACE();
    RET(connect_network(ARGP(0), ARGP(2), (uint16_t)ARG(3)));
}
HLE_FUNC("_ZN2nn3ldn7ConnectERKNS0_11NetworkInfoERKNS0_14SecurityConfigERKNS0_10UserConfigEi", ldn_connect2) { NTRACE();
    RET(connect_network(ARGP(0), ARGP(2), (uint16_t)ARG(3)));
}
// ConnectPrivate(config, security, SecurityParameter, user, int version, ConnectOption): a network known by its
// session id (from the scan list)
static uint32_t connect_private(Cpu *c) {
    const uint8_t *session = (const uint8_t *)ARGP(2) + 0x10;
    for (int i = 0; i < nfound; i++)
        if (!memcmp(found[i].session, session, 0x10)) {
            uint8_t info[NETINFO_SIZE] = {0};
            memcpy(info + NI_BSSID, found[i].bssid, 6);
            return connect_network(info, ARGP(3), (uint16_t)STK(0));
        }
    LOG("[ldn] ConnectPrivate: no scanned network has that session\n");
    return R_CONNECT_NOT_FOUND;
}
HLE_FUNC("_ZN2nn3ldn14ConnectPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiNS0_13ConnectOptionE",
         ldn_connect_private) {
    RET(connect_private(c));
}

HLE_FUNC("_ZN2nn3ldn10DisconnectEv", ldn_disconnect) { NTRACE();
    if (L.state != ST_STATION_CONNECTED && L.state != ST_ERROR) { RET(R_INVALID_STATE); return; }
    station_leave();
    L.state = ST_STATION, L.reason = DR_BY_USER;
    L.own_ip = L.subnet = 0;
    signal_change();
    RET(0);
}
