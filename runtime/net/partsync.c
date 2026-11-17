// Multiplayer: a large monster's part breaks reach the other games when they happen (env MHGU_NO_PARTSYNC=1: the original).
//
// In the original game a client learns a monster's parts only from netEmAction, the message the game that runs the
// monster sends when the monster starts an action (the set-action function 0x276154, send at 0x2765f8 through
// 0x2cafe8(monster, 1, 0)). It holds each part's break count and flinch HP (dmPack, 8 parts) and the sever count and HP of
// up to two severable parts (spPack); the client hands them to the monster's vtable slot 0x428 (0x2cb22c for most classes),
// which runs the game's own break (0x299d64, 0x299f98) until each part's count is the received one, and takes the HPs. A
// break of a standing monster comes with a flinch, a new action, so it shows at once everywhere; a break while the monster
// is down, staggered or trapped (one long action) shows on the other games only once it stands up again.
//
// Here the game that runs the monster (+0x7420) looks at the break counts after each update of the monster's hits
// (0x297ddc) and, when one went up, sends the same packed values at once (F_AUX). The others hand them to the same
// vtable slot, as a later netEmAction would; that netEmAction then finds the counts already there and breaks nothing
// twice. They do it from the collision update (0x36b15c), not 0x297ddc: a game reaches 0x297ddc only for the monsters it
// runs (the first version waited there, and its packs never went in). Resending netEmAction itself would not do: its
// receive (0x2cc154) ends by setting the action again (0x276154), which would restart the monster's knockdown.
//
// Mixed versions: no change to the link protocol; a game without this drops the message (unknown magic) and shows breaks
// as the original did. Off in crossplay, like the other port-only features (F_AUX doesn't travel there).
//
// Debug: MHGU_LOG_PARTSYNC=1 logs each message sent and applied.
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "net.h"

// Game data (main, v1.4.0; runtime addresses)
#define FN_EN_APPLY   0x297ddcu   // (monster): this update's hits into its HP, parts, KO, on the game that runs it
#define FN_COLLISION  0x36b15cu   // the collision manager's update: every game, every update
#define G_SENEMY      0x1a51fe0u  // sEnemy *: +0x1c: the 10 live enemies of the loaded zone
#define G_MEMBER      0x1a8ca48u  // +0x30: the local player's slot
#define EN_INFO       0x1428u     // monster: status block pointer
#define EN_RUNS       0x7420u     // monster: u8, this game runs it
#define EN_OWNER      0x7424u     // monster: u8, the slot of the game that runs it
#define EN_DTBASE     0x75ecu     // monster: its dtbase data; +0x68: 10-byte part entries, s16 sever HP at +6
#define INFO_ID       0x1b6u      // u16 network id: large monsters below 10000
#define INFO_PARTS    0x3bcu      // 12-byte part records: u8 breaks, u8 severs, u16 flinch HP (12 bits), u16 sever HP
#define VT_PARTS      0x428u      // monster vtable: (monster, const u16 pack[10]) the packed parts into the monster
#define ZAKO_FIRST    10000u
#define N_PARTS       8
#define N_SEVER       2

#define MAGIC         0xE8u       // aux message (emsync.c 0xE5, hpscale.c 0xE6, hitsync.c 0xE7)
enum { M_PARTS };
#define PENDING       8
#define STALE_NS      5000000000ull

typedef struct {
    uint8_t magic, kind, slot, pad;
    uint16_t id, seq;
    uint16_t pack[N_PARTS + N_SEVER];  // as netEmAction's dmPack[8] and spPack[2]: count << 12 | HP & 0xfff
} Msg;

static bool g_off, g_log;
static uint32_t g_buf;            // guest copy of a pack for VT_PARTS
static uint16_t g_seq;

// The breaks last seen per monster (the game thread's own)
static struct { uint32_t en; uint16_t id; uint8_t n[N_PARTS]; } g_seen[PENDING];

// Packs from the network, waiting for their monster's next update
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { Msg m; uint64_t at; bool used; } g_in[PENDING];
static volatile int g_nin;

static bool sane(uint32_t a) { return a >= 0x08000000u && a < 0xF0000000u && !(a & 3); }
static bool active(void) { return !g_off && ldn_net_up() && !ldn_crossplay(); }

static int local_slot(void) {
    uint32_t mem = LD32(G_MEMBER);
    int s = sane(mem) ? (int)LD32(mem + 0x30) : -1;
    return s >= 0 && s < 4 ? s : -1;
}

