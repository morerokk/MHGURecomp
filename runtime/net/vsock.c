// nn::socket on the virtual local play network (net.h). Every socket the game opens lives here: UDP datagrams and
// TCP streams between the games travel inside the ldn.c link (reliable and in order, so TCP needs no retransmission:
// a connection is SYN / SYNACK / data / FIN or RST messages). Nothing reaches the host's real network. The values
// the game passes are the Switch's (BSD socket options, Linux errno numbers, O_NONBLOCK 0x800).
#include <string.h>
#include <time.h>

#include "../hle.h"
#include "net.h"

void netrate_udp(uint32_t len);  // (debug, MHGU_NETRATE: runtime/hle/netrate.c)

// ---- the Switch's constants
enum { S_EBADF = 9, S_EAGAIN = 11, S_ENOMEM = 12, S_EINVAL = 22, S_EMFILE = 24, S_EPIPE = 32, S_ENOTSOCK = 88,
       S_EDESTADDRREQ = 89, S_EMSGSIZE = 90, S_EPROTONOSUPPORT = 93, S_EOPNOTSUPP = 95, S_EAFNOSUPPORT = 97,
       S_EADDRINUSE = 98, S_ENETUNREACH = 101, S_ECONNRESET = 104, S_EISCONN = 106, S_ENOTCONN = 107,
       S_ETIMEDOUT = 110, S_ECONNREFUSED = 111, S_EALREADY = 114, S_EINPROGRESS = 115 };
enum { S_AF_INET = 2, S_SOCK_STREAM = 1, S_SOCK_DGRAM = 2 };
enum { S_SOL_SOCKET = 0xFFFF, S_SO_ACCEPTCONN = 2, S_SO_SNDTIMEO = 0x1005, S_SO_RCVTIMEO = 0x1006, S_SO_ERROR = 0x1007,
       S_SO_TYPE = 0x1008, S_SO_NONBLOCK = 0x1100 };
enum { S_MSG_PEEK = 2, S_MSG_WAITALL = 0x40, S_MSG_DONTWAIT = 0x80 };
enum { S_O_NONBLOCK = 0x800, S_F_GETFL = 3, S_F_SETFL = 4 };
enum { S_POLLIN = 1, S_POLLPRI = 2, S_POLLOUT = 4, S_POLLERR = 8, S_POLLHUP = 0x10, S_POLLNVAL = 0x20,
       S_POLLRDNORM = 0x40, S_POLLRDBAND = 0x80 };

#define MAX_SOCKS 128
#define FD_BASE 3
#define MAX_QUEUED 512        // datagrams waiting in one UDP socket
#define MAX_STREAM (4 << 20)  // bytes waiting in one TCP socket
#define CHUNK 0x8000          // TCP data per packet

typedef struct Dgram { struct Dgram *next; uint32_t ip; uint16_t port; uint32_t len; uint8_t data[]; } Dgram;

enum { T_NONE, T_LISTEN, T_CONNECTING, T_CONNECTED, T_REFUSED };
typedef struct {
    bool used, pending;      // pending: accepted by the network, not yet by the game (in a listener's queue)
    int type;                // S_SOCK_*
    bool nonblock;
    uint32_t ip, rip;        // local / remote address (rip: TCP peer, or a connected UDP socket's)
    uint16_t port, rport;
    uint32_t rcv_timeout_ms; // 0: none
    // UDP
    Dgram *head, *tail;
    int nq;
    // TCP
    int tstate;
    bool peer_fin, shut_wr, reset;
    uint8_t *buf;
    uint32_t blen, bcap;
    int accept_q[16], naccept, listener;
} VSock;

static VSock socks[MAX_SOCKS];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static uint16_t next_port = 49152;
static __thread int s_errno;

#define ONCE(...) do { static int once_; if (!once_++) LOG(__VA_ARGS__); } while (0)
#define NETLOG(...) do { if (ENV("MHGU_LOG_NET")) LOG(__VA_ARGS__); } while (0)

static int fail(int e) { s_errno = e; return -1; }
static VSock *get(int fd) {
    int i = fd - FD_BASE;
    return i >= 0 && i < MAX_SOCKS && socks[i].used && !socks[i].pending ? &socks[i] : NULL;
}
static int fd_of(VSock *s) { return (int)(s - socks) + FD_BASE; }

static void free_queue(VSock *s) {
    for (Dgram *d = s->head, *n; d; d = n) n = d->next, free(d);
    s->head = s->tail = NULL, s->nq = 0;
    free(s->buf);
    s->buf = NULL, s->blen = s->bcap = 0;
}

