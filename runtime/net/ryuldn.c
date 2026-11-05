// RyuLdn client (ryuldn.h). Wire format: header {u32 'RLDN', u8 type, u8 version = 1, 2 bytes padding, s32 payload
// size}, then the payload, little-endian, in the C# servers' sequential struct layout. The offsets below were
// measured from LdnServer's types. The Nintendo structs inside (NetworkInfo 0x480, NetworkConfig 0x20, SecurityConfig
// 0x44, UserConfig 0x30, ScanFilter 0x60) are the SDK's, as in ldn.c.
//
// Protocol and behaviour: Ryujinx's LdnServer and LDN client (MIT, Copyright (c) Ryujinx Team and Contributors; see
// THIRD_PARTY_NOTICES). Written from the protocol, not copied.
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

#include "../config.h"
#include "../hle.h"
#include "ryuldn.h"

#ifdef _WIN32
typedef SOCKET rsock;
#define RBAD INVALID_SOCKET
#define rclose closesocket
static void rnet_init(void) {
    static int done;
    if (!done) { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); done = 1; }
}
static void rnonblock(rsock s, bool on) { u_long v = on; ioctlsocket(s, FIONBIO, &v); }
static void rshutdown(rsock s) { shutdown(s, SD_BOTH); }
#else
typedef int rsock;
#define RBAD (-1)
#define rclose close
static void rnet_init(void) {}
static void rnonblock(rsock s, bool on) {
    int f = fcntl(s, F_GETFL);
    fcntl(s, F_SETFL, on ? f | O_NONBLOCK : f & ~O_NONBLOCK);
}
static void rshutdown(rsock s) { shutdown(s, SHUT_RDWR); }
#endif

enum {
    P_INITIALIZE = 0, P_PASSPHRASE, P_CREATE, P_CREATE_PRIVATE, P_EXTERNAL_PROXY, P_EXTERNAL_PROXY_TOKEN,
    P_EXTERNAL_PROXY_STATE, P_SYNC_NETWORK, P_REJECT, P_REJECT_REPLY, P_SCAN, P_SCAN_REPLY, P_SCAN_REPLY_END,
    P_CONNECT, P_CONNECT_PRIVATE, P_CONNECTED, P_DISCONNECT, P_PROXY_CONFIG, P_PROXY_CONNECT, P_PROXY_CONNECT_REPLY,
    P_PROXY_DATA, P_PROXY_DISCONNECT, P_SET_ACCEPT_POLICY, P_SET_ADVERTISE, P_PING = 254, P_NETWORK_ERROR = 255,
};
enum { E_NONE = 0, E_PORT_UNREACHABLE, E_TOO_MANY_PLAYERS, E_VERSION_TOO_LOW, E_VERSION_TOO_HIGH, E_CONNECT_FAILURE,
       E_CONNECT_NOT_FOUND, E_CONNECT_TIMEOUT, E_CONNECT_REJECTED, E_REJECT_FAILED, E_BANNED = 127 };

#define HDR 12
#define MAGIC 0x4E444C52u  // 'R' 'L' 'D' 'N'
#define MAX_PACKET 131072
#define MAX_PAYLOAD (MAX_PACKET - HDR - 1)
#define SEC_SIZE 0x44
#define USER_SIZE 0x30
#define NETCFG_SIZE 0x20
#define RYUCFG_SIZE 0x28
#define CREATE_SIZE 0xBC       // security 0, user 0x44, network 0x74, ryu 0x94, then the advertise data
#define CONNECT_SIZE 0x500     // security 0, user 0x44, lcv u32 0x74, option u32 0x78, pad, NetworkInfo 0x80
#define PROXYINFO 0x10         // src ip u32, src port u16, dst ip u32, dst port u16, proto u32 (packed)
#define ADV_MAX 0x180
#define CONNECT_MS 4000
#define SCAN_MS 4000
#define REJECT_MS 4000
#define LDN_RESULT(d) ((uint32_t)(((203) & 0x1FF) | ((d) << 9)))
#define R_CONNECT_FAILURE LDN_RESULT(64)
#define R_CONNECT_NOT_FOUND LDN_RESULT(65)
#define R_CONNECT_TIMEOUT LDN_RESULT(66)
#define R_CONNECT_REJECTED LDN_RESULT(67)
#define R_VERSION_TOO_LOW LDN_RESULT(113)
#define R_VERSION_TOO_HIGH LDN_RESULT(114)
#define R_TOO_MANY_PLAYERS LDN_RESULT(144)
#define R_ACCESS_POINT_FAILED LDN_RESULT(48)
#define GAME_VERSION "1.4.0"
#define DEFAULT_PORT 30456

