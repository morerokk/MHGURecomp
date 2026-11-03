// (debug) MHGU_EMTRACE=1: logs every large monster action of the loaded zone with how long it lasted, in real time and
// in game updates, for comparing a monster's moves at different frame rates. MHGU_EMTRACE=2 also logs the monster's
// position every update. MHGU_CONTROL's "emact <group> <index> [mode]" starts that action on the zone's first large
// monster (as its AI does: f_276154). MHGU_EMTRACE_ZAKO=1 logs small monsters: every new action and, twice a second,
// each one's distance to the hunter and current targets.
// Hooked: the engine's frame function (0xda5824: the limiter, which also measures the frame step), after it runs.
#include <math.h>
#include <stdlib.h>

#include "../hle.h"

#define FN_FRAME      0xda5824u   // engine frame start: sleeps to the target, sets the step (+0x68)
#define FN_SET_ACTION 0x276154u   // (monster, group, index, mode): starts a monster action
#define G_SYSTEM      0x2326764u  // engine system object *: +0x68 the frame step in 60 fps frames
#define G_SENEMY      0x1a51fe0u  // sEnemy *: +0x1c: the 10 live enemies of the loaded zone
#define EN_INFO       0x1428u     // enemy: status block pointer (+0x40 position, +0x1b6 network id)
#define EN_ACTION     0x73e0u     // enemy: the current action (group, index, ...)
#define MAX_EN        10

static int g_level, g_zako;
static struct { uint32_t en, act; uint64_t start_ns; uint32_t updates; float x0, y0, z0, rise, dist; } g_seen[MAX_EN];
static volatile int g_req;  // emact request from the control file
static uint32_t g_req_group, g_req_index, g_req_mode;
// emsweep <group> <first> <last>: each action of the group in turn, the next one a second after the last one ended
static volatile int g_sweep;
static uint32_t g_sweep_group, g_sweep_next, g_sweep_last, g_sweep_act;
static uint64_t g_sweep_wait_ns, g_sweep_end_ns;
static uint32_t g_forced = ~0u;  // the last action started from the control file (group | index << 8)

void emtrace_request(uint32_t group, uint32_t index, uint32_t mode) {
    g_req_group = group, g_req_index = index, g_req_mode = mode;
    __atomic_store_n(&g_req, 1, __ATOMIC_RELEASE);
}

void emtrace_sweep(uint32_t group, uint32_t first, uint32_t last) {
    g_sweep_group = group, g_sweep_next = first, g_sweep_last = last, g_sweep_act = 0, g_sweep_wait_ns = 0;
    __atomic_store_n(&g_sweep, 1, __ATOMIC_RELEASE);
}

static bool large(uint32_t en) {
    uint32_t info = LD32(en + EN_INFO);
    if (!info) return false;
    uint32_t id = LD16(info + 0x1b6);
    return id != 0xffff && id < 10000;
}

static bool zako(uint32_t en) {
    uint32_t info = LD32(en + EN_INFO);
    if (!info) return false;
    uint32_t id = LD16(info + 0x1b6);
    return id != 0xffff && id >= 10000;
}

// The hunter's distance from a monster (sPlayer +0x1c: the 4 player slots, position at +0x40).
static float slot0_dist(uint32_t info) {
    uint32_t spl = LD32(0x1a85b58u), p = spl ? LD32(spl + 0x1c) : 0;
    if (!p) return -1;
    float dx = LDF(p + 0x40) - LDF(info + 0x40), dz = LDF(p + 0x48) - LDF(info + 0x48);
    return sqrtf(dx * dx + dz * dz);
}

// The attention rates (f_2bb828: how much a target's score rises per update), last value per monster.
#define FN_HATE_RATE 0x2bb828u
static struct { uint32_t en; int pl, ot; } g_rate[32];

static void hate_rate(Cpu *c) {
    uint32_t en = c->r[0], mode = LD32(c->r[13]);
    fnhook_call_original(c, FN_HATE_RATE);
    int s = 0;
    while (s < 32 && g_rate[s].en && g_rate[s].en != en) s++;
    if (s == 32) return;
    g_rate[s].en = en;
    if (mode == 1) g_rate[s].pl = (int)c->r[0];
    else if (mode == 2) g_rate[s].ot = (int)c->r[0];
}

