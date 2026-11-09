// Multiplayer: melee hits count as the hunter who made them saw them ([Multiplayer] LagCompensation, env MHGU_LAGCOMP).
//
// In the original game every game checks every hunter's melee hits against its own copy of the monster, but only the
// game that runs the monster (its owner) applies them: a client's swing counts only if the owner's copy of that client
// (built from its network updates) also connects. Here the hunter's own game reports each weapon hit it saw, and the
// owner applies the report and ignores what its copy of that hunter does. Each hit is counted once: the owner drops its
// copy's hit in full, and a report travels the reliable link (ldn_send_aux), once. Only direct weapon hits on large
// monsters: bows and bowguns (hit-checked on the owner only), kinsects, phials, thrown items and Palicoes stay as they are.
//
// A hit doesn't touch the monster's HP. It adds to per-update fields of the status block (*(monster + 0x1428)), all of
// them in one place:
//   +0x3a0 u16 flags         +0x444 u8 hit this update
//   +0x484 u8 reaction kind, +0x485 u8, +0x486 s16 direction, +0x4e4 u8 attacker slot, +0x4f0 vec4 where: the strongest
//                            hit of the update (by the table behind 0x267254)
//   +0x488 s16 x38           part damage (8), its per-special copies, statuses, KO (+0x4d2)
//   +0x4d8 s16 x4            more statuses, mount    +0x4e0 u32 flags    +0x4e5 u8 who mounted    +0x4e6, +0x4e7 u8
// and the monster's update (0x297ddc, a no-op unless this game runs the monster) turns them into HP, part damage, KO.
// 0x37260c clears them at the start of the next collision update. So the report is the change a hit made to these
// fields, and the owner adds it just before 0x297ddc, where the game's own code then does everything else (flinches,
// breaks, KOs). Aggro is separate: 0x29dd40 is recorded, and called on the owner. The list of attack numbers that have
// hit (+0x448: one hit per number and swing) isn't part of a report and isn't undone on the owner.
//
// Mixed versions: no change to the link protocol. A game that has this on says so (Hello, to all, every second and when
// it changes); an owner ignores its copy of a hunter, and applies that hunter's reports, only while it hears that
// hunter's Hello and has this on itself. A game without this drops the unknown messages (the F_AUX handlers check their
// magic number), and its hits work as they always did, in either role. Not in a Ryujinx crossplay hub: F_AUX doesn't
// travel there.
//
// Debug: MHGU_LOG_HITSYNC=1 logs each report and copy's hit and adds up per hunter what a report said against what the
// hunter's copy did on the owner (how much the original game would have lost or added). MHGU_HITSYNC_DIFF=1 instead
// logs every byte range one hit changes on the monster (for checking the list above).
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../config.h"
#include "../hle.h"
#include "net.h"

// Game data (main, v1.4.0; runtime addresses)
#define FN_HIT        0x370968u   // (hit system, attack, target, ...): one attack against one target, on every game
#define FN_EN_APPLY   0x297ddcu   // (monster): this update's hits into its HP, parts, KO, on the owner
#define FN_AGGRO      0x29dd40u   // (monster, kind 0 = hunter, slot, damage, [sp] flag): the monster's aggro
#define FN_RANK       0x267254u   // (-, reaction kind): its rank: a stronger hit replaces a weaker one's reaction
#define G_SPLAYER     0x1a85b58u  // sPlayer *: +0x1c the 4 player slots
#define G_MEMBER      0x1a8ca48u  // +0x30: the local player's slot
#define EN_INFO       0x1428u     // monster: status block pointer
#define EN_REACT      0x142cu     // monster: f32 and bytes the hit sets, read by its update (reset after 0x297ddc)
#define EN_REACT_SIZE 8u
#define EN_RUNS       0x7420u     // monster: u8, this game runs it
#define INFO_ID       0x1b6u      // u16 network id: large monsters below 10000
#define INFO_HP       0x370u
#define ZAKO_FIRST    10000u
#define AT_TYPE       0x3au       // attack: u8. 8, 9, 0x45: shots
#define AT_OWNER      0xcu        // attack: its player (with AT_VIA 0)
#define AT_VIA        0x10u       // attack: u8, 0: the weapon itself; else made by something the player threw or fired
#define AT_KIND       0x6du       // attack: u8, 1: a hunter's
#define KIND_HUNTER   1u