static void put16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static uint16_t get16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t get32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

typedef struct { char host[128]; int port; } Server;
static Server g_srv[RYU_MAX_SERVERS];
static int g_nsrv = -1;
static uint8_t g_ident[RYU_MAX_SERVERS + 1][22];  // each server's id (16) and MAC (6) for this run, sent again on reconnects
static RyuHooks g_hooks;

void ryu_set_hooks(const RyuHooks *h) { g_hooks = *h; }

static bool parse_server(const char *t, Server *s) {
    while (*t == ' ') t++;
    if (!*t) return false;
    memset(s, 0, sizeof *s);
    s->port = DEFAULT_PORT;
    const char *colon = strrchr(t, ':');
    size_t n = colon && strchr(t, ':') == colon ? (size_t)(colon - t) : strlen(t);
    if (n == 0 || n >= sizeof s->host) return false;
    memcpy(s->host, t, n);
    while (n && s->host[n - 1] == ' ') s->host[--n] = 0;
    if (colon && atoi(colon + 1) > 0 && atoi(colon + 1) < 65536) s->port = atoi(colon + 1);
    return n > 0;
}

static void load_servers(void) {
    if (g_nsrv >= 0) return;
    g_nsrv = 0;
    char list[sizeof g_config.ldn_ryu_servers];
    snprintf(list, sizeof list, "%s", g_config.ldn_ryu_servers);
    for (char *t = strtok(list, ",;"); t && g_nsrv < RYU_MAX_SERVERS; t = strtok(NULL, ",;"))
        if (parse_server(t, &g_srv[g_nsrv])) g_nsrv++;
}
int ryu_server_count(void) { load_servers(); return g_nsrv; }
static char g_self[40];  // the server this game runs itself (ryusrv.h), when it does
void ryu_set_self(const char *addr) { snprintf(g_self, sizeof g_self, "%s", addr ? addr : ""); }
// The server a hub we create is made on: ini RyuHost, else our own server.
const char *ryu_host_addr(void) { return g_config.ldn_ryu_host[0] ? g_config.ldn_ryu_host : g_self; }
bool ryu_enabled(void) { return ryu_server_count() > 0 || ryu_host_addr()[0]; }

// ---- sockets
static bool send_all(rsock s, const void *b, size_t n) {
    const char *p = b;
    while (n) {
        int k = send(s, p, (int)(n > 0x10000 ? 0x10000 : n), 0);
        if (k <= 0) return false;
        p += k, n -= (size_t)k;
    }
    return true;
}
static bool recv_all(rsock s, void *b, size_t n) {
    char *p = b;
    while (n) {
        int k = recv(s, p, (int)(n > 0x10000 ? 0x10000 : n), 0);
        if (k <= 0) return false;
        p += k, n -= (size_t)k;
    }
    return true;
}

static rsock connect_to(const char *host, int port, int timeout_ms) {
    rnet_init();
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char ps[8];
    snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) || !res) return RBAD;
    rsock s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == RBAD) { freeaddrinfo(res); return RBAD; }
    rnonblock(s, true);
    int r = connect(s, res->ai_addr, (int)res->ai_addrlen);
    freeaddrinfo(res);
    if (r != 0) {
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
            rclose(s);
            return RBAD;
        }
    }
    rnonblock(s, false);
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    return s;
}

// ---- one connection (to a server, or to a host's own proxy)
typedef struct Conn {
    rsock s;
    pthread_t th;
    bool has_th;
    pthread_mutex_t mu, wmu;  // mu: the state below; wmu: one writer at a time
    volatile bool alive;      // the connection is up (cleared by the reader or by close)
    bool p2p;                 // a host's proxy: only ProxyConfig and ProxyData
    int srv;
    // answers
    bool scan_done, join_done, reject_done;
    int join_err, net_err;
    uint8_t (*scan)[RYU_NETINFO];
    int nscan, scan_max;
    bool have_proxy;
    uint32_t proxy_ip, proxy_mask;
    uint8_t info[RYU_NETINFO];
    bool have_info;
    bool deliver;  // pass lobby changes and game data on (false: a scan's short connection)
    struct Conn *parent;  // p2p: the server connection it belongs to
    // a peer: one more client of the server for a player who is not in this process (xbridge.c), with its own hooks
    bool peer;
    volatile bool in_lobby;
    void *ud;
    RyuPeerHooks ph;
} Conn;
#define CONN_IN_LOBBY(c) ((c)->peer ? (c)->in_lobby : g_in_lobby)

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static Conn *g_main;       // the server connection of the lobby we are in
static Conn *g_p2p;        // the host's proxy, if it offered one
static volatile bool g_in_lobby;
static bool g_p2p_expected, g_p2p_ready;