// A species' attention tables (monster +0x75ec -> +0x88: player table, Palico table; each: distance limits, rates, bonuses).
static void hate_tables(uint32_t en) {
    static uint32_t seen[16];
    uint32_t vt = LD32(en);
    int s = 0;
    while (s < 16 && seen[s] && seen[s] != vt) s++;
    if (s == 16 || seen[s]) return;
    uint32_t par = LD32(en + 0x75ec), sb = par ? LD32(par + 0x88) : 0;
    if (!sb) return;
    seen[s] = vt;
    LOG("[ems] tables vt %08x: at %08x, zone list %08x\n", vt, sb, LD32(en + 0x75f0));
    for (int t = 0; t < 2; t++) {
        uint32_t tb = LD32(sb + 4u * (uint32_t)t);
        if (!tb) { LOG("[ems]   %s: none (defaults)\n", t ? "palico" : "player"); continue; }
        uint32_t lim = LD32(tb), rate = LD32(tb + 4), bon = LD32(tb + 8);
        LOG("[ems]   %s: limits %.0f %.0f %.0f %.0f\n", t ? "palico" : "player", lim ? LDF(lim) : -1, lim ? LDF(lim + 4) : -1,
            lim ? LDF(lim + 8) : -1, lim ? LDF(lim + 12) : -1);
        if (rate)
            LOG("[ems]   rates %d %d %d %d %d %d %d %d\n", (int)LD32(rate), (int)LD32(rate + 4), (int)LD32(rate + 8),
                (int)LD32(rate + 12), (int)LD32(rate + 16), (int)LD32(rate + 20), (int)LD32(rate + 24), (int)LD32(rate + 28));
        if (bon)
            LOG("[ems]   bonuses %d %d %d %d %d %d %d %d %d\n", (int)LD32(bon), (int)LD32(bon + 4), (int)LD32(bon + 8),
                (int)LD32(bon + 12), (int)LD32(bon + 16), (int)LD32(bon + 20), (int)LD32(bon + 24), (int)LD32(bon + 28),
                (int)LD32(bon + 32));
    }
}

static void zako_log(uint32_t sen, uint64_t now) {
    static uint64_t last;
    if (now - last < 500000000ull) return;
    last = now;
    for (int k = 0; k < MAX_EN; k++) {
        uint32_t en = LD32(sen + 0x1c + 4u * (uint32_t)k);
        if (!en || !zako(en)) continue;
        uint32_t info = LD32(en + EN_INFO);
        hate_tables(en);
        for (int s = 0; s < 32; s++)
            if (g_rate[s].en == en) LOG("[ems] %08x rate pl %d ot %d\n", en, g_rate[s].pl, g_rate[s].ot);
        LOG("[ems] %08x vt %08x id %u act %08x flags %02x d %.0f t1 %u/%u/%u/%u t2 %u/%u/%u/%u at %.0f,%.0f hate min %d pl %d ot %d %d mask %02x %02x step %.3f\n", en, LD32(en),
            LD16(info + 0x1b6), LD32(en + EN_ACTION), LD8(en + 0x1408), slot0_dist(info), LD8(info + 0x230), LD8(info + 0x232),
            LD8(info + 0x234), LD8(info + 0x236), LD8(info + 0x231), LD8(info + 0x233), LD8(info + 0x235), LD8(info + 0x237),
            LDF(info + 0x1d0), LDF(info + 0x1d8), (int)LD32(info + 0x250), (int)LD32(info + 0x254), (int)LD32(info + 0x264),
            (int)LD32(info + 0x268), LD8(info + 0x24d), LD8(info + 0x24f), LDF(en + 0x1c));
    }
}