static uint32_t rec(uint32_t info, int i) { return info + INFO_PARTS + 12u * (uint32_t)i; }

// The packed parts as the game's netEmAction send packs them (0x2cb8a8): every part's breaks and flinch HP, then the
// severs and sever HP of the first two parts its data gives sever HP.
static void pack_parts(uint32_t en, uint32_t info, uint16_t *pack) {
    memset(pack, 0, (N_PARTS + N_SEVER) * sizeof *pack);
    uint32_t dt = LD32(en + EN_DTBASE), tab = sane(dt) ? LD32(dt + 0x68) : 0;
    int k = 0;
    for (int i = 0; i < N_PARTS; i++) {
        uint32_t r = rec(info, i);
        pack[i] = (uint16_t)(LD8(r) << 12 | (LD16(r + 2) & 0xfffu));
        if (k < N_SEVER && sane(tab & ~3u) && (int16_t)LD16(tab + 10u * (uint32_t)i + 6) >= 1)
            pack[N_PARTS + k++] = (uint16_t)(LD8(r + 1) << 12 | (LD16(r + 4) & 0xfffu));
    }
}

// The game that runs the monster: a break since the last update goes to the others.
static void check_breaks(uint32_t en, uint32_t info, uint16_t id) {
    int free_at = -1, i = 0;
    for (; i < PENDING; i++) {
        if (g_seen[i].en == en) break;
        if (!g_seen[i].en && free_at < 0) free_at = i;
    }
    uint8_t n[N_PARTS];
    for (int p = 0; p < N_PARTS; p++) n[p] = LD8(rec(info, p));
    if (i == PENDING || g_seen[i].id != id) {  // (first seen: nothing to compare with)
        if (i == PENDING) i = free_at >= 0 ? free_at : (int)(id % PENDING);
        g_seen[i].en = en, g_seen[i].id = id;
        memcpy(g_seen[i].n, n, sizeof n);
        return;
    }
    bool more = false;
    for (int p = 0; p < N_PARTS; p++) more |= n[p] > g_seen[i].n[p];
    memcpy(g_seen[i].n, n, sizeof n);
    if (!more || !LD8(en + EN_RUNS)) return;
    int me = local_slot();
    if (me < 0) return;
    Msg m = {MAGIC, M_PARTS, (uint8_t)me, 0, id, ++g_seq, {0}};
    pack_parts(en, info, m.pack);
    ldn_send_aux(&m, sizeof m);
    if (g_log)
        LOG("[partsync] %u sent: monster %u, breaks %u %u %u %u %u %u %u %u\n", m.seq, id, n[0], n[1], n[2], n[3], n[4], n[5],
            n[6], n[7]);
}

// A game that doesn't run the monster: a waiting pack with more breaks than it shows goes into the monster.
static void take_pack(Cpu *c, uint32_t en, uint32_t info, uint16_t id) {
    Msg m;
    bool got = false;
    uint64_t now = plat_now_ns();
    pthread_mutex_lock(&g_lock);
    int left = 0;
    for (int i = 0; i < PENDING; i++) {
        if (!g_in[i].used) continue;
        if (now - g_in[i].at > STALE_NS) {  // (its monster never came)
            g_in[i].used = false;
            if (g_log) LOG("[partsync] %u from slot %u dropped: monster %u not in this zone\n", g_in[i].m.seq, g_in[i].m.slot, g_in[i].m.id);
            continue;
        }
        if (g_in[i].m.id == id) { m = g_in[i].m, got = true, g_in[i].used = false; continue; }
        left++;
    }
    __atomic_store_n(&g_nin, left, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_lock);
    if (!got) return;
    uint8_t owner = LD8(en + EN_OWNER);
    if (owner < 4 && owner != m.slot) {  // (from a game that no longer runs it)
        if (g_log) LOG("[partsync] %u from slot %u dropped: monster %u is run by slot %u\n", m.seq, m.slot, id, owner);
        return;
    }
    bool more = false;
    for (int p = 0; p < N_PARTS; p++) more |= (m.pack[p] >> 12) > LD8(rec(info, p));
    if (!more) {  // (a netEmAction brought them first)
        if (g_log) LOG("[partsync] %u from slot %u: monster %u already has these breaks\n", m.seq, m.slot, id);
        return;
    }
    uint32_t vt = LD32(en), fn = vt >= 0x200000u && !(vt & 3) ? LD32(vt + VT_PARTS) : 0;  // (vtables are in main, not heap)
    if (!fn) {
        if (g_log) LOG("[partsync] %u from slot %u dropped: monster %u has no vtable (%08x)\n", m.seq, m.slot, id, vt);
        return;
    }
    memcpy(G2H(g_buf), m.pack, sizeof m.pack);
    uint32_t saved[16];
    memcpy(saved, c->r, sizeof saved);
    __typeof__(c->v) v = c->v;
    c->r[0] = en, c->r[1] = g_buf;
    guest_call(c, fn);
    memcpy(c->r, saved, sizeof saved);
    c->v = v;
    if (g_log) {
        uint8_t n[N_PARTS];
        for (int p = 0; p < N_PARTS; p++) n[p] = LD8(rec(info, p));
        LOG("[partsync] %u from slot %u applied: monster %u, breaks now %u %u %u %u %u %u %u %u\n", m.seq, m.slot, id, n[0], n[1],
            n[2], n[3], n[4], n[5], n[6], n[7]);
    }
}