static bool conn_send(Conn *c, uint8_t type, const void *a, uint32_t alen, const void *b, uint32_t blen) {
    if (!c || !c->alive || (uint64_t)alen + blen > MAX_PAYLOAD) return false;
    uint8_t h[HDR] = {0};
    put32(h, MAGIC);
    h[4] = type, h[5] = 1;
    put32(h + 8, alen + blen);
    pthread_mutex_lock(&c->wmu);
    bool ok = send_all(c->s, h, HDR) && (!alen || send_all(c->s, a, alen)) && (!blen || send_all(c->s, b, blen));
    pthread_mutex_unlock(&c->wmu);
    return ok;
}

static void handle(Conn *c, uint8_t type, const uint8_t *d, uint32_t n);

static void *reader(void *arg) {
    Conn *c = arg;
    static __thread uint8_t buf[MAX_PACKET];
    uint8_t h[HDR];
    while (recv_all(c->s, h, HDR)) {
        int32_t size;
        memcpy(&size, h + 8, 4);
        if (get32(h) != MAGIC || h[5] != 1 || size < 0 || (uint32_t)size > MAX_PAYLOAD) break;
        if (size && !recv_all(c->s, buf, (size_t)size)) break;
        handle(c, h[4], buf, (uint32_t)size);
    }
    pthread_mutex_lock(&c->mu);
    bool was = c->alive;
    c->alive = false;
    pthread_mutex_unlock(&c->mu);
    if (was && c->deliver && c->peer) {
        bool inl = c->in_lobby;
        c->in_lobby = false;
        if (inl && c->ph.lobby) c->ph.lobby(c->ud, NULL);
    } else if (was && c->deliver) {
        // The connection broke while we were in a lobby through it.
        bool ours;
        pthread_mutex_lock(&g_mu);
        ours = c == g_main || c == g_p2p;
        if (ours && c == g_p2p && g_main && g_in_lobby) conn_send(g_main, P_DISCONNECT, (uint8_t[4]){0}, 4, NULL, 0);
        if (ours && g_in_lobby) g_in_lobby = false;
        else ours = false;
        pthread_mutex_unlock(&g_mu);
        if (ours) {
            LOG("[ryuldn] the connection to the lobby was lost\n");
            if (g_hooks.lobby) g_hooks.lobby(NULL);
        }
    }
    return NULL;
}

static Conn *conn_open(const Server *sv, int srv_index, bool deliver, bool handshake) {
    rsock s = connect_to(sv->host, sv->port, CONNECT_MS);
    if (s == RBAD) {
        LOG("[ryuldn] can't reach %s:%d\n", sv->host, sv->port);
        return NULL;
    }
    Conn *c = calloc(1, sizeof *c);
    if (!c) { rclose(s); return NULL; }
    c->s = s, c->srv = srv_index, c->deliver = deliver, c->alive = true;
    pthread_mutex_init(&c->mu, NULL);
    pthread_mutex_init(&c->wmu, NULL);
    c->has_th = pthread_create(&c->th, NULL, reader, c) == 0;
    if (!c->has_th) { rclose(s); free(c); return NULL; }
    if (!handshake) return c;  // (a host's own proxy wants the token first)
    uint8_t id[22];
    memcpy(id, g_ident[srv_index < 0 ? RYU_MAX_SERVERS : srv_index], sizeof id);  // (zeros on first contact)
    conn_send(c, P_INITIALIZE, id, sizeof id, NULL, 0);
    uint8_t pass[0x80] = {0};
    const char *ps = g_config.ldn_ryu_pass;
    // "" (public) or "Ryujinx-" + 8 hex digits; anything else counts as empty, as in Ryujinx
    bool ok = !strncmp(ps, "Ryujinx-", 8) && strlen(ps) == 16;
    for (int i = 8; ok && i < 16; i++) ok = (ps[i] >= '0' && ps[i] <= '9') || (ps[i] >= 'a' && ps[i] <= 'f') || (ps[i] >= 'A' && ps[i] <= 'F');
    if (ok) memcpy(pass, ps, 16);
    conn_send(c, P_PASSPHRASE, pass, sizeof pass, NULL, 0);
    return c;
}

