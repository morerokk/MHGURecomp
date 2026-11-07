// (debug) MHGU_NETRATE=1: how much the game sends to the other players, against the real frame rate, for checking
// whether sends scale with the frame rate. Every 5 s one line: presented frames a second, the game's UDP datagrams
// (and bytes) a second (runtime/net/vsock.c counts them), and its RPC sends a second per sending site. Every message
// type (nFestaNetwork::net*) has its own send function calling the shared send 0xbfc3d8 (this, ?, ?, [sp] flags), so the
// return address names the type (map a site to its class with the DTI it references a little further on).
#include <stdlib.h>

#include "../hle.h"

#define FN_RPC_SEND 0xbfc3d8u
#define MAX_SITES 64

static bool g_on;
static struct { uint32_t lr, n; } g_site[MAX_SITES];
static uint32_t g_dgrams, g_bytes;
static uint64_t g_t0, g_f0;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

void netrate_udp(uint32_t len) {  // (vsock.c: a datagram the game sent)
    if (!g_on) return;
    __atomic_add_fetch(&g_dgrams, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_bytes, len, __ATOMIC_RELAXED);
}

static void report(uint64_t now) {
    double s = (double)(now - g_t0) / 1e9;
    uint64_t f = g_present_count;
    char buf[1024];
    int o = 0;
    for (int pass = 0; pass < MAX_SITES; pass++) {  // the busiest first
        int best = -1;
        for (int i = 0; i < MAX_SITES; i++)
            if (g_site[i].n && (best < 0 || g_site[i].n > g_site[best].n)) best = i;
        if (best < 0) break;
        if (o < (int)sizeof buf - 40) o += snprintf(buf + o, sizeof buf - (size_t)o, " %06x:%.1f", g_site[best].lr - 4, g_site[best].n / s);
        g_site[best].n = 0;
    }
    buf[o] = 0;
    uint32_t d = __atomic_exchange_n(&g_dgrams, 0, __ATOMIC_RELAXED), b = __atomic_exchange_n(&g_bytes, 0, __ATOMIC_RELAXED);
    LOG("[netrate] %.1f fps, udp %.1f/s %.0f B/s, rpc/s (send site:rate)%s\n", (f - g_f0) / s, d / s, b / s, buf);
    g_t0 = now, g_f0 = f;
}

static void rpc_send(Cpu *c) {
    uint32_t lr = c->r[14];
    uint64_t now = plat_now_ns();
    pthread_mutex_lock(&g_mu);
    if (!g_t0) g_t0 = now, g_f0 = g_present_count;
    int i = 0;
    while (i < MAX_SITES && g_site[i].lr && g_site[i].lr != lr) i++;
    if (i < MAX_SITES) g_site[i].lr = lr, g_site[i].n++;
    if (now - g_t0 >= 5000000000ull) report(now);
    pthread_mutex_unlock(&g_mu);
    fnhook_call_original(c, FN_RPC_SEND);
}

void netrate_init(void) {
    const char *e = getenv("MHGU_NETRATE");
    if (!e || !*e || !atoi(e)) return;
    g_on = true;
    fnhook_set(FN_RPC_SEND, hle_register("netrate:send", rpc_send));
    LOG("[netrate] counting network sends against the frame rate\n");
}