static VSock *alloc_sock(int type) {
    for (int i = 0; i < MAX_SOCKS; i++)
        if (!socks[i].used) {
            memset(&socks[i], 0, sizeof socks[i]);
            socks[i].used = true, socks[i].type = type, socks[i].listener = -1;
            return &socks[i];
        }
    return NULL;
}

static bool port_in_use(int type, uint16_t port) {
    for (int i = 0; i < MAX_SOCKS; i++)
        if (socks[i].used && socks[i].type == type && socks[i].port == port && (type == S_SOCK_DGRAM || socks[i].tstate == T_LISTEN))
            return true;
    return false;
}

static uint16_t ephemeral(int type) {
    for (int n = 0; n < 16384; n++) {
        uint16_t p = next_port++;
        if (next_port == 0) next_port = 49152;
        if (!port_in_use(type, p)) return p;
    }
    return 0;
}

// Wait on the shared condition; false when timeout_ms (> 0) passed. Caller holds mu.
static bool wait_until(const struct timespec *dl) {
    if (!dl) { pthread_cond_wait(&cv, &mu); return true; }
    return pthread_cond_timedwait(&cv, &mu, dl) == 0;
}
static struct timespec *deadline(struct timespec *ts, uint32_t ms) {
    if (!ms) return NULL;
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000;
    if (ts->tv_nsec >= 1000000000) ts->tv_sec++, ts->tv_nsec -= 1000000000;
    return ts;
}

