// Small monsters synced in multiplayer (ini [Multiplayer] SyncSmallMonsters). The game syncs only large monsters:
// each has a master player whose game runs its AI and sends every new action with its full state (netEmAction),
// the others replay it. Small monsters ("zako", network ids 10000 + a quest-wide spawn index) get the same code but
// never a master: every game runs them itself (flag 0x80, "simulate locally"), so each player sees them elsewhere.
// Here, per zone, one player becomes their master (the first one there; claims below) (flag 8: the game then
// sends their actions), the others' copies follow (0x80 cleared: they replay what arrives); alone in a zone, a
// player's small monsters behave as the game made them. A filter in the game's per-monster receive (tools/gen.py
// PATCHES, 0x2cb120) drops small monster messages from a player in another zone and
// those reaching the master. Details: docs/research/small-monster-sync.md.
#include <string.h>

#include "../config.h"
#include "../hle.h"
#include "net.h"

// Game data (main, v1.4.0; runtime addresses)
#define G_SENEMY      0x1a51fe0u  // sEnemy *: +0x1c: the 10 live enemies of the loaded zone
#define G_SPLAYER     0x1a85b58u  // sPlayer *: +0x1c: the 4 player slots
#define G_MEMBER      0x1a8ca48u  // +0x30: the local player's slot
#define EN_INFO       0x1428u     // enemy: status block pointer
#define INFO_FLAGS    0x1a8u      // u32: 8 = master (sends), 0x80 = simulated locally (small monsters)
#define INFO_ID       0x1b6u      // u16: network id (0xffff none; small monsters 10000+)
#define EN_ACTION     0x73e0u     // enemy: the current action and the last one (0: its first action, 0:0, and none before)
#define PL_AREA       0x1054u     // player: current area (zone) number, u8 0..12 (setter: uPlayer vtable +0xa4)
#define F_MASTER      0x8u
#define F_LOCAL       0x80u
#define ZAKO_FIRST    10000u

static int g_area = -1;
static bool g_master, g_shared;  // master of the local zone's small monsters / someone else is in this zone

static bool sane(uint32_t a) { return a >= 0x08000000u && a < 0xF0000000u && !(a & 3); }

static bool is_zako(uint32_t en, uint32_t *info) {
    if (!sane(en)) return false;
    uint32_t in = LD32(en + EN_INFO);
    if (!sane(in)) return false;
    uint16_t id = LD16(in + INFO_ID);
    *info = in;
    return id >= ZAKO_FIRST && id != 0xffff;
}

// The zone of a player slot, -1: no such player.
static int slot_area_raw(uint32_t splayer, int i) {
    uint32_t p = LD32(splayer + 0x1c + 4u * (uint32_t)i);
    return sane(p) ? (int)LD8(p + PL_AREA) : -1;
}

// The same, debounced: a new value counts once it has been read for STABLE_NS. (Seen in tests: a value that differs
// for a moment flipped the zone's master between two players standing still.)
#define STABLE_NS 700000000ull
static struct { int stable, cand; uint64_t since; } g_sa[4] = {{-1, -1, 0}, {-1, -1, 0}, {-1, -1, 0}, {-1, -1, 0}};

static void areas_update(uint32_t splayer, uint64_t now) {
    for (int i = 0; i < 4; i++) {
        int v = slot_area_raw(splayer, i);
        if (v == g_sa[i].stable) {
            if (g_sa[i].cand != v && now - g_sa[i].since > 50000000ull)
                LOG("[emsync] slot %d: area read %d for %llu ms, ignored\n", i, g_sa[i].cand,
                    (unsigned long long)((now - g_sa[i].since) / 1000000));
            g_sa[i].cand = v;
            continue;
        }
        if (v != g_sa[i].cand) g_sa[i].cand = v, g_sa[i].since = now;
        else if (now - g_sa[i].since >= STABLE_NS || g_sa[i].stable == -2) g_sa[i].stable = v;
    }
}
static int slot_area(uint32_t splayer, int i) { (void)splayer; return g_sa[i].stable; }

static void set_flags(uint32_t info, uint32_t set, uint32_t clear) {
    uint32_t *f = (uint32_t *)G2H(info + INFO_FLAGS);
    uint32_t v = __atomic_load_n(f, __ATOMIC_RELAXED), n;
    do {
        n = (v | set) & ~clear;
        if (n == v) return;
    } while (!__atomic_compare_exchange_n(f, &v, n, true, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED));
}

// Who runs a zone's small monsters: claims, exchanged over the local play link (ldn_send_aux) twice a second and on
// every change. The player already in a zone keeps it (a newcomer's freshly spawned monsters follow the ones that
// have been running there); two claims for one zone (players arriving together): the lower slot keeps it.
typedef struct { uint8_t magic, slot, area, claim; } Claim;  // claim: 1 = runs `area`'s small monsters
#define CLAIM_MAGIC 0xE5
#define CLAIM_STALE_NS 2500000000ull
static struct { volatile uint64_t at; volatile uint8_t area, claim; } g_claims[4];
static bool g_claiming;  // our own claim (kept while we stay in the zone)