#define P_FIRST       0x484u      // the per-update fields: +0x484 .. +0x4ff
#define P_SIZE        0x7cu
#define P(o)          ((o) - P_FIRST)
#define P_PARTS       0x488u      // s16 x38
#define N_PARTS       38
#define P_MORE        0x4d8u      // s16 x4
#define N_MORE        4
#define P_FLAGS       0x4e0u
#define P_KIND        0x484u
#define P_DIR         0x486u
#define P_SLOT        0x4e4u
#define P_MOUNT       0x4e5u
#define P_F6          0x4e6u
#define P_F7          0x4e7u
#define P_POS         0x4f0u
#define I_FLAGS       0x3a0u
#define I_HIT         0x444u

#define MAGIC         0xE7u        // aux message (emsync.c's claims are 0xE5, hpscale.c's settings 0xE6)
enum { M_HELLO, M_REPORT };
#define HELLO_NS      1000000000ull
#define HELLO_STALE   3500000000ull
#define QUEUE         128
#define QUEUE_STALE   3000000000ull
#define MAX_ADD       16000       // a part's damage or the like from one hit: more is a damaged or hostile message

typedef struct { uint8_t magic, kind, slot, on; } Hello;

typedef struct {
    uint8_t magic, kind, slot, has_kind;
    uint16_t id, seq;
    int16_t parts[N_PARTS], more[N_MORE];
    uint32_t flags, react_f;
    uint16_t info_flags;
    uint8_t react_kind, react_b, mount, f6, f7, react_b0;
    int16_t dir;
    uint8_t react_b1, att;
    int32_t aggro, aggro_flag;
    float pos[4];
} Report;

static volatile uint64_t g_hello[4];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { Report r; uint64_t at; } g_q[QUEUE];
static volatile int g_nq;
static uint16_t g_seq;

// What the hit being run does with aggro: 1 add it up for the report, 2 drop it (the owner's copy of a reporting hunter).
static __thread int t_aggro;
static __thread int t_aggro_sum, t_aggro_flag;

static struct { uint32_t sent, sent_dmg, applied, applied_dmg, copy, copy_dmg; } g_st[4];
static bool g_log;

static bool sane(uint32_t a) { return a >= 0x08000000u && a < 0xF0000000u && !(a & 3); }
static bool active(void) { return config_live_lag_compensation() && ldn_net_up() && !ldn_crossplay(); }

static bool reports(int slot, uint64_t now) { return g_hello[slot] && now - g_hello[slot] < HELLO_STALE; }

static int local_slot(void) {
    uint32_t mem = LD32(G_MEMBER);
    int s = sane(mem) ? (int)LD32(mem + 0x30) : -1;
    return s >= 0 && s < 4 ? s : -1;
}

static int slot_of(uint32_t player) {
    uint32_t sp = LD32(G_SPLAYER);
    if (!sane(sp) || !sane(player)) return -1;
    for (int i = 0; i < 4; i++)
        if (LD32(sp + 0x1c + 4u * (uint32_t)i) == player) return i;
    return -1;
}

static bool large(uint32_t en, uint32_t *info) {
    if (!sane(en)) return false;
    uint32_t in = LD32(en + EN_INFO);
    if (!sane(in)) return false;
    *info = in;
    return LD16(in + INFO_ID) < ZAKO_FIRST;
}

typedef struct { uint8_t p[P_SIZE], react[EN_REACT_SIZE], hit; uint16_t info_flags; } Snap;

static void snap_take(Snap *s, uint32_t en, uint32_t info) {
    memcpy(s->p, G2H(info + P_FIRST), P_SIZE);
    memcpy(s->react, G2H(en + EN_REACT), EN_REACT_SIZE);
    s->hit = LD8(info + I_HIT);
    s->info_flags = LD16(info + I_FLAGS);
}