// ---- guest sockaddr_in: {u8 len, u8 family, u16 port (big endian), u32 address (big endian), u8 zero[8]}
static bool read_addr(uint32_t ga, uint32_t len, uint32_t *ip, uint16_t *port) {
    if (!ga || len < 8) return false;
    const uint8_t *a = G2H(ga);
    *port = (uint16_t)(a[2] << 8 | a[3]);
    *ip = (uint32_t)a[4] << 24 | (uint32_t)a[5] << 16 | (uint32_t)a[6] << 8 | a[7];
    return true;
}
static void write_addr(uint32_t ga, uint32_t glen, uint32_t ip, uint16_t port) {
    if (!ga) return;
    uint8_t a[16] = {16, S_AF_INET, (uint8_t)(port >> 8), (uint8_t)port, (uint8_t)(ip >> 24), (uint8_t)(ip >> 16),
                     (uint8_t)(ip >> 8), (uint8_t)ip};
    uint32_t n = glen ? LD32(glen) : 16;
    memcpy(G2H(ga), a, n < 16 ? n : 16);
    if (glen) ST32(glen, 16);
}
static const char *ipstr(uint32_t ip) {
    static __thread char b[4][20];
    static __thread int k;
    char *s = b[k++ & 3];
    snprintf(s, 20, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    return s;
}

// ---- out of the lock: packets for the network
static void send_tcp(VSock *s, int kind, const void *data, uint32_t len) {  // (fields read under mu by the caller)
    VPacket p = {s->ip ? s->ip : ldn_own_ip(), s->rip, s->port, s->rport, VPROTO_TCP, (uint8_t)kind, 0};
    ldn_send(&p, data, len);
}

// ---- from the network
void vsock_deliver(const VPacket *p, const void *data, uint32_t len) {
    VPacket reply = {0};
    bool send_reply = false;
    pthread_mutex_lock(&mu);
    uint32_t bcast = ldn_broadcast_ip();
    if (p->proto == VPROTO_UDP) {
        bool all = p->dst_ip == bcast || p->dst_ip == 0xFFFFFFFFu;
        for (int i = 0; i < MAX_SOCKS; i++) {
            VSock *s = &socks[i];
            if (!s->used || s->type != S_SOCK_DGRAM || s->port != p->dst_port) continue;
            if (s->ip && s->ip != p->dst_ip && !all) continue;
            if (s->rip && (s->rip != p->src_ip || s->rport != p->src_port)) continue;
            if (s->nq >= MAX_QUEUED) { ONCE("[net] a socket's receive queue is full: dropping datagrams\n"); continue; }
            Dgram *d = malloc(sizeof *d + len);
            if (!d) break;
            d->next = NULL, d->ip = p->src_ip, d->port = p->src_port, d->len = len;
            memcpy(d->data, data, len);
            if (s->tail) s->tail->next = d; else s->head = d;
            s->tail = d, s->nq++;
            if (!all) break;
        }
    } else {
        VSock *conn = NULL;
        for (int i = 0; i < MAX_SOCKS && !conn; i++) {
            VSock *s = &socks[i];
            if (s->used && s->type == S_SOCK_STREAM && s->port == p->dst_port && s->rport == p->src_port &&
                s->rip == p->src_ip && (s->tstate == T_CONNECTED || s->tstate == T_CONNECTING))
                conn = s;
        }
        switch (p->kind) {
        case VTCP_SYN: {
            VSock *l = NULL;
            for (int i = 0; i < MAX_SOCKS && !l; i++)
                if (socks[i].used && socks[i].type == S_SOCK_STREAM && socks[i].tstate == T_LISTEN && socks[i].port == p->dst_port)
                    l = &socks[i];
            VSock *n = l && l->naccept < 16 ? alloc_sock(S_SOCK_STREAM) : NULL;
            reply = (VPacket){p->dst_ip, p->src_ip, p->dst_port, p->src_port, VPROTO_TCP, VTCP_RST, 0};
            if (n) {
                n->pending = true, n->tstate = T_CONNECTED;
                n->ip = p->dst_ip, n->port = p->dst_port, n->rip = p->src_ip, n->rport = p->src_port;
                n->listener = (int)(l - socks);
                l->accept_q[l->naccept++] = (int)(n - socks);
                reply.kind = VTCP_SYNACK;
            }
            NETLOG("[net] tcp SYN %s:%u -> %s:%u: %s\n", ipstr(p->src_ip), p->src_port, ipstr(p->dst_ip), p->dst_port,
                   n ? "accepted" : "refused");
            send_reply = true;
            break;
        }
        case VTCP_SYNACK: if (conn && conn->tstate == T_CONNECTING) conn->tstate = T_CONNECTED; break;
        case VTCP_DATA:
            if (!conn || conn->blen + len > MAX_STREAM) {
                if (conn) ONCE("[net] a TCP socket's receive buffer is full: dropping data\n");
                break;
            }
            if (conn->blen + len > conn->bcap) {
                uint32_t cap = conn->bcap ? conn->bcap : 0x4000;
                while (cap < conn->blen + len) cap *= 2;
                uint8_t *nb = realloc(conn->buf, cap);
                if (!nb) break;
                conn->buf = nb, conn->bcap = cap;
            }
            memcpy(conn->buf + conn->blen, data, len);
            conn->blen += len;
            break;
        case VTCP_FIN: if (conn) conn->peer_fin = true; break;
        case VTCP_RST:
            if (conn) { conn->reset = true; if (conn->tstate == T_CONNECTING) conn->tstate = T_REFUSED; }
            break;
        }
    }
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    if (send_reply) ldn_send(&reply, NULL, 0);
}

void vsock_peer_gone(uint32_t ip) {
    pthread_mutex_lock(&mu);
    for (int i = 0; i < MAX_SOCKS; i++)
        if (socks[i].used && socks[i].type == S_SOCK_STREAM && socks[i].rip == ip && socks[i].tstate != T_LISTEN)
            socks[i].reset = true;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
}

void vsock_network_down(void) {
    pthread_mutex_lock(&mu);
    for (int i = 0; i < MAX_SOCKS; i++) {
        VSock *s = &socks[i];
        if (!s->used) continue;
        if (s->type == S_SOCK_STREAM && s->tstate != T_LISTEN && s->tstate != T_NONE) s->reset = true;
        for (Dgram *d = s->head, *n; d; d = n) n = d->next, free(d);
        s->head = s->tail = NULL, s->nq = 0;
    }
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
}

// ---- the calls
static int sock_close_locked(VSock *s, VPacket *fin) {
    bool tell = s->type == S_SOCK_STREAM && s->tstate == T_CONNECTED && !s->reset && !s->shut_wr;
    if (tell) *fin = (VPacket){s->ip ? s->ip : ldn_own_ip(), s->rip, s->port, s->rport, VPROTO_TCP, VTCP_FIN, 0};
    if (s->tstate == T_LISTEN)  // connections nobody accepted: refused
        for (int k = 0; k < s->naccept; k++) { socks[s->accept_q[k]].used = false; free_queue(&socks[s->accept_q[k]]); }
    free_queue(s);
    s->used = false;
    pthread_cond_broadcast(&cv);
    return tell;
}

static void x_socket(Cpu *c) {
    int domain = (int)ARG(0), type = (int)ARG(1);
    if (domain != S_AF_INET) { RET(fail(S_EAFNOSUPPORT)); return; }
    if (type != S_SOCK_STREAM && type != S_SOCK_DGRAM) { RET(fail(S_EPROTONOSUPPORT)); return; }
    pthread_mutex_lock(&mu);
    VSock *s = alloc_sock(type);
    pthread_mutex_unlock(&mu);
    if (!s) { RET(fail(S_EMFILE)); return; }
    NETLOG("[net] socket(%s) -> %d\n", type == S_SOCK_DGRAM ? "udp" : "tcp", fd_of(s));
    RET(fd_of(s));
}

static void x_close(Cpu *c) {
    pthread_mutex_lock(&mu);
    VSock *s = get((int)ARG(0));
    VPacket fin;
    int tell = s ? sock_close_locked(s, &fin) : 0;
    pthread_mutex_unlock(&mu);
    if (!s) { RET(fail(S_EBADF)); return; }
    if (tell) ldn_send(&fin, NULL, 0);
    NETLOG("[net] close(%d)\n", (int)ARG(0));
    RET(0);
}

static void x_bind(Cpu *c) {
    uint32_t ip;
    uint16_t port;
    if (!read_addr(ARG(1), ARG(2), &ip, &port)) { RET(fail(S_EINVAL)); return; }
    pthread_mutex_lock(&mu);
    VSock *s = get((int)ARG(0));
    int r = 0;
    if (!s) r = fail(S_EBADF);
    else if (s->port) r = fail(S_EINVAL);
    else {
        if (!port) port = ephemeral(s->type);
        if (port_in_use(s->type, port) && s->type == S_SOCK_STREAM) r = fail(S_EADDRINUSE);
        else s->ip = ip == ldn_broadcast_ip() ? 0 : ip, s->port = port;
    }
    pthread_mutex_unlock(&mu);
    NETLOG("[net] bind(%d, %s:%u) -> %d\n", (int)ARG(0), ipstr(ip), port, r);
    RET(r);
}

static void x_listen(Cpu *c) {
    pthread_mutex_lock(&mu);
    VSock *s = get((int)ARG(0));
    int r = 0;
    if (!s) r = fail(S_EBADF);
    else if (s->type != S_SOCK_STREAM) r = fail(S_EOPNOTSUPP);
    else {
        if (!s->port) s->port = ephemeral(S_SOCK_STREAM);
        s->tstate = T_LISTEN;
    }
    pthread_mutex_unlock(&mu);
    NETLOG("[net] listen(%d) port %u -> %d\n", (int)ARG(0), s ? s->port : 0, r);
    RET(r);
}

static void x_accept(Cpu *c) {
    int fd = (int)ARG(0), r;
    pthread_mutex_lock(&mu);
    for (;;) {
        VSock *s = get(fd);
        if (!s || s->tstate != T_LISTEN) { r = fail(s ? S_EINVAL : S_EBADF); break; }
        if (s->naccept) {
            VSock *n = &socks[s->accept_q[0]];
            memmove(s->accept_q, s->accept_q + 1, (size_t)--s->naccept * sizeof s->accept_q[0]);
            n->pending = false;
            write_addr(ARG(1), ARG(2), n->rip, n->rport);
            r = fd_of(n);
            NETLOG("[net] accept(%d) -> %d from %s:%u\n", fd, r, ipstr(n->rip), n->rport);
            break;
        }
        if (s->nonblock) { r = fail(S_EAGAIN); break; }
        wait_until(NULL);
    }
    pthread_mutex_unlock(&mu);
    RET(r);
}

static void x_connect(Cpu *c) {
    uint32_t ip;
    uint16_t port;
    int fd = (int)ARG(0), r = 0;
    if (!read_addr(ARG(1), ARG(2), &ip, &port)) { RET(fail(S_EINVAL)); return; }
    pthread_mutex_lock(&mu);
    VSock *s = get(fd);
    if (!s) { pthread_mutex_unlock(&mu); RET(fail(S_EBADF)); return; }
    if (s->type == S_SOCK_DGRAM) {  // a default destination and a receive filter
        if (!s->port) s->port = ephemeral(S_SOCK_DGRAM);
        s->rip = ip, s->rport = port;
        pthread_mutex_unlock(&mu);
        NETLOG("[net] connect(%d, udp %s:%u)\n", fd, ipstr(ip), port);
        RET(0);
        return;
    }
    if (s->tstate == T_CONNECTED) { pthread_mutex_unlock(&mu); RET(fail(S_EISCONN)); return; }
    if (s->tstate == T_CONNECTING) { pthread_mutex_unlock(&mu); RET(fail(S_EALREADY)); return; }
    if (!ldn_net_up()) { pthread_mutex_unlock(&mu); RET(fail(S_ENETUNREACH)); return; }
    if (!s->port) s->port = ephemeral(S_SOCK_STREAM);
    s->rip = ip, s->rport = port, s->tstate = T_CONNECTING, s->reset = false;
    VSock copy = *s;
    pthread_mutex_unlock(&mu);
    send_tcp(&copy, VTCP_SYN, NULL, 0);
    pthread_mutex_lock(&mu);
    struct timespec ts, *dl = deadline(&ts, 10000);
    for (;;) {
        s = get(fd);
        if (!s) { r = fail(S_EBADF); break; }
        if (s->tstate == T_CONNECTED) break;
        if (s->tstate == T_REFUSED) { s->tstate = T_NONE; s->reset = false; r = fail(S_ECONNREFUSED); break; }
        if (copy.nonblock) { r = fail(S_EINPROGRESS); break; }
        if (!wait_until(dl)) { s->tstate = T_NONE; r = fail(S_ETIMEDOUT); break; }
    }
    pthread_mutex_unlock(&mu);
    NETLOG("[net] connect(%d, tcp %s:%u) -> %d (errno %d)\n", fd, ipstr(ip), port, r, r ? s_errno : 0);
    RET(r);
}

static int do_send(Cpu *c, int fd, uint32_t buf, uint32_t len, int flags, uint32_t to, uint32_t tolen) {
    (void)flags;
    uint32_t ip = 0;
    uint16_t port = 0;
    bool has_to = to && read_addr(to, tolen, &ip, &port);
    pthread_mutex_lock(&mu);
    VSock *s = get(fd);
    if (!s) { pthread_mutex_unlock(&mu); return fail(S_EBADF); }
    if (s->type == S_SOCK_DGRAM) {
        if (!has_to) {
            if (!s->rport) { pthread_mutex_unlock(&mu); return fail(S_EDESTADDRREQ); }
            ip = s->rip, port = s->rport;
        }
        if (!s->port) s->port = ephemeral(S_SOCK_DGRAM);
        VPacket p = {s->ip ? s->ip : ldn_own_ip(), ip, s->port, port, VPROTO_UDP, 0, 0};
        pthread_mutex_unlock(&mu);
        if (len > 0xFFE0) return fail(S_EMSGSIZE);
        if (!ldn_net_up()) return fail(S_ENETUNREACH);
        ldn_send(&p, G2H(buf), len);
        netrate_udp(len);  // (debug, MHGU_NETRATE)
        return (int)len;
    }
    if (s->reset) { pthread_mutex_unlock(&mu); return fail(S_ECONNRESET); }
    if (s->tstate != T_CONNECTED) { pthread_mutex_unlock(&mu); return fail(S_ENOTCONN); }
    if (s->shut_wr) { pthread_mutex_unlock(&mu); return fail(S_EPIPE); }
    VSock copy = *s;
    pthread_mutex_unlock(&mu);
    for (uint32_t off = 0; off < len; off += CHUNK)
        send_tcp(&copy, VTCP_DATA, (const uint8_t *)G2H(buf) + off, len - off < CHUNK ? len - off : CHUNK);
    (void)c;
    return (int)len;
}

static int do_recv(int fd, uint32_t buf, uint32_t len, int flags, uint32_t from, uint32_t fromlen) {
    int r;
    pthread_mutex_lock(&mu);
    VSock *s = get(fd);
    struct timespec ts, *dl = s ? deadline(&ts, s->rcv_timeout_ms) : NULL;
    for (;;) {
        s = get(fd);
        if (!s) { r = fail(S_EBADF); break; }
        bool dontwait = s->nonblock || (flags & S_MSG_DONTWAIT);
        if (s->type == S_SOCK_DGRAM) {
            if (s->head) {
                Dgram *d = s->head;
                uint32_t n = d->len < len ? d->len : len;
                memcpy(G2H(buf), d->data, n);
                write_addr(from, fromlen, d->ip, d->port);
                if (!(flags & S_MSG_PEEK)) {
                    s->head = d->next;
                    if (!s->head) s->tail = NULL;
                    s->nq--;
                    free(d);
                }
                r = (int)n;
                break;
            }
        } else {
            if (s->tstate != T_CONNECTED && !s->reset) { r = fail(S_ENOTCONN); break; }
            if (s->blen) {
                uint32_t n = s->blen < len ? s->blen : len;
                memcpy(G2H(buf), s->buf, n);
                if (!(flags & S_MSG_PEEK)) memmove(s->buf, s->buf + n, s->blen -= n);
                write_addr(from, fromlen, s->rip, s->rport);
                r = (int)n;
                break;
            }
            if (s->reset) { r = fail(S_ECONNRESET); break; }
            if (s->peer_fin) { r = 0; break; }
        }
        if (dontwait) { r = fail(S_EAGAIN); break; }
        if (!wait_until(dl)) { r = fail(S_EAGAIN); break; }
    }
    pthread_mutex_unlock(&mu);
    return r;
}

static void x_send(Cpu *c) { RET(do_send(c, (int)ARG(0), ARG(1), ARG(2), (int)ARG(3), 0, 0)); }
static void x_sendto(Cpu *c) { RET(do_send(c, (int)ARG(0), ARG(1), ARG(2), (int)ARG(3), STK(0), STK(1))); }
static void x_recv(Cpu *c) { RET(do_recv((int)ARG(0), ARG(1), ARG(2), (int)ARG(3), 0, 0)); }
static void x_recvfrom(Cpu *c) { RET(do_recv((int)ARG(0), ARG(1), ARG(2), (int)ARG(3), STK(0), STK(1))); }

static void x_shutdown(Cpu *c) {
    pthread_mutex_lock(&mu);
    VSock *s = get((int)ARG(0));
    bool fin = false;
    VSock copy;
    if (s && s->type == S_SOCK_STREAM && s->tstate == T_CONNECTED && ARG(1) != 0 && !s->shut_wr)
        s->shut_wr = fin = true, copy = *s;
    pthread_mutex_unlock(&mu);
    if (!s) { RET(fail(S_EBADF)); return; }
    if (fin) send_tcp(&copy, VTCP_FIN, NULL, 0);
    RET(0);
}

// Poll(pollfd *fds, nfds, timeout ms): pollfd = {int fd; short events; short revents}
static void x_poll(Cpu *c) {
    uint32_t fds = ARG(0), n = ARG(1);
    int timeout = (int)ARG(2), ready;
    pthread_mutex_lock(&mu);
    struct timespec ts, *dl = timeout > 0 ? deadline(&ts, (uint32_t)timeout) : NULL;
    for (;;) {
        ready = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t pf = fds + i * 8;
            int fd = (int)LD32(pf);
            uint16_t ev = *(uint16_t *)G2H(pf + 4), re = 0;
            if (fd < 0) { *(uint16_t *)G2H(pf + 6) = 0; continue; }
            VSock *s = get(fd);
            if (!s) re = S_POLLNVAL;
            else if (s->type == S_SOCK_DGRAM) {
                if (s->head) re |= ev & (S_POLLIN | S_POLLRDNORM);
                re |= ev & S_POLLOUT;
            } else if (s->tstate == T_LISTEN) {
                if (s->naccept) re |= ev & (S_POLLIN | S_POLLRDNORM);
            } else {
                if (s->blen || s->peer_fin || s->reset) re |= ev & (S_POLLIN | S_POLLRDNORM);
                if (s->tstate == T_CONNECTED && !s->reset) re |= ev & S_POLLOUT;
                if (s->reset || s->tstate == T_REFUSED) re |= S_POLLERR | S_POLLHUP;
                else if (s->peer_fin) re |= S_POLLHUP;
            }
            *(uint16_t *)G2H(pf + 6) = re;
            if (re) ready++;
        }
        if (ready || timeout == 0) break;
        if (!wait_until(dl)) break;
    }
    pthread_mutex_unlock(&mu);
    RET(ready);
}