static void conn_close(Conn *c) {
    if (!c) return;
    pthread_mutex_lock(&c->mu);
    c->deliver = false;  // (the reader stays quiet)
    pthread_mutex_unlock(&c->mu);
    rshutdown(c->s);
    if (c->has_th) pthread_join(c->th, NULL);
    c->alive = false;
    rclose(c->s);
    free(c);
}

static bool wait_for(Conn *c, volatile bool *flag, int ms) {
    uint64_t end = plat_now_ns() + (uint64_t)ms * 1000000ull;
    while (!*flag && c->alive && plat_now_ns() < end) plat_sleep_until_ns(plat_now_ns() + 5000000);
    return *flag;
}

// ---- packets
static void p2p_start(Conn *main, const uint8_t *d, uint32_t n) {
    // ExternalProxyConfig (0x26): proxy ip[16], u32 address family, u16 port, token[16]. .NET InterNetwork = 2.
    if (n < 0x26 || get32(d + 16) != 2) {
        LOG("[ryuldn] the host's proxy needs an address family this client doesn't speak\n");
        pthread_mutex_lock(&main->mu);
        main->join_err = E_CONNECT_FAILURE;
        pthread_mutex_unlock(&main->mu);
        return;
    }
    char ip[20];
    snprintf(ip, sizeof ip, "%u.%u.%u.%u", d[0], d[1], d[2], d[3]);
    Server sv = {0};
    snprintf(sv.host, sizeof sv.host, "%s", ip);
    sv.port = get16(d + 20);
    pthread_mutex_lock(&g_mu);
    g_p2p_expected = true, g_p2p_ready = false;
    pthread_mutex_unlock(&g_mu);
    Conn *p = conn_open(&sv, -1, true, false);
    if (!p) {
        LOG("[ryuldn] can't reach the host's own proxy at %s:%d\n", ip, sv.port);
        pthread_mutex_lock(&main->mu);
        main->join_err = E_CONNECT_FAILURE;
        pthread_mutex_unlock(&main->mu);
        return;
    }
    p->p2p = true, p->parent = main;
    conn_send(p, P_EXTERNAL_PROXY, d, 0x26, NULL, 0);
    LOG("[ryuldn] the host runs its own proxy (%s:%d): connected to it\n", ip, sv.port);
    pthread_mutex_lock(&g_mu);
    g_p2p = p;
    pthread_mutex_unlock(&g_mu);
}

static void got_data(Conn *c, const uint8_t *d, uint32_t n) {
    // ProxyDataHeader (0x14): ProxyInfo, u32 length, then the bytes
    if (n < PROXYINFO + 4 || (c->peer ? !c->ph.data : !g_hooks.data)) return;
    uint32_t len = get32(d + PROXYINFO);
    if (len != n - PROXYINFO - 4) return;
    RyuProxy p;
    p.src_ip = get32(d), p.src_port = get16(d + 4), p.dst_ip = get32(d + 6), p.dst_port = get16(d + 10);
    p.proto = get32(d + 12);
    if (c->peer) c->ph.data(c->ud, &p, d + PROXYINFO + 4, len);
    else g_hooks.data(&p, d + PROXYINFO + 4, len);
}

