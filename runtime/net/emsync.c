// Small monsters synced in multiplayer (ini [Multiplayer] SyncSmallMonsters). The game syncs only large monsters:
// each has an owner player whose game runs its AI and sends every new action with its full state (netEmAction),
// the others replay it. Small monsters ("zako", network ids 10000 + a quest-wide spawn index) get the same code but
// no owner: every game runs them itself, so each player sees them elsewhere. Here, per zone, one player becomes their
// owner (the first one there; claims below) and they get what a large monster has on every game: flag 8 (networked),
// and who runs it (EN_RUNS, EN_OWNER): the others' copies replay what arrives. Alone in a zone, a player's small
// monsters behave as the game made them. A filter in the game's per-monster receive (tools/gen.py
// PATCHES, 0x2cb120) drops small monster messages from a player in another zone and
// those reaching the master. Details: docs/research/small-monster-sync.md.
#include <stdlib.h>
#include <string.h>

#include "../config.h"
#include "../hle.h"
#include "net.h"

// Game data (main, v1.4.0; runtime addresses)
#define G_SENEMY      0x1a51fe0u  // sEnemy *: +0x1c: the 10 live enemies of the loaded zone
#define G_SPLAYER     0x1a85b58u  // sPlayer *: +0x1c: the 4 player slots
#define G_MEMBER      0x1a8ca48u  // +0x30: the local player's slot
#define EN_INFO       0x1428u     // enemy: status block pointer
#define INFO_FLAGS    0x1a8u      // u32: 8 = networked (actions are sent). Others are species data, e.g. 0x80: no attention scores
#define INFO_ID       0x1b6u      // u16: network id (0xffff none; small monsters 10000+)
#define EN_ACTION     0x73e0u     // enemy: the current action and the last one (0: its first action, 0:0, and none before)
#define EN_AREA       0x1054u     // enemy: its zone, u8 (as PL_AREA)
#define EN_RUNS       0x7420u    // enemy: u8, this game runs its AI (0: it only replays what arrives)
#define EN_TOOK       0x7423u     // enemy: u8, 0x2b16e0 has been: the session host's game takes a networked monster, once
#define EN_OWNER      0x7424u     // enemy: u8, the slot of the player who runs it
#define PL_AREA       0x1054u     // player: current area (zone) number, u8 0..12 (setter: uPlayer vtable +0xa4)
#define F_NET         0x8u
#define ZAKO_FIRST    10000u

static int g_area = -1;
static bool g_master, g_shared;  // runs the local zone's small monsters / someone else is in this zone

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

// A small monster as the game made it (no owner, this game runs it), or owned by a player (the local one or not).
// A monster the game has retired (0x2af368: owner 0xff, nobody runs it) stays so.
static void make_local(uint32_t en, uint32_t info, int me) {
    if (!(LD32(info + INFO_FLAGS) & F_NET) || LD8(en + EN_OWNER) == 0xff) return;  // (never networked: nothing to undo)
    set_flags(info, 0, F_NET);
    ST8(en + EN_RUNS, 1), ST8(en + EN_OWNER, me);
}

static void make_owned(uint32_t en, uint32_t info, int me, int owner) {
    if (LD8(en + EN_OWNER) == 0xff) return;
    ST8(en + EN_TOOK, 1);  // (the owner is settled here)
    set_flags(info, F_NET, 0);
    ST8(en + EN_RUNS, owner == me), ST8(en + EN_OWNER, owner);
}

// A monster joins the sync once it has left its first action: uEms100 (file ems101, species 0x1065: Volcanic Hollow's
// breakable rock, e.g. two in zone 7; ems100-106 are each map's) starts 0:0 again every update while whole, and
// networked it would send each one (hundreds a second: players drop). Such a monster stays as the game made it. (Others
// pass through 0:0 for a moment, so the action word alone can't tell.) Keyed by the network id too: a new monster can
// get the address and list place of one that left. The send cap below catches whatever this misses.
static struct { uint32_t en; uint16_t id; bool left_first; } g_seen[10];
static int g_noguard = -1;  // (debug) MHGU_EMSYNC_NOGUARD=1: no such check (the rocks flood again: tests the cap)