static void x_fcntl(Cpu *c) {
    int cmd = (int)ARG(1), r = 0;
    pthread_mutex_lock(&mu);
    VSock *s = get((int)ARG(0));
    if (!s) r = fail(S_EBADF);
    else if (cmd == S_F_GETFL) r = s->nonblock ? S_O_NONBLOCK : 0;
    else if (cmd == S_F_SETFL) s->nonblock = (ARG(2) & S_O_NONBLOCK) != 0;
    else LOG("[net] fcntl(%d, cmd %d, %#x): ignored\n", (int)ARG(0), cmd, ARG(2));
    pthread_mutex_unlock(&mu);
    RET(r);
}

// SetSockOpt(fd, level, name, const void *value, len)
static void x_setsockopt(Cpu *c) {
    int level = (int)ARG(1), name = (int)ARG(2);
    uint32_t val = ARG(3), len = STK(0);
    pthread_mutex_lock(&mu);
    VSock *s = get((int)ARG(0));
    int r = 0;
    if (!s) r = fail(S_EBADF);
    else if (level == S_SOL_SOCKET && name == S_SO_NONBLOCK && len >= 4) s->nonblock = LD32(val) != 0;
    else if (level == S_SOL_SOCKET && name == S_SO_RCVTIMEO && len >= 8) {
        uint64_t sec = len >= 16 ? *(uint64_t *)G2H(val) : LD32(val);
        uint64_t usec = len >= 16 ? *(uint64_t *)G2H(val + 8) : LD32(val + 4);
        s->rcv_timeout_ms = (uint32_t)(sec * 1000 + usec / 1000);
    } else NETLOG("[net] setsockopt(%d, %#x, %#x, len %u) (accepted)\n", (int)ARG(0), level, name, len);
    pthread_mutex_unlock(&mu);
    RET(r);
}