static void handle(Conn *c, uint8_t type, const uint8_t *d, uint32_t n) {
    switch (type) {
    case P_INITIALIZE:
        if (n >= 22 && c->srv >= -1 && !c->p2p) memcpy(g_ident[c->srv < 0 ? RYU_MAX_SERVERS : c->srv], d, 22);
        break;
    case P_PING:
        if (n >= 2 && d[0] == 0) conn_send(c, P_PING, d, 2, NULL, 0);  // the server checks that we're alive: echo it
        break;
    case P_NETWORK_ERROR:
        if (n >= 4) {
            int e = (int)get32(d);
            if (e == E_PORT_UNREACHABLE) break;  // about a proxy of ours that doesn't exist
            LOG("[ryuldn] the server answered with error %d\n", e);
            pthread_mutex_lock(&c->mu);
            c->net_err = e, c->join_err = e;
            pthread_mutex_unlock(&c->mu);
        }
        break;
    case P_PROXY_CONFIG:
        if (n >= 8) {
            pthread_mutex_lock(&c->mu);
            c->have_proxy = true, c->proxy_ip = get32(d), c->proxy_mask = get32(d + 4);
            pthread_mutex_unlock(&c->mu);
            if (c->p2p) {
                pthread_mutex_lock(&g_mu);
                g_p2p_ready = true;
                pthread_mutex_unlock(&g_mu);
                if (c->parent) {
                    pthread_mutex_lock(&c->parent->mu);
                    c->parent->have_proxy = true, c->parent->proxy_ip = get32(d), c->parent->proxy_mask = get32(d + 4);
                    pthread_mutex_unlock(&c->parent->mu);
                }
            }
        }
        break;
    case P_CONNECTED:
        if (n >= RYU_NETINFO) {
            pthread_mutex_lock(&c->mu);
            memcpy(c->info, d, RYU_NETINFO);
            c->have_info = true, c->join_done = true;
            pthread_mutex_unlock(&c->mu);
            if (c->deliver && CONN_IN_LOBBY(c)) {
                if (c->peer) { if (c->ph.lobby) c->ph.lobby(c->ud, d); }
                else if (g_hooks.lobby) g_hooks.lobby(d);
            }
        }
        break;
    case P_SYNC_NETWORK:
        if (n >= RYU_NETINFO && c->deliver) {
            pthread_mutex_lock(&c->mu);
            memcpy(c->info, d, RYU_NETINFO);
            c->have_info = true;
            pthread_mutex_unlock(&c->mu);
            if (CONN_IN_LOBBY(c)) {
                if (c->peer) { if (c->ph.lobby) c->ph.lobby(c->ud, d); }
                else if (g_hooks.lobby) g_hooks.lobby(d);
            }
        }
        break;
    case P_DISCONNECT:
        if (c->deliver && c->peer) {
            bool was = c->in_lobby;
            c->in_lobby = false;
            if (was && c->ph.lobby) c->ph.lobby(c->ud, NULL);
        } else if (c->deliver) {
            bool was;
            pthread_mutex_lock(&g_mu);
            was = g_in_lobby;
            g_in_lobby = false;
            pthread_mutex_unlock(&g_mu);
            if (was) {
                LOG("[ryuldn] the lobby was closed\n");
                if (g_hooks.lobby) g_hooks.lobby(NULL);
            }
        }
        break;
    case P_SCAN_REPLY:
        pthread_mutex_lock(&c->mu);
        if (n >= RYU_NETINFO && c->scan && c->nscan < c->scan_max) memcpy(c->scan[c->nscan++], d, RYU_NETINFO);
        pthread_mutex_unlock(&c->mu);
        break;
    case P_SCAN_REPLY_END:
        c->scan_done = true;
        break;
    case P_REJECT_REPLY:
        c->reject_done = true;
        break;
    case P_PROXY_DATA:
        if (c->deliver) got_data(c, d, n);
        break;
    case P_EXTERNAL_PROXY:
        if (!c->p2p && c->deliver) p2p_start(c, d, n);
        break;
    default:
        break;  // proxy TCP messages, external proxy tokens: not used
    }
}

// ---- scan
// The game's thread must not wait for slow or dead servers: a worker thread asks them and the scan returns what the last
// round found (the game scans again and again). The very first scan waits a moment for the first round.
#define SCAN_MAX 64
static struct {
    pthread_mutex_t mu;
    bool running;
    uint64_t started, ok_at;
    uint8_t filter[0x60];
    uint8_t (*infos)[RYU_NETINFO];  // the last round
    int server_of[SCAN_MAX], n;
    uint64_t down_until[RYU_MAX_SERVERS];  // a server that didn't answer is left alone for a while
} S = {.mu = PTHREAD_MUTEX_INITIALIZER};