static bool left_first_action(int k, uint32_t en, uint32_t info) {
    uint16_t id = LD16(info + INFO_ID);
    if (g_noguard < 0) {
        const char *e = getenv("MHGU_EMSYNC_NOGUARD");
        g_noguard = e && atoi(e);
    }
    if (g_noguard) return true;
    if (g_seen[k].en != en || g_seen[k].id != id) g_seen[k].en = en, g_seen[k].id = id, g_seen[k].left_first = false;
    if (LD32(en + EN_ACTION) != 0) g_seen[k].left_first = true;
    return g_seen[k].left_first;
}

// Who runs a zone's small monsters: claims, exchanged over the local play link (ldn_send_aux) twice a second and on
// every change. The player already in a zone keeps it (a newcomer's freshly spawned monsters follow the ones that
// have been running there); two claims for one zone (players arriving together): the lower slot keeps it.
typedef struct { uint8_t magic, slot, area, claim; } Claim;  // claim: 1 = runs `area`'s small monsters
#define CLAIM_MAGIC 0xE5
#define CAP_MAGIC   0xE9  // (the flood cap below; hpscale.c 0xE6, hitsync.c 0xE7, partsync.c 0xE8)
#define CLAIM_STALE_NS 2500000000ull
static struct { volatile uint64_t at; volatile uint8_t area, claim; } g_claims[4];
static bool g_claiming;  // our own claim (kept while we stay in the zone)

static void cap_received(const void *data, uint32_t len);

void emsync_aux_received(const void *data, uint32_t len) {
    Claim m;
    if (len < sizeof m) return;
    memcpy(&m, data, sizeof m);
    if (m.magic == CAP_MAGIC) { cap_received(data, len); return; }
    if (m.magic != CLAIM_MAGIC || m.slot > 3) return;
    g_claims[m.slot].area = m.area, g_claims[m.slot].claim = m.claim;
    g_claims[m.slot].at = plat_now_ns();
}

static bool claims_area(int slot, int area, uint64_t now) {
    return g_claims[slot].claim && g_claims[slot].area == area && now - g_claims[slot].at < CLAIM_STALE_NS;
}

static int my_slot(void) {
    uint32_t mem = LD32(G_MEMBER);
    int me = sane(mem) ? (int)LD32(mem + 0x30) : -1;
    return me >= 0 && me <= 3 ? me : 0;
}

// Flood cap: whatever the cause (a monster restarting its action every update like the rocks above, or one not found
// yet), a small monster that sends more than CAP_PER_SEC actions in a second, or whose messages arrive faster than that,
// leaves the sync: its messages are dropped and this game runs it as the game made it (make_local). The owner keeps it
// so for the rest of its stay in the zone and tells the others twice a second (CAP_MAGIC, an aux message older builds
// ignore), who do the same while they hear it. Normal small monsters start a few actions a second (busiest seen: 9 in a
// second, species 0x1013, Volcanic Hollow); the rocks' loop was 2 per update (120 a second at 60 fps, 480 at 240), and a
// player dropped after ~576 of them, so the burst before the cap stays far below that. Large monsters are never touched.
// Checked with sync on or off: with it off, nothing is sent, but a flood from another game is still dropped.
#define FN_EM_SEND   0x2cafe8u           // (monster, kind, data): a networked monster's message; kind 1: netEmAction
#define CAP_PER_SEC  20
#define CAP_HOLD_NS  5000000000ull       // a cap heard from another game, or one from incoming messages, lasts this long
#define CAP_OURS     0xffffffffffffffffull  // (until: ours, for this stay in the zone)
typedef struct { uint8_t magic, slot, area, pad; uint16_t id; } CapMsg;
static pthread_mutex_t g_cap_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { uint16_t id, n; uint64_t t0; } g_rate[16];
static struct { uint16_t id; uint64_t until; } g_cap[16];
static int g_cap_limit = CAP_PER_SEC;  // (debug) MHGU_EMSYNC_CAP=<n>

static int cap_find(uint16_t id, uint64_t now) {  // (g_cap_mu held)
    for (int i = 0; i < 16; i++)
        if (g_cap[i].id == id && now < g_cap[i].until) return i;
    return -1;
}

static void cap_add(uint16_t id, uint64_t until, uint64_t now) {  // (g_cap_mu held)
    int i = cap_find(id, now);
    if (i < 0) {
        for (i = 0; i < 16 && now < g_cap[i].until; i++) {}
        if (i == 16) i = 0;  // (16 at once: never seen; the first one goes)
        g_cap[i].id = id, g_cap[i].until = 0;
    }
    if (until > g_cap[i].until) g_cap[i].until = until;
}