// GetSockOpt(fd, level, name, void *value, u32 *len)
static void x_getsockopt(Cpu *c) {
    int level = (int)ARG(1), name = (int)ARG(2);
    uint32_t val = ARG(3), plen = STK(0), v = 0;
    pthread_mutex_lock(&mu);
    VSock *s = get((int)ARG(0));
    int r = 0;
    if (!s) r = fail(S_EBADF);
    else if (level == S_SOL_SOCKET && name == S_SO_ERROR) {
        if (s->tstate == T_REFUSED) v = S_ECONNREFUSED, s->tstate = T_NONE;
        else if (s->reset) v = S_ECONNRESET;
    } else if (level == S_SOL_SOCKET && name == S_SO_TYPE) v = (uint32_t)s->type;
    else if (level == S_SOL_SOCKET && name == S_SO_ACCEPTCONN) v = s->tstate == T_LISTEN;
    else if (level == S_SOL_SOCKET && name == S_SO_NONBLOCK) v = s->nonblock;
    else if (level == S_SOL_SOCKET && (name == 0x1001 || name == 0x1002)) v = 0x10000;  // SO_SNDBUF / SO_RCVBUF
    pthread_mutex_unlock(&mu);
    if (!r && val && plen && LD32(plen) >= 4) { ST32(val, v); ST32(plen, 4); }
    RET(r);
}