static void *scan_worker(void *arg) {
    (void)arg;
    static uint8_t stage[SCAN_MAX][RYU_NETINFO];
    int server_of[SCAN_MAX], total = 0;
    uint8_t filter[0x60];
    pthread_mutex_lock(&S.mu);
    memcpy(filter, S.filter, sizeof filter);
    pthread_mutex_unlock(&S.mu);
    for (int i = 0; i < g_nsrv && total < SCAN_MAX; i++) {
        if (plat_now_ns() < S.down_until[i]) continue;
        Conn *c = conn_open(&g_srv[i], i, false, true);
        if (!c) { S.down_until[i] = plat_now_ns() + 10000000000ull; continue; }
        c->scan = stage + total, c->scan_max = SCAN_MAX - total, c->nscan = 0, c->scan_done = false;
        bool ok = conn_send(c, P_SCAN, filter, 0x60, NULL, 0) && wait_for(c, &c->scan_done, SCAN_MS);
        pthread_mutex_lock(&c->mu);
        int k = c->nscan;
        c->scan = NULL;
        pthread_mutex_unlock(&c->mu);
        for (int j = 0; j < k; j++) server_of[total + j] = i;
        total += k;
        if (!ok) S.down_until[i] = plat_now_ns() + 10000000000ull;
        conn_close(c);
    }
    pthread_mutex_lock(&S.mu);
    if (!S.infos) S.infos = malloc(sizeof stage);
    if (S.infos) {
        memcpy(S.infos, stage, (size_t)total * RYU_NETINFO);
        memcpy(S.server_of, server_of, (size_t)total * sizeof(int));
        S.n = total;
    }
    S.ok_at = plat_now_ns();
    S.running = false;
    pthread_mutex_unlock(&S.mu);
    return NULL;
}

int ryu_scan(const uint8_t *filter, uint8_t (*infos)[RYU_NETINFO], int *server_of, int max) {
    load_servers();
    if (!g_nsrv) return 0;
    uint64_t now = plat_now_ns();
    pthread_mutex_lock(&S.mu);
    bool first = S.ok_at == 0 && !S.running;
    if (!S.running && now - S.started > 2000000000ull) {
        pthread_t t;
        memcpy(S.filter, filter, sizeof S.filter);
        S.running = true, S.started = now;
        if (pthread_create(&t, NULL, scan_worker, NULL)) S.running = false;
        else pthread_detach(t);
    }
    pthread_mutex_unlock(&S.mu);
    if (first) {  // (nothing known yet)
        for (int w = 0; w < 300; w++) {
            pthread_mutex_lock(&S.mu);
            bool done = !S.running;
            pthread_mutex_unlock(&S.mu);
            if (done) break;
            plat_sleep_until_ns(plat_now_ns() + 5000000);
        }
    }
    pthread_mutex_lock(&S.mu);
    int n = S.infos ? (S.n < max ? S.n : max) : 0;
    for (int i = 0; i < n; i++) memcpy(infos[i], S.infos[i], RYU_NETINFO), server_of[i] = S.server_of[i];
    pthread_mutex_unlock(&S.mu);
    return n;
}

// ---- joining and hosting
static uint32_t wait_joined(Conn *c, uint32_t *ip, uint32_t *mask, uint8_t *info_out) {
    uint64_t end = plat_now_ns() + 2ull * CONNECT_MS * 1000000ull;
    int err = E_CONNECT_TIMEOUT;
    for (;;) {
        pthread_mutex_lock(&c->mu);
        bool done = c->join_done, bad = c->join_err != E_NONE, proxy = c->have_proxy;
        err = c->join_err;
        pthread_mutex_unlock(&c->mu);
        pthread_mutex_lock(&g_mu);
        bool p2p_wait = g_p2p_expected && !g_p2p_ready;
        pthread_mutex_unlock(&g_mu);
        if (bad) break;
        if (done && proxy && !p2p_wait) { err = E_NONE; break; }
        if (!c->alive) { err = E_CONNECT_FAILURE; break; }
        if (plat_now_ns() > end) { err = done ? E_CONNECT_FAILURE : E_CONNECT_TIMEOUT; break; }
        plat_sleep_until_ns(plat_now_ns() + 5000000);
    }
    if (err == E_NONE) {
        pthread_mutex_lock(&c->mu);
        *ip = c->proxy_ip, *mask = c->proxy_mask;
        memcpy(info_out, c->info, RYU_NETINFO);
        pthread_mutex_unlock(&c->mu);
        return 0;
    }
    switch (err) {
    case E_VERSION_TOO_LOW: return R_VERSION_TOO_LOW;
    case E_VERSION_TOO_HIGH: return R_VERSION_TOO_HIGH;
    case E_TOO_MANY_PLAYERS: return R_TOO_MANY_PLAYERS;
    case E_CONNECT_NOT_FOUND: return R_CONNECT_NOT_FOUND;
    case E_CONNECT_TIMEOUT: return R_CONNECT_TIMEOUT;
    case E_CONNECT_REJECTED: return R_CONNECT_REJECTED;
    default: return R_CONNECT_FAILURE;
    }
}