static bool over_rate(uint16_t id, uint64_t now) {  // (g_cap_mu held) counts one message
    int i, old = 0;
    for (i = 0; i < 16 && g_rate[i].id != id; i++)
        if (g_rate[i].t0 < g_rate[old].t0) old = i;
    if (i == 16) i = old, g_rate[i].id = id, g_rate[i].n = 0, g_rate[i].t0 = now;
    if (now - g_rate[i].t0 >= 1000000000ull) g_rate[i].t0 = now, g_rate[i].n = 0;
    return ++g_rate[i].n > g_cap_limit;
}

static bool capped(uint16_t id) {
    pthread_mutex_lock(&g_cap_mu);
    bool r = cap_find(id, plat_now_ns()) >= 0;
    pthread_mutex_unlock(&g_cap_mu);
    return r;
}

static void cap_clear(void) {  // (a new zone, or the sync off)
    pthread_mutex_lock(&g_cap_mu);
    memset(g_cap, 0, sizeof g_cap);
    memset(g_rate, 0, sizeof g_rate);
    pthread_mutex_unlock(&g_cap_mu);
}

static void cap_announce(uint16_t id) {
    CapMsg m = {CAP_MAGIC, (uint8_t)my_slot(), (uint8_t)g_area, 0, id};
    ldn_send_aux(&m, sizeof m);
}

static void cap_log(const char *how, uint32_t en, uint32_t info) {
    LOG("[emsync] small monster %u (species %x) %s more than %d messages a second: left out of the sync in area %d\n",
        LD16(info + INFO_ID), LD32(en + 0xb5f0), how, g_cap_limit, g_area);
}

static void em_send(Cpu *c) {
    uint32_t en = c->r[0], info;
    if (c->r[1] == 1 && is_zako(en, &info)) {
        uint16_t id = LD16(info + INFO_ID);
        uint64_t now = plat_now_ns();
        pthread_mutex_lock(&g_cap_mu);
        bool cap = cap_find(id, now) >= 0, fresh = false;
        if (!cap && over_rate(id, now)) cap_add(id, CAP_OURS, now), cap = fresh = true;
        pthread_mutex_unlock(&g_cap_mu);
        if (cap) {
            make_local(en, info, my_slot());
            if (fresh) {
                cap_log("sent", en, info);
                if (ldn_net_up()) cap_announce(id);
            }
            RET(0);  // (dropped)
            return;
        }
    }
    fnhook_call_next(c, FN_EM_SEND);
}

// Another game left one of this zone's small monsters out (it hears its own messages no more than we do)
static void cap_received(const void *data, uint32_t len) {
    CapMsg m;
    if (len < sizeof m) return;
    memcpy(&m, data, sizeof m);
    if (m.slot > 3 || m.id < ZAKO_FIRST || m.id == 0xffff || g_area < 0 || m.area != g_area) return;
    uint64_t now = plat_now_ns();
    pthread_mutex_lock(&g_cap_mu);
    bool fresh = cap_find(m.id, now) < 0;
    cap_add(m.id, now + CAP_HOLD_NS, now);
    pthread_mutex_unlock(&g_cap_mu);
    if (fresh) LOG("[emsync] small monster %u left out of the sync in area %d by slot %u's game\n", m.id, g_area, m.slot);
}