// The game that runs a monster: after its update's hits, a new break goes out.
static void en_apply(Cpu *c) {
    uint32_t en = c->r[0], info = sane(en) ? LD32(en + EN_INFO) : 0;
    uint16_t id = sane(info) ? LD16(info + INFO_ID) : 0xffff;
    bool on = id < ZAKO_FIRST && active();
    fnhook_call_next(c, FN_EN_APPLY);
    if (on) check_breaks(en, info, id);
}

// The other games: 0x297ddc is only reached for a monster this game runs, so waiting packs go in from the collision
// update (every game, every update, on the game thread), each to the zone's large monster with its id.
static void collision(Cpu *c) {
    if (__atomic_load_n(&g_nin, __ATOMIC_ACQUIRE) && active()) {
        uint32_t sen = LD32(G_SENEMY);
        for (int k = 0; sane(sen) && k < 10 && __atomic_load_n(&g_nin, __ATOMIC_ACQUIRE); k++) {
            uint32_t en = LD32(sen + 0x1c + 4u * (uint32_t)k), info = sane(en) ? LD32(en + EN_INFO) : 0;
            if (!sane(info) || LD8(en + EN_RUNS)) continue;
            uint16_t id = LD16(info + INFO_ID);
            if (id < ZAKO_FIRST) take_pack(c, en, info, id);
        }
        if (__atomic_load_n(&g_nin, __ATOMIC_ACQUIRE)) take_pack(c, 0, 0, 0xffff);  // (only drops the stale ones)
    }
    fnhook_call_next(c, FN_COLLISION);
}

void partsync_aux_received(const void *data, uint32_t len) {
    const uint8_t *b = data;
    if (len < sizeof(Msg) || b[0] != MAGIC || b[1] != M_PARTS) return;
    Msg m;
    memcpy(&m, data, sizeof m);
    if (m.slot > 3 || m.id >= ZAKO_FIRST || (int)m.slot == local_slot()) return;
    uint64_t now = plat_now_ns();
    pthread_mutex_lock(&g_lock);
    int at = -1, oldest = 0;
    for (int i = 0; i < PENDING; i++) {
        if (g_in[i].used && g_in[i].m.id == m.id) { at = i; break; }  // (a newer pack for the same monster replaces it)
        if (!g_in[i].used && at < 0) at = i;
        if (g_in[i].at < g_in[oldest].at) oldest = i;
    }
    if (at < 0) at = oldest;
    int n = 0;
    g_in[at].m = m, g_in[at].at = now, g_in[at].used = true;
    for (int i = 0; i < PENDING; i++) n += g_in[i].used;
    __atomic_store_n(&g_nin, n, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_lock);
    if (g_log) LOG("[partsync] %u from slot %u received: monster %u\n", m.seq, m.slot, m.id);
}

void partsync_init(void) {
    const char *e = getenv("MHGU_NO_PARTSYNC");
    g_off = e && *e && atoi(e);
    e = getenv("MHGU_LOG_PARTSYNC");
    g_log = e && *e && atoi(e);
    if (g_off) { LOG("[partsync] off (MHGU_NO_PARTSYNC)\n"); return; }
    g_buf = hle_alloc(32);
    fnhook_set_outer(FN_EN_APPLY, hle_register("partsync:apply", en_apply));
    fnhook_set_outer(FN_COLLISION, hle_register("partsync:collision", collision));
}