void emsync_aux_received(const void *data, uint32_t len) {
    Claim m;
    if (len < sizeof m) return;
    memcpy(&m, data, sizeof m);
    if (m.magic != CLAIM_MAGIC || m.slot > 3) return;
    g_claims[m.slot].area = m.area, g_claims[m.slot].claim = m.claim;
    g_claims[m.slot].at = plat_now_ns();
}

static bool claims_area(int slot, int area, uint64_t now) {
    return g_claims[slot].claim && g_claims[slot].area == area && now - g_claims[slot].at < CLAIM_STALE_NS;
}

// From the game's input polling (each frame, on the game's thread): who is master here, and the flags that follow.
void emsync_tick(void) {
    static uint64_t last, last_sent;
    static int sent_area = -2, sent_claim = -1;
    uint64_t now = plat_now_ns();
    if (now - last < 33000000ull) return;
    last = now;
    static int was = -1;  // last logged state
    uint32_t sen = LD32(G_SENEMY), spl = LD32(G_SPLAYER), mem = LD32(G_MEMBER);
    bool on = config_live_sync_small_monsters();
    if (!on && g_area >= 0) {  // (turned off in the settings menu: this zone's small monsters as the game runs them)
        for (int k = 0; sane(sen) && k < 10; k++) {
            uint32_t en = LD32(sen + 0x1c + 4u * (uint32_t)k), info;
            if (is_zako(en, &info)) set_flags(info, F_LOCAL, F_MASTER);
        }
        int me = sane(mem) ? (int)LD32(mem + 0x30) : -1;
        if (me >= 0 && me <= 3 && ldn_net_up()) {
            Claim m = {CLAIM_MAGIC, (uint8_t)me, (uint8_t)g_area, 0};
            ldn_send_aux(&m, sizeof m);
        }
        LOG("[emsync] SyncSmallMonsters off: area %d's small monsters local (as the game does)\n", g_area);
        sent_area = -2, sent_claim = -1, was = -1;
    }
    if (!on || !ldn_net_up()) { g_area = -1, g_claiming = false; return; }
    if (!sane(sen) || !sane(spl) || !sane(mem)) { g_area = -1, g_claiming = false; return; }
    int me = (int)LD32(mem + 0x30);
    if (me < 0 || me > 3) return;  // (keep the last state: a passing glitch)
    areas_update(spl, now);
    int area = slot_area(spl, me), here = 0;
    if (area != g_area) g_claiming = false;  // (a new zone: claims start over)
    bool other_claim = false, lower_claim = false;
    for (int i = 0; i < 4; i++) {
        if (slot_area(spl, i) != area) continue;
        here++;
        if (i != me && claims_area(i, area, now)) {
            other_claim = true;
            if (i < me) lower_claim = true;
        }
    }
    if (g_claiming && lower_claim) g_claiming = false;  // (arrived together: the lower slot keeps it)
    if (!g_claiming && !other_claim && area >= 0 && area <= 12) g_claiming = true;  // nobody runs this zone: we do
    if (g_claiming != (sent_claim == 1) || area != sent_area || now - last_sent > 500000000ull) {
        Claim m = {CLAIM_MAGIC, (uint8_t)me, (uint8_t)area, g_claiming};
        ldn_send_aux(&m, sizeof m);
        sent_area = area, sent_claim = g_claiming, last_sent = now;
    }
    g_area = area, g_shared = here > 1, g_master = g_claiming;
    int state = !g_shared ? 0 : g_master ? 1 : 2;
    if (state != was || ENV("MHGU_LOG_EMSYNC")) {
        if (state != was)
            LOG("[emsync] slot %d, area %d, %d player(s) here: small monsters %s\n", me, area, here,
                state == 0 ? "local (as the game does)" : state == 1 ? "run here, sent to the others" : "follow the master");
        was = state;
    }
    for (int k = 0; k < 10; k++) {
        uint32_t en = LD32(sen + 0x1c + 4u * (uint32_t)k), info;
        if (!is_zako(en, &info)) continue;
        // A monster that never leaves its first action stays as the game made it: uEms100 (Volcanic Hollow zones 2, 3, 7)
        // starts 0:0 again every update, and as a master's it would send each one (hundreds a second: players drop).
        if (LD32(en + EN_ACTION) == 0) { set_flags(info, F_LOCAL, F_MASTER); continue; }
        if (state == 1) set_flags(info, F_MASTER | F_LOCAL, 0);
        else if (state == 2) set_flags(info, 0, F_MASTER | F_LOCAL);
        else set_flags(info, F_LOCAL, F_MASTER);
    }
}

// The patched entry of the per-monster receive (0x2cb120: r0 = monster, r2 = the message's parameters, byte 1 = the
// sender's slot). True: drop the message.
int emsync_filter(Cpu *c) {
    if (g_area < 0 || !config_live_sync_small_monsters()) return 0;
    uint32_t info;
    if (!is_zako(c->r[0], &info)) return 0;
    if (g_master) return 1;  // (a moment of two masters: keep ours)
    uint32_t spl = LD32(G_SPLAYER);
    int sender = LD8(c->r[2] + 1);
    if (!sane(spl) || sender < 0 || sender > 3) return 1;
    return slot_area(spl, sender) != g_area;
}