static void snap_restore(const Snap *s, uint32_t en, uint32_t info) {
    memcpy(G2H(info + P_FIRST), s->p, P_SIZE);
    memcpy(G2H(en + EN_REACT), s->react, EN_REACT_SIZE);
    ST8(info + I_HIT, s->hit);
    ST16(info + I_FLAGS, s->info_flags);
}

static int16_t at16(const uint8_t *p, uint32_t o) { int16_t v; memcpy(&v, p + P(o), 2); return v; }

static int damage_of(const int16_t *parts) {
    int d = 0;
    for (int i = 0; i < 8; i++) d += parts[i];
    return d;
}

// The change this hit made (s: the fields before it), to the other games.
static void report_send(const Snap *s, uint32_t en, uint32_t info, int slot) {
    const uint8_t *now = G2H(info + P_FIRST);
    Report r;
    memset(&r, 0, sizeof r);
    r.magic = MAGIC, r.kind = M_REPORT, r.slot = (uint8_t)slot;
    r.id = LD16(info + INFO_ID), r.seq = ++g_seq;
    bool any = t_aggro_sum != 0;
    for (int i = 0; i < N_PARTS; i++) {
        r.parts[i] = (int16_t)(at16(now, P_PARTS + 2u * (uint32_t)i) - at16(s->p, P_PARTS + 2u * (uint32_t)i));
        any |= r.parts[i] != 0;
    }
    for (int i = 0; i < N_MORE; i++) {
        r.more[i] = (int16_t)(at16(now, P_MORE + 2u * (uint32_t)i) - at16(s->p, P_MORE + 2u * (uint32_t)i));
        any |= r.more[i] != 0;
    }
    uint32_t f0, f1;
    memcpy(&f0, s->p + P(P_FLAGS), 4), memcpy(&f1, now + P(P_FLAGS), 4);
    r.flags = f1 & ~f0;
    r.info_flags = LD16(info + I_FLAGS) & (uint16_t)~s->info_flags;
    r.mount = now[P(P_MOUNT)] != s->p[P(P_MOUNT)] ? now[P(P_MOUNT)] : 0xff;
    r.f6 = now[P(P_F6)] & (uint8_t)~s->p[P(P_F6)];
    r.f7 = now[P(P_F7)] & (uint8_t)~s->p[P(P_F7)];
    any |= r.flags || r.info_flags || r.mount != 0xff || r.f6 || r.f7;
    if (memcmp(now, s->p, 4) || memcmp(now + P(P_POS), s->p + P(P_POS), 16) || now[P(P_SLOT)] != s->p[P(P_SLOT)]) {  // (this hit's reaction won)
        r.has_kind = 1;
        r.react_kind = now[P(P_KIND)], r.react_b = now[P(P_KIND) + 1];
        memcpy(&r.dir, now + P(P_DIR), 2);
        r.att = now[P(P_SLOT)];
        memcpy(r.pos, now + P(P_POS), 16);
        any = true;
    }
    if (memcmp(G2H(en + EN_REACT), s->react, EN_REACT_SIZE)) {
        r.has_kind |= 2;
        memcpy(&r.react_f, G2H(en + EN_REACT), 4);
        r.react_b0 = LD8(en + EN_REACT + 4), r.react_b1 = LD8(en + EN_REACT + 5);
    }
    r.aggro = t_aggro_sum, r.aggro_flag = t_aggro_flag;
    if (!any) return;
    ldn_send_aux(&r, sizeof r);
    int d = damage_of(r.parts);
    __atomic_add_fetch(&g_st[slot].sent, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_st[slot].sent_dmg, (uint32_t)d, __ATOMIC_RELAXED);
    if (g_log) LOG("[hitsync] report %u sent: monster %u, damage %d, KO %d, aggro %d\n", r.seq, r.id, d, r.parts[(0x4d2 - P_PARTS) / 2], r.aggro);
}