void emsync_init(void) {
    const char *e = getenv("MHGU_EMSYNC_CAP");
    if (e && atoi(e) > 0) g_cap_limit = atoi(e);
    fnhook_set_outer(FN_EM_SEND, hle_register("emsync:send", em_send));
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
    bool on = config_live_sync_small_monsters(), up = ldn_net_up() && !ldn_crossplay();  // (crossplay: no small monster sync)
    if ((!on || !up) && g_area >= 0) {  // (turned off in the settings menu, or the link is gone: this zone's small monsters as the game runs them)
        int me = sane(mem) ? (int)LD32(mem + 0x30) : -1;
        for (int k = 0; sane(sen) && k < 10; k++) {
            uint32_t en = LD32(sen + 0x1c + 4u * (uint32_t)k), info;
            if (is_zako(en, &info)) make_local(en, info, me >= 0 && me <= 3 ? me : 0);
        }
        if (me >= 0 && me <= 3 && up) {
            Claim m = {CLAIM_MAGIC, (uint8_t)me, (uint8_t)g_area, 0};
            ldn_send_aux(&m, sizeof m);
        }
        LOG("[emsync] %s: area %d's small monsters local (as the game does)\n", on ? "link down" : "SyncSmallMonsters off", g_area);
        sent_area = -2, sent_claim = -1, was = -1;
        cap_clear();
    }
    if (!on || !up) { g_area = -1, g_claiming = false; return; }
    if (!sane(sen) || !sane(spl) || !sane(mem)) { g_area = -1, g_claiming = false; return; }
    int me = (int)LD32(mem + 0x30);
    if (me < 0 || me > 3) return;  // (keep the last state: a passing glitch)
    areas_update(spl, now);
    int area = slot_area(spl, me), here = 0;
    if (area != g_area) {  // (a new zone: claims and caps start over)
        g_claiming = false;
        memset(g_seen, 0, sizeof g_seen);
        cap_clear();
    }
    bool other_claim = false, lower_claim = false;
    int owner = me;
    for (int i = 0; i < 4; i++) {
        if (slot_area(spl, i) != area) continue;
        here++;
        if (i != me && claims_area(i, area, now)) {
            if (!other_claim || i < owner) owner = i;
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
        uint16_t ours[16];
        int n = 0;
        pthread_mutex_lock(&g_cap_mu);
        for (int i = 0; i < 16; i++)
            if (g_cap[i].until == CAP_OURS) ours[n++] = g_cap[i].id;
        pthread_mutex_unlock(&g_cap_mu);
        for (int i = 0; i < n; i++) cap_announce(ours[i]);  // (the others keep theirs local while they hear this)
    }
    g_area = area, g_shared = here > 1, g_master = g_claiming;
    int state = !g_shared ? 0 : g_master ? 1 : other_claim ? 2 : 0;
    if (state != was || ENV("MHGU_LOG_EMSYNC")) {
        if (state != was)
            LOG("[emsync] slot %d, area %d, %d player(s) here: small monsters %s\n", me, area, here,
                state == 0 ? "local (as the game does)" : state == 1 ? "run here, sent to the others" : "follow their owner");
        was = state;
    }
    for (int k = 0; k < 10; k++) {
        uint32_t en = LD32(sen + 0x1c + 4u * (uint32_t)k), info;
        if (!is_zako(en, &info)) continue;
        if (state == 0 || LD8(en + EN_AREA) != area || !left_first_action(k, en, info) || capped(LD16(info + INFO_ID)))
            make_local(en, info, me);  // (the list keeps some of the zone just left)
        else make_owned(en, info, me, state == 1 ? me : owner);
    }
}

// The patched entry of the per-monster receive (0x2cb120: r0 = monster, r2 = the message's parameters, byte 1 = the
// sender's slot, byte 3 the kind: 1 = netEmAction). True: drop the message. A small monster's flood of actions is dropped
// whatever the setting (the flood cap above); other kinds (HP...) go on as before.
int emsync_filter(Cpu *c) {
    uint32_t info;
    if (!is_zako(c->r[0], &info)) return 0;
    bool cap = false, fresh = false;
    if (sane(c->r[2] & ~3u) && LD8(c->r[2] + 3) == 1) {
        uint16_t id = LD16(info + INFO_ID);
        uint64_t now = plat_now_ns();
        pthread_mutex_lock(&g_cap_mu);
        cap = cap_find(id, now) >= 0;
        if (over_rate(id, now)) cap_add(id, now + CAP_HOLD_NS, now), fresh = !cap, cap = true;  // (held while it goes on)
        pthread_mutex_unlock(&g_cap_mu);
    }
    if (cap) {
        make_local(c->r[0], info, my_slot());
        if (fresh) cap_log("received", c->r[0], info);
        return 1;
    }
    if (g_area < 0 || !config_live_sync_small_monsters()) return 0;
    if (g_master) return 1;  // (a moment of two owners: keep ours)
    uint32_t spl = LD32(G_SPLAYER);
    int sender = LD8(c->r[2] + 1);
    if (!sane(spl) || sender < 0 || sender > 3) return 1;
    return slot_area(spl, sender) != g_area;
}