static void frame(Cpu *c) {
    fnhook_call_original(c, FN_FRAME);
    uint32_t sen = LD32(G_SENEMY), sys = LD32(G_SYSTEM);
    if (!sen) return;
    uint64_t now = plat_now_ns();
    if (g_zako) zako_log(sen, now);
    float step = sys ? LDF(sys + 0x68) : 0;
    for (int k = 0; k < MAX_EN; k++) {
        uint32_t en = LD32(sen + 0x1c + 4u * (uint32_t)k);
        if (!en || !large(en)) continue;
        int s = 0;
        while (s < MAX_EN && g_seen[s].en && g_seen[s].en != en) s++;
        if (s == MAX_EN) continue;
        uint32_t act = LD32(en + EN_ACTION), info = LD32(en + EN_INFO);
        float x = LDF(info + 0x40), y = LDF(info + 0x44), z = LDF(info + 0x48);
        bool restart = false;
        if (g_seen[s].en != en) {
            g_seen[s].en = en, restart = true;
            LOG("[em] %08x (vtable %08x) in the zone, action %08x\n", en, LD32(en), act);
        } else if (act != g_seen[s].act) {
            LOG("[em] %08x action %08x -> %08x after %.0f ms, %u updates (step %.3f), rose %.1f, moved %.1f\n", en,
                g_seen[s].act, act, (double)(now - g_seen[s].start_ns) / 1e6, g_seen[s].updates, step, g_seen[s].rise,
                g_seen[s].dist);
            restart = true;
        }
        if (restart) {
            g_seen[s].act = act, g_seen[s].start_ns = now, g_seen[s].updates = 0;
            g_seen[s].x0 = x, g_seen[s].y0 = y, g_seen[s].z0 = z, g_seen[s].rise = 0, g_seen[s].dist = 0;
        }
        if (y - g_seen[s].y0 > g_seen[s].rise) g_seen[s].rise = y - g_seen[s].y0;
        static int dumps;  // level 3: the monster object on 4 updates in a row once it is 200 units up in the air
        if (g_level >= 3 && dumps < 4 && (act & 0xffff) == g_forced && (dumps || y - g_seen[s].y0 > 200)) {
            char path[512];
            const char *home = getenv("MHGU_HOME");
            snprintf(path, sizeof path, "%s%semair_%d.bin", home ? home : "", home ? "/" : "", dumps++);
            FILE *f = fopen(path, "wb");
            if (f) fwrite(G2H(en), 1, 0xd000, f), fclose(f);
            LOG("[em] %s: %08x at height %.1f, step %.3f\n", path, en, y - g_seen[s].y0, step);
        }
        float dx = x - g_seen[s].x0, dz = z - g_seen[s].z0;
        g_seen[s].dist = sqrtf(dx * dx + dz * dz);
        g_seen[s].updates++;
        if (g_level >= 2)
            LOG("[em] %08x %.1f ms act %08x pos %.1f %.1f %.1f step %.3f\n", en, (double)(now - g_seen[s].start_ns) / 1e6,
                act, x, y, z, step);
    }
    if (__atomic_load_n(&g_sweep, __ATOMIC_ACQUIRE) && !g_req) {
        uint32_t en = 0;
        for (int k = 0; k < MAX_EN && !en; k++) {
            uint32_t e = LD32(sen + 0x1c + 4u * (uint32_t)k);
            if (e && large(e)) en = e;
        }
        uint32_t cur = en ? (LD32(en + EN_ACTION) & 0xffff) : 0;
        if (g_sweep_act && (cur != g_sweep_act || now >= g_sweep_end_ns)) {  // the swept action ended (or ran 12 s)
            if (cur == g_sweep_act) LOG("[em] sweep: %u:%u still running after 12 s, next\n", cur & 0xff, cur >> 8);
            g_sweep_act = 0;
            g_sweep_wait_ns = now + 1500000000ull;
        } else if (!g_sweep_act && now >= g_sweep_wait_ns) {
            if (g_sweep_next > g_sweep_last) {
                g_sweep = 0;
                LOG("[em] sweep done\n");
            } else {
                g_req_group = g_sweep_group, g_req_index = g_sweep_next++, g_req_mode = 0, g_req = 1;
                g_sweep_act = g_req_group | g_req_index << 8;
                g_sweep_end_ns = now + 12000000000ull;
            }
        }
    }
    if (__atomic_load_n(&g_req, __ATOMIC_ACQUIRE)) {
        g_req = 0;
        uint32_t en = 0;
        for (int k = 0; k < MAX_EN && !en; k++) {
            uint32_t e = LD32(sen + 0x1c + 4u * (uint32_t)k);
            if (e && large(e)) en = e;
        }
        if (!en) { LOG("[em] emact: no large monster in the zone\n"); return; }
        LOG("[em] emact %08x %u:%u mode %u\n", en, g_req_group, g_req_index, g_req_mode);
        g_forced = g_req_group | g_req_index << 8;
        uint32_t saved[16];
        for (int i = 0; i < 16; i++) saved[i] = c->r[i];
        c->r[0] = en, c->r[1] = g_req_group, c->r[2] = g_req_index, c->r[3] = g_req_mode;
        guest_call(c, FN_SET_ACTION);
        for (int i = 0; i < 16; i++) c->r[i] = saved[i];
    }
}

static void set_action(Cpu *c) {
    if (large(c->r[0]))
        LOG("[em] %08x set action %u:%u mode %u (was %08x) lr %08x\n", c->r[0], c->r[1] & 0xff, c->r[2] & 0xff, c->r[3],
            LD32(c->r[0] + EN_ACTION), c->r[14]);
    else if (g_zako && zako(c->r[0]))
        LOG("[ems] %08x set action %u:%u mode %u (was %08x) d %.0f lr %08x\n", c->r[0], c->r[1] & 0xff, c->r[2] & 0xff, c->r[3],
            LD32(c->r[0] + EN_ACTION), slot0_dist(LD32(c->r[0] + EN_INFO)), c->r[14]);
    fnhook_call_original(c, FN_SET_ACTION);
}

void emtrace_init(void) {
    const char *e = getenv("MHGU_EMTRACE");
    g_zako = getenv("MHGU_EMTRACE_ZAKO") != NULL;
    if ((!e || !*e || !(g_level = atoi(e))) && !g_zako) return;
    fnhook_set(FN_FRAME, hle_register("emtrace:frame", frame));
    fnhook_set(FN_SET_ACTION, hle_register("emtrace:set_action", set_action));
    if (g_zako) fnhook_set(FN_HATE_RATE, hle_register("emtrace:hate_rate", hate_rate));
    LOG("[em] tracing large monster actions (level %d)\n", g_level);
}