static void hit(Cpu *c) {
    if (!active()) { fnhook_call_original(c, FN_HIT); return; }
    uint32_t at = c->r[1], en = c->r[2], info;
    if (LD8(at + AT_KIND) != KIND_HUNTER || LD8(at + AT_VIA) || !large(en, &info)) { fnhook_call_original(c, FN_HIT); return; }
    uint8_t type = LD8(at + AT_TYPE);
    int slot = slot_of(LD32(at + AT_OWNER)), me = local_slot();
    bool shot = type == 8 || type == 9 || type == 0x45, owner = LD8(en + EN_RUNS) != 0;
    if (shot || slot < 0 || me < 0) { fnhook_call_original(c, FN_HIT); return; }
    Snap s;
    if (owner && slot != me && reports(slot, plat_now_ns())) {  // our copy of a hunter who reports: its hit doesn't count
        snap_take(&s, en, info);
        t_aggro = 2;
        fnhook_call_original(c, FN_HIT);
        t_aggro = 0;
        int16_t parts[8];
        const uint8_t *now = G2H(info + P_FIRST);
        for (int i = 0; i < 8; i++) parts[i] = (int16_t)(at16(now, P_PARTS + 2u * (uint32_t)i) - at16(s.p, P_PARTS + 2u * (uint32_t)i));
        snap_restore(&s, en, info);
        int d = damage_of(parts);
        __atomic_add_fetch(&g_st[slot].copy, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_st[slot].copy_dmg, (uint32_t)d, __ATOMIC_RELAXED);
        if (g_log) LOG("[hitsync] slot %d's copy hit monster %u: damage %d, ignored\n", slot, LD16(info + INFO_ID), d);
        return;
    }
    if (!owner && slot == me) {  // our hunter's hit on a monster someone else runs: to the owner
        snap_take(&s, en, info);
        t_aggro = 1, t_aggro_sum = 0, t_aggro_flag = 0;
        fnhook_call_original(c, FN_HIT);
        t_aggro = 0;
        report_send(&s, en, info, slot);
        return;
    }
    fnhook_call_original(c, FN_HIT);
}

static void aggro(Cpu *c) {
    if (t_aggro && c->r[1] == 0) {
        if (t_aggro == 2) return;  // (r0 isn't read by the hit code)
        t_aggro_sum += (int32_t)c->r[3], t_aggro_flag = (int32_t)LD32(c->r[13]);
    }
    fnhook_call_original(c, FN_AGGRO);
}

static uint32_t rank_of(Cpu *c, uint32_t kind) {
    c->r[1] = kind;
    guest_call(c, FN_RANK);
    return c->r[0];
}

// One report into the fields (on the game's thread, just before 0x297ddc reads them).
static void report_apply(Cpu *c, const Report *r, uint32_t en, uint32_t info) {
    for (int i = 0; i < N_PARTS; i++) {
        uint32_t a = info + P_PARTS + 2u * (uint32_t)i;
        ST16(a, (uint16_t)((int16_t)LD16(a) + r->parts[i]));
    }
    for (int i = 0; i < N_MORE; i++) {
        uint32_t a = info + P_MORE + 2u * (uint32_t)i;
        ST16(a, (uint16_t)((int16_t)LD16(a) + r->more[i]));
    }
    ST32(info + P_FLAGS, LD32(info + P_FLAGS) | r->flags);
    ST16(info + I_FLAGS, LD16(info + I_FLAGS) | r->info_flags);
    ST8(info + I_HIT, 1);
    if (r->mount != 0xff && LD8(info + P_MOUNT) == 0xff) ST8(info + P_MOUNT, r->mount);
    ST8(info + P_F6, LD8(info + P_F6) | r->f6);
    ST8(info + P_F7, LD8(info + P_F7) | r->f7);
    if (r->has_kind & 1) {
        uint32_t new_rank = rank_of(c, r->react_kind), cur_rank = rank_of(c, LD8(info + P_KIND));
        if (new_rank >= cur_rank) {
            ST8(info + P_KIND, r->react_kind), ST8(info + P_KIND + 1, r->react_b);
            ST16(info + P_DIR, (uint16_t)r->dir);
            ST8(info + P_SLOT, r->att);
            memcpy(G2H(info + P_POS), r->pos, 16);
        }
    }
    if (r->has_kind & 2) {
        ST32(en + EN_REACT, r->react_f);
        ST8(en + EN_REACT + 4, r->react_b0), ST8(en + EN_REACT + 5, r->react_b1);
    }
    if (r->aggro) {
        uint32_t sp = c->r[13];
        c->r[13] = sp - 8;
        ST32(c->r[13], (uint32_t)r->aggro_flag);
        c->r[0] = en, c->r[1] = 0, c->r[2] = r->slot, c->r[3] = (uint32_t)r->aggro;
        guest_call(c, FN_AGGRO);
        c->r[13] = sp;
    }
}