static Conn *begin(int server, bool host, uint32_t *fail) {
    ryu_leave();
    load_servers();
    Server sv;
    int idx = server;
    if (host && ryu_host_addr()[0] && server < 0) {
        if (!parse_server(ryu_host_addr(), &sv)) { *fail = R_CONNECT_FAILURE; return NULL; }
        idx = -1;
    } else if (server >= 0 && server < g_nsrv) {
        sv = g_srv[server];
    } else {
        *fail = R_CONNECT_NOT_FOUND;
        return NULL;
    }
    Conn *c = conn_open(&sv, idx, true, true);
    if (!c) { *fail = R_CONNECT_TIMEOUT; return NULL; }
    pthread_mutex_lock(&g_mu);
    g_main = c, g_in_lobby = true, g_p2p_expected = g_p2p_ready = false;
    pthread_mutex_unlock(&g_mu);
    return c;
}

static void fail_lobby(void) {
    pthread_mutex_lock(&g_mu);
    g_in_lobby = false;
    pthread_mutex_unlock(&g_mu);
    ryu_leave();
}

uint32_t ryu_create(int server, const uint8_t *net_cfg, const uint8_t *sec_cfg, const uint8_t *user, const void *adv,
                    uint32_t adv_size, uint8_t *info, uint32_t *ip, uint32_t *mask) {
    uint32_t fail = 0;
    Conn *c = begin(server, true, &fail);
    if (!c) return fail;
    uint8_t req[CREATE_SIZE + ADV_MAX] = {0};
    memcpy(req, sec_cfg, SEC_SIZE);
    memcpy(req + SEC_SIZE, user, USER_SIZE);
    memcpy(req + 0x74, net_cfg, NETCFG_SIZE);
    memcpy(req + 0x94, GAME_VERSION, sizeof GAME_VERSION);  // RyuNetworkConfig: game version, no P2P proxy of ours
    if (adv_size > ADV_MAX) adv_size = ADV_MAX;
    memcpy(req + CREATE_SIZE, adv, adv_size);
    conn_send(c, P_CREATE, req, CREATE_SIZE, req + CREATE_SIZE, adv_size);
    uint32_t r = wait_joined(c, ip, mask, info);
    if (r) { fail_lobby(); return r == R_CONNECT_FAILURE ? R_ACCESS_POINT_FAILED : r; }
    LOG("[ryuldn] lobby created on %s as %u.%u.%u.%u\n", ryu_host_addr()[0] && server < 0 ? ryu_host_addr() : "the server",
        *ip >> 24, (*ip >> 16) & 255, (*ip >> 8) & 255, *ip & 255);
    return 0;
}

uint32_t ryu_connect(int server, const uint8_t *netinfo, const uint8_t *sec_cfg, const uint8_t *user, uint16_t lcv,
                     uint8_t *info_out, uint32_t *ip, uint32_t *mask) {
    uint32_t fail = 0;
    Conn *c = begin(server, false, &fail);
    if (!c) return fail;
    uint8_t req[CONNECT_SIZE] = {0};
    memcpy(req, sec_cfg, SEC_SIZE);
    memcpy(req + SEC_SIZE, user, USER_SIZE);
    put32(req + 0x74, lcv);
    memcpy(req + 0x80, netinfo, RYU_NETINFO);
    conn_send(c, P_CONNECT, req, CONNECT_SIZE, NULL, 0);
    uint32_t r = wait_joined(c, ip, mask, info_out);
    if (r) { fail_lobby(); return r; }
    LOG("[ryuldn] joined a lobby as %u.%u.%u.%u\n", *ip >> 24, (*ip >> 16) & 255, (*ip >> 8) & 255, *ip & 255);
    return 0;
}

void ryu_leave(void) {
    Conn *m, *p;
    pthread_mutex_lock(&g_mu);
    m = g_main, p = g_p2p;
    g_main = g_p2p = NULL;
    bool was = g_in_lobby;
    g_in_lobby = false;
    pthread_mutex_unlock(&g_mu);
    if (was && m) conn_send(m, P_DISCONNECT, (uint8_t[4]){0}, 4, NULL, 0);
    conn_close(p);
    conn_close(m);
}