static void x_getsockname(Cpu *c) {
    pthread_mutex_lock(&mu);
    VSock *s = get((int)ARG(0));
    if (s) write_addr(ARG(1), ARG(2), s->ip, s->port);
    pthread_mutex_unlock(&mu);
    RET(s ? 0 : fail(S_EBADF));
}

static void x_getpeername(Cpu *c) {
    pthread_mutex_lock(&mu);
    VSock *s = get((int)ARG(0));
    bool ok = s && (s->rport || s->tstate == T_CONNECTED);
    if (ok) write_addr(ARG(1), ARG(2), s->rip, s->rport);
    pthread_mutex_unlock(&mu);
    RET(ok ? 0 : fail(s ? S_ENOTCONN : S_EBADF));
}

// ---- nn::socket (C++) and nnsocket (C) names of the same calls
HLE_FUNC("_ZN2nn6socket10InitializeEPvjji", sock_initialize) { NTRACE(); (void)c; ONCE("[net] sockets: local play network only\n"); RET(0); }
HLE_FUNC("_ZN2nn6socket8FinalizeEv", sock_finalize) { NTRACE(); RET(0); }
HLE_FUNC("_ZN2nn6socket12GetLastErrnoEv", sock_errno) { NTRACE(); RET(s_errno); }
HLE_FUNC("nnsocketGetLastErrno", sock_errno_c) { NTRACE(); RET(s_errno); }
HLE_FUNC("nnsocketSetLastErrno", sock_set_errno_c) { NTRACE(); s_errno = (int)ARG(0); }
HLE_FUNC("_ZN2nn6socket6SocketEiii", sock_socket) { NTRACE(); x_socket(c); }
HLE_FUNC("nnsocketSocket", sock_socket_c) { NTRACE(); x_socket(c); }
HLE_FUNC("_ZN2nn6socket5CloseEi", sock_close) { NTRACE(); x_close(c); }
HLE_FUNC("nnsocketClose", sock_close_c) { NTRACE(); x_close(c); }
HLE_FUNC("_ZN2nn6socket4BindEiPK8sockaddrj", sock_bind) { NTRACE(); x_bind(c); }
HLE_FUNC("nnsocketBind", sock_bind_c) { NTRACE(); x_bind(c); }
HLE_FUNC("_ZN2nn6socket6ListenEii", sock_listen) { NTRACE(); x_listen(c); }
HLE_FUNC("_ZN2nn6socket6AcceptEiP8sockaddrPj", sock_accept) { NTRACE(); x_accept(c); }
HLE_FUNC("_ZN2nn6socket7ConnectEiPK8sockaddrj", sock_connect) { NTRACE(); x_connect(c); }
HLE_FUNC("nnsocketConnect", sock_connect_c) { NTRACE(); x_connect(c); }
HLE_FUNC("_ZN2nn6socket4SendEiPKvji", sock_send) { NTRACE(); x_send(c); }
HLE_FUNC("nnsocketSend", sock_send_c) { NTRACE(); x_send(c); }
HLE_FUNC("_ZN2nn6socket6SendToEiPKvjiPK8sockaddrj", sock_sendto) { NTRACE(); x_sendto(c); }
HLE_FUNC("_ZN2nn6socket4RecvEiPvji", sock_recv) { NTRACE(); x_recv(c); }
HLE_FUNC("nnsocketRecv", sock_recv_c) { NTRACE(); x_recv(c); }
HLE_FUNC("_ZN2nn6socket8RecvFromEiPvjiP8sockaddrPj", sock_recvfrom) { NTRACE(); x_recvfrom(c); }
HLE_FUNC("_ZN2nn6socket8ShutdownEii", sock_shutdown) { NTRACE(); x_shutdown(c); }
HLE_FUNC("_ZN2nn6socket4PollEP6pollfdmi", sock_poll) { NTRACE(); x_poll(c); }
HLE_FUNC("nnsocketPoll", sock_poll_c) { NTRACE(); x_poll(c); }
HLE_FUNC("_ZN2nn6socket5FcntlEiiz", sock_fcntl) { NTRACE(); x_fcntl(c); }
HLE_FUNC("nnsocketFcntl", sock_fcntl_c) { NTRACE(); x_fcntl(c); }
HLE_FUNC("_ZN2nn6socket10SetSockOptEiiiPKvj", sock_setsockopt) { NTRACE(); x_setsockopt(c); }
HLE_FUNC("nnsocketSetSockOpt", sock_setsockopt_c) { NTRACE(); x_setsockopt(c); }
HLE_FUNC("_ZN2nn6socket10GetSockOptEiiiPvPj", sock_getsockopt) { NTRACE(); x_getsockopt(c); }
HLE_FUNC("nnsocketGetSockOpt", sock_getsockopt_c) { NTRACE(); x_getsockopt(c); }
HLE_FUNC("_ZN2nn6socket11GetSockNameEiP8sockaddrPj", sock_getsockname) { NTRACE(); x_getsockname(c); }
HLE_FUNC("nnsocketGetSockName", sock_getsockname_c) { NTRACE(); x_getsockname(c); }
HLE_FUNC("_ZN2nn6socket11GetPeerNameEiP8sockaddrPj", sock_getpeername) { NTRACE(); x_getpeername(c); }
HLE_FUNC("nnsocketGetPeerName", sock_getpeername_c) { NTRACE(); x_getpeername(c); }
// Name lookups (the internet): nothing resolves.
HLE_FUNC("_ZN2nn6socket11GetAddrInfoEPKcS2_PK8addrinfoPPS3_", sock_getaddrinfo) { NTRACE(); if (ARG(3)) ST32(ARG(3), 0); RET(8); }  // EAI_NONAME
HLE_FUNC("_ZN2nn6socket12FreeAddrInfoEP8addrinfo", sock_freeaddrinfo) { NTRACE(); (void)c; }
HLE_FUNC("_ZN2nn6socket13GetHostByNameEPKc", sock_gethostbyname) { NTRACE(); RET(0); }
HLE_FUNC("nnsocketGetHostByNameWithOptions", sock_gethostbyname_opt) { NTRACE(); RET(0); }
HLE_FUNC("nnsocketRequestCancelHandle", sock_cancel_handle) { NTRACE(); RET(1); }
HLE_FUNC("nnsocketCancel", sock_cancel) { NTRACE(); RET(0); }