static void en_apply(Cpu *c) {
    uint32_t en = c->r[0], info;
    if (__atomic_load_n(&g_nq, __ATOMIC_ACQUIRE) && large(en, &info)) {
        uint32_t saved[16];
        memcpy(saved, c->r, sizeof saved);
        float s0 = c->v.f[0];
        uint16_t id = LD16(info + INFO_ID);
        bool owner = LD8(en + EN_RUNS) != 0 && config_live_lag_compensation();
        uint64_t now = plat_now_ns();
        static Report take[QUEUE];  // (one game thread runs the monsters)
        int n = 0;
        pthread_mutex_lock(&g_lock);
        int k = 0;
        for (int i = 0; i < g_nq; i++) {
            if (now - g_q[i].at > QUEUE_STALE) continue;  // (its monster never came: another zone, or gone)
            if (g_q[i].r.id == id) {
                if (owner && reports(g_q[i].r.slot, now)) take[n++] = g_q[i].r;
                continue;
            }
            if (k != i) g_q[k] = g_q[i];
            k++;
        }
        __atomic_store_n(&g_nq, k, __ATOMIC_RELEASE);
        pthread_mutex_unlock(&g_lock);
        for (int i = 0; i < n; i++) {
            report_apply(c, &take[i], en, info);
            int d = damage_of(take[i].parts), s = take[i].slot;
            __atomic_add_fetch(&g_st[s].applied, 1, __ATOMIC_RELAXED);
            __atomic_add_fetch(&g_st[s].applied_dmg, (uint32_t)d, __ATOMIC_RELAXED);
            if (g_log) LOG("[hitsync] report %u from slot %d applied: monster %u, damage %d, HP now %d\n", take[i].seq, s, id, d, (int)LD32(info + INFO_HP));
        }
        memcpy(c->r, saved, sizeof saved);
        c->v.f[0] = s0;
    }
    fnhook_call_original(c, FN_EN_APPLY);
}

static void hello_send(int me, bool on) {
    Hello m = {MAGIC, M_HELLO, (uint8_t)me, on};
    ldn_send_aux(&m, sizeof m);
}

// From the game's input polling (each frame).
void hitsync_tick(void) {
    static uint64_t last, last_log;
    static bool was;
    uint64_t now = plat_now_ns();
    bool on = active();
    int me = ldn_net_up() ? local_slot() : -1;
    if (me >= 0 && (on != was || (on && now - last >= HELLO_NS))) {
        if (on != was) LOG("[hitsync] lag compensation %s\n", on ? "on" : "off");
        hello_send(me, on);
        last = now, was = on;
    } else if (me < 0) {
        was = false;
    }
    if (g_log && now - last_log >= 10000000000ull) {
        last_log = now;
        for (int i = 0; i < 4; i++)
            if (g_st[i].sent || g_st[i].applied || g_st[i].copy)
                LOG("[hitsync] slot %d so far: reports sent %u (damage %u), applied %u (damage %u); its copy hit %u times (damage %u)\n", i, g_st[i].sent,
                    g_st[i].sent_dmg, g_st[i].applied, g_st[i].applied_dmg, g_st[i].copy, g_st[i].copy_dmg);
    }
}