bool ryu_in_lobby(void) { return g_in_lobby; }

void ryu_set_advertise(const void *data, uint32_t len) {
    if (g_in_lobby && g_main) conn_send(g_main, P_SET_ADVERTISE, data, len > ADV_MAX ? ADV_MAX : len, NULL, 0);
}
void ryu_set_accept_policy(uint8_t policy) {
    if (g_in_lobby && g_main) conn_send(g_main, P_SET_ACCEPT_POLICY, &policy, 1, NULL, 0);
}

bool ryu_reject(uint32_t node_id, uint32_t reason) {
    Conn *m = g_main;
    if (!g_in_lobby || !m) return false;
    uint8_t r[8];
    put32(r, node_id), put32(r + 4, reason);
    m->reject_done = false, m->net_err = E_NONE;
    conn_send(m, P_REJECT, r, 8, NULL, 0);
    wait_for(m, &m->reject_done, REJECT_MS);
    return m->reject_done && m->net_err == E_NONE;
}

void ryu_send(const RyuProxy *p, const void *data, uint32_t len) {
    if (!g_in_lobby || PROXYINFO + 4 + len > MAX_PAYLOAD) return;
    uint8_t h[PROXYINFO + 4];
    put32(h, p->src_ip), put16(h + 4, p->src_port), put32(h + 6, p->dst_ip), put16(h + 10, p->dst_port);
    put32(h + 12, p->proto);
    put32(h + PROXYINFO, len);
    Conn *c = g_p2p ? g_p2p : g_main;  // the host's own proxy relays when it offered one
    conn_send(c, P_PROXY_DATA, h, sizeof h, data, len);
}

// ---- peers: further clients of the server we host on, for players in a suyu room (xbridge.c)
RyuPeer *ryu_peer_join(const uint8_t *netinfo, const uint8_t *sec_cfg, const uint8_t *user, uint16_t lcv, const RyuPeerHooks *hooks,
                       void *ud, uint8_t *info_out, uint32_t *ip, uint32_t *mask, uint32_t *result) {
    Server sv;
    if (!ryu_host_addr()[0] || !parse_server(ryu_host_addr(), &sv)) { *result = R_CONNECT_FAILURE; return NULL; }
    Conn *c = conn_open(&sv, -1, false, true);
    if (!c) { *result = R_CONNECT_TIMEOUT; return NULL; }
    pthread_mutex_lock(&c->mu);
    c->peer = true, c->ud = ud, c->ph = *hooks, c->in_lobby = true, c->deliver = true;
    pthread_mutex_unlock(&c->mu);
    uint8_t req[CONNECT_SIZE] = {0};
    memcpy(req, sec_cfg, SEC_SIZE);
    memcpy(req + SEC_SIZE, user, USER_SIZE);
    put32(req + 0x74, lcv);
    memcpy(req + 0x80, netinfo, RYU_NETINFO);
    conn_send(c, P_CONNECT, req, CONNECT_SIZE, NULL, 0);
    uint32_t r = wait_joined(c, ip, mask, info_out);
    if (r) {
        c->in_lobby = false;
        conn_close(c);
        *result = r;
        return NULL;
    }
    *result = 0;
    return (RyuPeer *)c;
}

void ryu_peer_send(RyuPeer *peer, const RyuProxy *p, const void *data, uint32_t len) {
    Conn *c = (Conn *)peer;
    if (!c || !c->in_lobby || PROXYINFO + 4 + len > MAX_PAYLOAD) return;
    uint8_t h[PROXYINFO + 4];
    put32(h, p->src_ip), put16(h + 4, p->src_port), put32(h + 6, p->dst_ip), put16(h + 10, p->dst_port);
    put32(h + 12, p->proto);
    put32(h + PROXYINFO, len);
    conn_send(c, P_PROXY_DATA, h, sizeof h, data, len);
}

void ryu_peer_close(RyuPeer *peer) {
    Conn *c = (Conn *)peer;
    if (!c) return;
    if (c->in_lobby) conn_send(c, P_DISCONNECT, (uint8_t[4]){0}, 4, NULL, 0);
    c->in_lobby = false;
    conn_close(c);
}