void hitsync_aux_received(const void *data, uint32_t len) {
    const uint8_t *m = data;
    if (len < 2 || m[0] != MAGIC) return;
    if (m[1] == M_HELLO && len >= sizeof(Hello)) {
        Hello h;
        memcpy(&h, data, sizeof h);
        if (h.slot < 4) g_hello[h.slot] = h.on ? plat_now_ns() : 0;
        return;
    }
    if (m[1] != M_REPORT || len < sizeof(Report)) return;
    Report r;
    memcpy(&r, data, sizeof r);
    if (r.slot > 3 || r.id >= ZAKO_FIRST) return;
    for (int i = 0; i < N_PARTS; i++) if (r.parts[i] > MAX_ADD || r.parts[i] < -MAX_ADD) return;
    for (int i = 0; i < N_MORE; i++) if (r.more[i] > MAX_ADD || r.more[i] < -MAX_ADD) return;
    if (r.aggro < 0 || r.aggro > MAX_ADD) return;
    pthread_mutex_lock(&g_lock);
    if (g_nq < QUEUE) {
        g_q[g_nq].r = r, g_q[g_nq].at = plat_now_ns();
        __atomic_store_n(&g_nq, g_nq + 1, __ATOMIC_RELEASE);
    } else if (g_log) {
        LOG("[hitsync] queue full: report %u dropped\n", r.seq);
    }
    pthread_mutex_unlock(&g_lock);
}

// ---- (debug) MHGU_HITSYNC_DIFF=1
#define INFO_SIZE 0x6000u
#define EN_SIZE   0xc000u
static uint8_t g_info[INFO_SIZE], g_en[EN_SIZE];

static void log_diff(const char *what, const uint8_t *before, uint32_t guest, uint32_t size) {
    const uint8_t *now = G2H(guest);
    for (uint32_t i = 0; i < size;) {
        if (before[i] == now[i]) { i++; continue; }
        uint32_t e = i + 1;
        while (e < size && (before[e] != now[e] || (e + 1 < size && before[e + 1] != now[e + 1]))) e++;
        char a[40] = "", b[40] = "";
        for (uint32_t k = i, n = 0; k < e && n < 12; k++, n++) {
            snprintf(a + n * 2, 3, "%02x", before[k]);
            snprintf(b + n * 2, 3, "%02x", now[k]);
        }
        LOG("[hitsync]   %s +0x%x (%u): %s -> %s\n", what, i, e - i, a, b);
        i = e;
    }
}

static void hit_trace(Cpu *c) {
    uint32_t at = c->r[1], en = c->r[2], info = LD32(en + EN_INFO);
    uint8_t kind = LD8(at + AT_KIND), type = LD8(at + AT_TYPE), via = LD8(at + AT_VIA);
    memcpy(g_info, G2H(info), INFO_SIZE);
    memcpy(g_en, G2H(en), EN_SIZE);
    fnhook_call_original(c, FN_HIT);
    LOG("[hitsync] hit: kind %u type 0x%x via %u, target %08x id %u, runs it %u\n", kind, type, via, en, LD16(info + INFO_ID), LD8(en + EN_RUNS));
    log_diff("info", g_info, info, INFO_SIZE);
    log_diff("enemy", g_en, en, EN_SIZE);
}

void hitsync_init(void) {
    g_log = getenv("MHGU_LOG_HITSYNC") && atoi(getenv("MHGU_LOG_HITSYNC"));
    const char *e = getenv("MHGU_HITSYNC_DIFF");
    if (e && *e && atoi(e)) {
        fnhook_set(FN_HIT, hle_register("hitsync:trace", hit_trace));
        return;
    }
    fnhook_set(FN_HIT, hle_register("hitsync:hit", hit));
    fnhook_set(FN_AGGRO, hle_register("hitsync:aggro", aggro));
    fnhook_set(FN_EN_APPLY, hle_register("hitsync:apply", en_apply));
}
