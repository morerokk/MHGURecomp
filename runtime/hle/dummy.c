// (debug) MHGU_DUMMY=1: a training dummy. Outside the camp (nothing can be hit there) the zone's first large monster stands in
// front of the hunter, wherever the hunter faces, in the hunter's area, with its motion stopped, and every hit on it takes 1 HP, so
// it lasts. Each hit is logged with the damage it would have done: for testing attacks without chasing a monster. MHGU_DUMMY=<n>,
// n > 1: its distance in cm instead of 240 (at 350 some attacks don't reach a Volvidon). A good target: quest 404, a Volvidon.
// Hooked: the collision manager's update (just before hits are decided) and the monster's HP damage.
#include <math.h>
#include <stdlib.h>

#include "../hle.h"

#define FN_COLLISION   0x36b15cu  // the collision manager's update: attacks against targets, every update
#define FN_EN_DAMAGE   0x29d6f8u  // (enemy, part, damage, kind): takes the damage from its HP
#define FN_EN_SPEED    0x2b0870u  // (enemy, speed s0): its motion's speed
#define G_SENEMY       0x1a51fe0u // sEnemy *: +0x1c the 10 live enemies
#define G_SPLAYER      0x1a85b58u // sPlayer *: +0x1c the 4 player slots
#define G_GAMECONTROL  0x1a8ca48u // sGameControl *: +0x30 our player slot
#define EN_INFO        0x1428u    // enemy: status block: +0x40 position, +0x1b6 network id (below 10000: large)
#define OBJ_POS        0x40u      // player: position
#define OBJ_ROT        0x50u      // player: rotation (quaternion)
#define OBJ_ZONE       0x1054u    // player, enemy: the area (u8; 0 the camp)
#define MAX_EN         10

static float g_dist = 240.0f;
static uint32_t g_dummy;  // the monster held, 0 none
// MHGU_DUMMY_SLOT=<0..3>: in front of that player slot's hunter instead of our own (multiplayer: every game holds the
// monster in front of the same hunter). MHGU_DUMMY_FACE=1: the monster turned to face that hunter, so blows land on
// the head.
static int g_slot = -1;
static bool g_face;
static float g_turn = 180.0f;  // (degrees added to the hunter's heading; the control file's "dummy <cm> <degrees>")
// MHGU_DUMMY_SCALE=<x>: the held monster's model scale (object +0x60, x y z) set to x every update, so its size doesn't
// depend on the quest's random size roll. The scale it had is logged once.
static float g_scale;
// MHGU_DUMMY_AIM=<pitch>: our hunter's aim pitch (player +0x14b8 and its copy +0x14bc, -100..100) set every update, so
// shots fly along the same line in every run. The hits log the pitch either way.
static bool g_aim_set;
static float g_aim;

static uint32_t local_player(void) {
    uint32_t gc = LD32(G_GAMECONTROL), sp = LD32(G_SPLAYER);
    uint32_t slot = gc ? LD32(gc + 0x30) : 4;
    return sp && slot < 4 ? LD32(sp + 0x1c + 4 * slot) : 0;
}

static uint32_t target_player(void) {
    if (g_slot < 0) return local_player();
    uint32_t sp = LD32(G_SPLAYER);
    return sp ? LD32(sp + 0x1c + 4u * (uint32_t)g_slot) : 0;
}

static volatile int g_pin_state;  // 0 off, 1 pin at the next update, 2 pinned
static float g_pin[3];

static void hold(Cpu *c) {
    uint32_t pl = target_player(), sen = LD32(G_SENEMY), en = 0;
    g_dummy = 0;
    if (!pl || !sen || !LD8(pl + OBJ_ZONE)) return;
    for (int k = 0; k < MAX_EN && !en; k++) {
        uint32_t e = LD32(sen + 0x1c + 4u * (uint32_t)k), info = e ? LD32(e + EN_INFO) : 0;
        if (info && LD16(info + 0x1b6) < 10000) en = e;
    }
    if (!en) return;
    static uint32_t logged;
    if (logged != en) {
        logged = en;
        LOG("[dummy] holding %08x: scale %.4f %.4f %.4f\n", en, (double)LDF(en + 0x60), (double)LDF(en + 0x64), (double)LDF(en + 0x68));
    }
    if (g_scale > 0) STF(en + 0x60, g_scale), STF(en + 0x64, g_scale), STF(en + 0x68, g_scale);
    if (g_aim_set && pl == local_player()) STF(pl + 0x14b8, g_aim), STF(pl + 0x14bc, g_aim);
    // After a "dummy" control command our own hunter, when it is the one held for, stays where it was then (combos walk
    // it onto slopes, where blows meet another part) with its HP topped up.
    if (g_pin_state && pl == local_player()) {
        if (g_pin_state == 1) g_pin[0] = LDF(pl + OBJ_POS), g_pin[1] = LDF(pl + OBJ_POS + 4), g_pin[2] = LDF(pl + OBJ_POS + 8), g_pin_state = 2;
        STF(pl + OBJ_POS, g_pin[0]), STF(pl + OBJ_POS + 4, g_pin[1]), STF(pl + OBJ_POS + 8, g_pin[2]);
        ST16(pl + 0x13d6, 5000), ST16(pl + 0x13da, 5000);
    }
    float qx = LDF(pl + OBJ_ROT), qy = LDF(pl + OBJ_ROT + 4), qz = LDF(pl + OBJ_ROT + 8), qw = LDF(pl + OBJ_ROT + 12);
    float fx = 2 * (qx * qz + qw * qy), fz = 1 - 2 * (qx * qx + qy * qy);  // the hunter's forward, in the ground plane
    uint32_t pos = LD32(en + EN_INFO) + 0x40;
    STF(pos, LDF(pl + OBJ_POS) + fx * g_dist);
    STF(pos + 4, LDF(pl + OBJ_POS + 4));
    STF(pos + 8, LDF(pl + OBJ_POS + 8) + fz * g_dist);
    ST8(en + OBJ_ZONE, LD8(pl + OBJ_ZONE));
    if (g_face) {  // its heading (status +0x54, 65536 = a full turn, rebuilt into the rotation each update): the hunter's + 180 degrees
        float turn = (atan2f(fx, fz) + g_turn * 0.0174532925f) / 6.28318531f;
        ST32(LD32(en + EN_INFO) + 0x54, (uint32_t)(int32_t)(turn * 65536.0f) & 0xffffu);
    }
    uint32_t saved[16];
    float s0 = c->v.f[0];
    for (int i = 0; i < 16; i++) saved[i] = c->r[i];
    c->r[0] = en, c->v.f[0] = 0;
    guest_call(c, FN_EN_SPEED);
    for (int i = 0; i < 16; i++) c->r[i] = saved[i];
    c->v.f[0] = s0;
    g_dummy = en;
}

// MHGU_SUMMON=<cm>: the same placing, and the monster stays free: for testing how it fights without looking for it. It is
// placed once, and again after 15 s apart (it changed areas, or the hunter was knocked into another: logged, to count them).
static float g_summon;
static bool g_summoned;
static uint64_t g_apart_since;

static void summon_once(void) {
    uint32_t pl = local_player(), sen = LD32(G_SENEMY), en = 0;
    if (!pl || !sen || !LD8(pl + OBJ_ZONE) || LD8(pl + OBJ_ZONE) == 255) return;  // (the camp, or not in a quest)
    for (int k = 0; k < MAX_EN && !en; k++) {
        uint32_t e = LD32(sen + 0x1c + 4u * (uint32_t)k), info = e ? LD32(e + EN_INFO) : 0;
        if (info && LD16(info + 0x1b6) < 10000) en = e;
    }
    if (!en) return;
    if (g_summoned) {
        uint64_t now = plat_now_ns();
        if (LD8(en + OBJ_ZONE) == LD8(pl + OBJ_ZONE)) { g_apart_since = 0; return; }
        if (!g_apart_since) {
            g_apart_since = now;
            LOG("[dummy] apart: monster in zone %u, hunter in zone %u\n", LD8(en + OBJ_ZONE), LD8(pl + OBJ_ZONE));
        }
        if (now - g_apart_since < 15000000000ull) return;
        g_apart_since = 0;
    }
    float qx = LDF(pl + OBJ_ROT), qy = LDF(pl + OBJ_ROT + 4), qz = LDF(pl + OBJ_ROT + 8), qw = LDF(pl + OBJ_ROT + 12);
    float fx = 2 * (qx * qz + qw * qy), fz = 1 - 2 * (qx * qx + qy * qy);
    uint32_t pos = LD32(en + EN_INFO) + 0x40;
    STF(pos, LDF(pl + OBJ_POS) + fx * g_summon);
    STF(pos + 4, LDF(pl + OBJ_POS + 4));
    STF(pos + 8, LDF(pl + OBJ_POS + 8) + fz * g_summon);
    ST8(en + OBJ_ZONE, LD8(pl + OBJ_ZONE));
    g_summoned = true;
    LOG("[dummy] summoned the large monster %.0f cm in front of the hunter, zone %u (hunter %08x)\n", (double)g_summon,
        LD8(pl + OBJ_ZONE), pl);
}

// MHGU_SUMMON_KEEPHP=1 (with MHGU_SUMMON): the large monster's HP goes back to its maximum whenever it falls under half (on
// the game that runs it), so a long run doesn't end in a kill and the monster never reaches its low-HP behaviour; and
// every 2 s its HP and its attention scores for the 4 hunters (status +0x254) are logged.
static bool g_keephp;
static float g_stick;

static void keep_hp(void) {
    uint32_t sen = LD32(G_SENEMY);
    if (!sen) return;
    for (int k = 0; k < MAX_EN; k++) {
        uint32_t e = LD32(sen + 0x1c + 4u * (uint32_t)k), info = e ? LD32(e + EN_INFO) : 0;
        if (!info || LD16(info + 0x1b6) >= 10000) continue;
        int hp = (int)LD32(info + 0x370), max = (int)LD32(info + 0x374);
        if (LD8(e + 0x7420) && max > 0 && hp > 0 && hp < max / 2) {
            ST32(info + 0x370, (uint32_t)max);
            LOG("[dummy] keephp: monster HP %d -> %d\n", hp, max);
        }
        static uint64_t last;
        uint64_t now = plat_now_ns();
        if (now - last >= 2000000000ull) {
            last = now;
            LOG("[dummy] keephp: HP %d/%d, runs it %u, hate %d %d %d %d\n", hp, max, LD8(e + 0x7420), (int)LD32(info + 0x254),
                (int)LD32(info + 0x258), (int)LD32(info + 0x25c), (int)LD32(info + 0x260));
        }
        return;
    }
}

static uint32_t g_updates;  // collision updates so far (the hit log's clock)
static int g_walk_reset;    // (MHGU_SUMMON_WALK) a forced action started: the walk starts over from the first spot
static float g_walk_t, g_anchor[3];
static bool g_anchored;
void dummy_walk_reset(uint32_t pl) {  // (from emtrace's emact, before it places the monster: the hunter back to its spot)
    if (g_anchored && pl) STF(pl + OBJ_POS, g_anchor[0]), STF(pl + OBJ_POS + 8, g_anchor[2]);
    g_walk_reset = 1;
}

static void collision(Cpu *c) {
    g_updates++;
    if (g_summon > 0) {
        uint32_t pl = local_player();
        // (a hit since the last update: logged, for counting how often attacks land. The game now and then puts the HP back
        // to its maximum, +0x13d8: that is no hit)
        // MHGU_SUMMON_HP: 5000 (default) as below; "max": topped up to the hunter's own maximum instead (hits then show
        // as HP lost below it; a single hit above the maximum still carts); 0: left alone
        static int mode = -1;  // (0 alone, 1 5000, 2 max)
        if (mode < 0) {
            const char *m = getenv("MHGU_SUMMON_HP");
            mode = !m || !*m ? 1 : *m == '0' ? 0 : *m == 'm' ? 2 : 1;
        }
        int hp = pl ? (int)LD16(pl + 0x13d6) : 5000, hp_max = pl ? (int)LD16(pl + 0x13d8) : 0;
        int top = mode == 2 ? hp_max : 5000;
        if (pl && mode == 1 && hp < 5000 && hp != hp_max)
            LOG("[dummy] hunter hit: HP -%d at %.1f %.1f %.1f, update %u\n", hp > hp_max ? 5000 - hp : hp_max - hp,
                (double)LDF(pl + 0x40), (double)LDF(pl + 0x44), (double)LDF(pl + 0x48), g_updates);
        if (pl && mode == 2 && hp < hp_max)
            LOG("[dummy] hunter hit: HP -%d at %.1f %.1f %.1f, update %u\n", hp_max - hp, (double)LDF(pl + 0x40),
                (double)LDF(pl + 0x44), (double)LDF(pl + 0x48), g_updates);
        if (pl && mode) ST16(pl + 0x13d6, top), ST16(pl + 0x13da, top);  // (the hunter's HP and its red part: it lasts as long as the test)
        if (g_keephp) keep_hp();
        summon_once();
        // The hunter stays where the first summon found it: knocked out of the area it would be put back in the camp.
        // MHGU_SUMMON_STICK=<cm>, on a game that doesn't run the monster: the hunter instead stays that far behind the
        // monster along its own heading, so it faces the monster and its swings land (multiplayer: a joiner that hits all the time).
        static uint32_t pin_pl;
        static float pin[3];
        uint32_t sen = LD32(G_SENEMY), en = 0;
        for (int k = 0; g_stick > 0 && sen && k < MAX_EN && !en; k++) {
            uint32_t e = LD32(sen + 0x1c + 4u * (uint32_t)k), info = e ? LD32(e + EN_INFO) : 0;
            if (info && LD16(info + 0x1b6) < 10000) en = e;
        }
        if (pl && en && !LD8(en + 0x7420) && LD8(en + OBJ_ZONE) == LD8(pl + OBJ_ZONE)) {
            float qx = LDF(pl + OBJ_ROT), qy = LDF(pl + OBJ_ROT + 4), qz = LDF(pl + OBJ_ROT + 8), qw = LDF(pl + OBJ_ROT + 12);
            float fx = 2 * (qx * qz + qw * qy), fz = 1 - 2 * (qx * qx + qy * qy), n = sqrtf(fx * fx + fz * fz);
            if (pin_pl != pl) pin_pl = pl, pin[0] = LDF(pl + OBJ_POS), pin[1] = LDF(pl + OBJ_POS + 4), pin[2] = LDF(pl + OBJ_POS + 8);
            if (n > 0.01f) {
                // (no further than 1000 cm from where the hunter first stood in the monster's area: following the monster
                // anywhere can put the hunter over an area's exit, back to the camp)
                uint32_t pos = LD32(en + EN_INFO) + 0x40;
                float x = LDF(pos) - fx / n * g_stick, z = LDF(pos + 8) - fz / n * g_stick;
                float dx = x - pin[0], dz = z - pin[2], d = sqrtf(dx * dx + dz * dz);
                bool far = d > 1000.0f;
                if (far) x = pin[0] + dx * 1000.0f / d, z = pin[2] + dz * 1000.0f / d;
                STF(pl + OBJ_POS, x);
                STF(pl + OBJ_POS + 4, far ? pin[1] : LDF(pos + 4));
                STF(pl + OBJ_POS + 8, z);
            }
        } else if (pl && g_summoned && !getenv("MHGU_SUMMON_NOPIN")) {  // (MHGU_SUMMON_NOPIN=1: left where pushes put it)
            if (pin_pl != pl) pin_pl = pl, pin[0] = LDF(pl + OBJ_POS), pin[1] = LDF(pl + OBJ_POS + 4), pin[2] = LDF(pl + OBJ_POS + 8);
            // (MHGU_SUMMON_WALK=<units a second>: the pinned spot slides along +x for 3 s after each forced action
            // starts, a hunter walking past; back to the first spot at the next one, emtrace.c)
            static float walk = -1.0f;
            if (walk < 0.0f) walk = getenv("MHGU_SUMMON_WALK") ? strtof(getenv("MHGU_SUMMON_WALK"), NULL) : 0.0f;
            uint32_t sysw = LD32(0x2326764u);
            if (!g_anchored) g_anchored = true, g_anchor[0] = pin[0], g_anchor[1] = pin[1], g_anchor[2] = pin[2];
            if (g_walk_reset) g_walk_reset = 0, g_walk_t = 0.0f, pin[0] = g_anchor[0], pin[2] = g_anchor[2];
            if (walk > 0.0f && sysw && g_walk_t < 3.0f) {  // (for 3 s after each forced action starts)
                float dt = LDF(sysw + 0x68) / 60.0f;
                g_walk_t += dt, pin[0] += walk * dt;
            }
            STF(pl + OBJ_POS, pin[0]), STF(pl + OBJ_POS + 4, pin[1]), STF(pl + OBJ_POS + 8, pin[2]);
        }
    } else {
        hold(c);
    }
    fnhook_call_original(c, FN_COLLISION);
}

static void damage(Cpu *c) {
    if (c->r[0] == g_dummy) {
        uint32_t pl = local_player();
        LOG("[dummy] hit: part %u, damage %d, kind %u, update %u, %.1f ms, aim %.2f, scale %.3f\n", c->r[1], (int)c->r[2], c->r[3],
            g_updates, (double)(plat_now_ns() / 100000ull) / 10.0, pl ? (double)LDF(pl + 0x14b8) : 0.0, (double)LDF(g_dummy + 0x60));
        c->r[2] = 1;
    }
    fnhook_call_original(c, FN_EN_DAMAGE);
}

// (debug, temporary) MHGU_ATKTRACE=1: the generic attack update (0x36919c, r0 the attack): each new owner logged once with
// the attack's kind (+0x6d), type (+0x3a) and windows (+0x5c, +0x60), and whether it is the zone's large monster
#define FN_ATK_UPDATE 0x36919cu
static void atk_update(Cpu *c) {
    uint32_t at = c->r[0], owner = LD32(at + 0xc);
    if (owner && LD32(owner) == 0x19f3104u) {  // (the monster's other attacks: every update, for their lifetime)
        uint32_t sys = LD32(0x2326764u);
        LOG("[atkobj] %08x %.1f ms step %.3f windows %.2f %.2f\n", owner, (double)(plat_now_ns() / 100000ull) / 10.0,
            sys ? (double)LDF(sys + 0x68) : 0.0, (double)LDF(at + 0x5c), (double)LDF(at + 0x60));
    }
    static uint32_t seen[64];
    static int n;
    int k = 0;
    while (k < n && seen[k] != owner) k++;
    if (k == n && n < 64) {
        seen[n++] = owner;
        uint32_t sen = LD32(G_SENEMY), big = 0;
        for (int i = 0; i < MAX_EN && sen; i++) {
            uint32_t e = LD32(sen + 0x1c + 4u * (uint32_t)i), info = e ? LD32(e + EN_INFO) : 0;
            if (info && LD16(info + 0x1b6) < 10000) big = big ? big : e;
        }
        LOG("[atk] attack %08x owner %08x (vt %08x)%s%s kind %u type 0x%x windows %.2f %.2f\n", at, owner, owner ? LD32(owner) : 0,
            owner == big ? " = large monster" : "", owner == local_player() ? " = our hunter" : "", LD8(at + 0x6d), LD8(at + 0x3a),
            (double)LDF(at + 0x5c), (double)LDF(at + 0x60));
    }
    fnhook_call_original(c, FN_ATK_UPDATE);
}

void dummy_set(float dist, float turn) {
    if (dist > 0) g_dist = dist;
    g_turn = turn;
    g_pin_state = 1;
    LOG("[dummy] now %.0f cm in front, turned %.0f degrees from the hunter's heading\n", (double)g_dist, (double)g_turn);
}

// (debug) MHGU_COLLTRACE=1: the test of an attack against a target (0x36bb38: collision system, attack, target) for the
// large monster's attacks, and each shape test under it (0x368198 sphere / 0x367b5c capsule / 0x3683dc: attack shape,
// target shape, out) with both shapes and the result; =2: every shape test misses, so a whole window is traced. The
// shapes are where the update has put them: no previous positions, no sweep
static int g_in_big;
static void coll_test(Cpu *c) {
    uint32_t at = c->r[1], owner = LD32(at + 0xc);
    static int ours = -1;  // (MHGU_COLLTRACE_PL=1: our hunter's attacks instead)
    if (ours < 0) ours = getenv("MHGU_COLLTRACE_PL") != NULL;
    int big = owner && (ours ? owner == local_player() : LD32(owner) == 0x19f1d44u);
    if (big) {
        uint32_t sys = LD32(0x2326764u);
        // (+0x13cc bit 2 of the target: it is skipped, 0x47c6f8; the attack's +0x38: its motion value and type)
        LOG("[coll] %.1f ms step %.2f attack %08x open %.2f target %08x +13cc %02x atk+30 %08x +34 %08x +38 %08x kind %u\n",
            (double)(plat_now_ns() / 100000ull) / 10.0, sys ? (double)LDF(sys + 0x68) : 0.0, at, (double)LDF(at + 0x60), c->r[2],
            LD8(c->r[2] + 0x13cc), LD32(at + 0x30), LD32(at + 0x34), LD32(at + 0x38), LD8(at + 0x6d));
    }
    g_in_big = big;
    fnhook_call_original(c, 0x36bb38u);
    g_in_big = 0;
}
static void shape_log(Cpu *c, uint32_t fn, const char *kind) {
    uint32_t a = c->r[0], t = c->r[1];
    if (!g_in_big) { fnhook_call_original(c, fn); return; }
    float A[9], T[7];  // (a sphere: centre +0, radius +0xc; the others: ends +0 and +0x10, radius +0x20)
    for (int i = 0; i < 4; i++) A[i] = LDF(a + 4u * (uint32_t)i);
    for (int i = 0; i < 3; i++) A[4 + i] = LDF(a + 0x10u + 4u * (uint32_t)i);
    A[7] = LDF(a + 0x20);
    for (int i = 0; i < 3; i++) T[i] = LDF(t + 4u * (uint32_t)i), T[3 + i] = LDF(t + 0x10u + 4u * (uint32_t)i);
    T[6] = LDF(t + 0x20);
    fnhook_call_original(c, fn);
    static int nohit = -1;
    if (nohit < 0) nohit = atoi(getenv("MHGU_COLLTRACE")) == 2;
    int got = (int)c->r[0];
    if (nohit) c->r[0] = 0;  // (=2: every shape test misses, so the whole window is traced)
    LOG("[shape] %s %d: %.1f %.1f %.1f w %.1f / %.1f %.1f %.1f r %.1f | hunter %.1f %.1f %.1f - %.1f %.1f %.1f r %.1f\n", kind,
        got, (double)A[0], (double)A[1], (double)A[2], (double)A[3], (double)A[4], (double)A[5], (double)A[6],
        (double)A[7], (double)T[0], (double)T[1], (double)T[2], (double)T[3], (double)T[4], (double)T[5], (double)T[6]);
}
static void shape_sphere(Cpu *c) { shape_log(c, 0x368198u, "sphere"); }
static void shape_capsule(Cpu *c) { shape_log(c, 0x367b5cu, "capsule"); }
static void shape_three(Cpu *c) { shape_log(c, 0x3683dcu, "three"); }

// (debug) MHGU_PLCOLL=1: our hunter's attacks against monsters (0x36c0c0: collision system, attack, target; its hits go
// through 0x370968): each test with the attack's countdowns, and each hit it makes, with the damage it did to each of
// the 8 parts and where the monster was
static uint32_t g_plcoll_at;
static void plcoll_test(Cpu *c) {
    uint32_t at = c->r[1], owner = LD32(at + 0xc), sys = LD32(0x2326764u);
    bool ours = owner && owner == local_player();
    if (ours)
        LOG("[plcoll] %.1f ms step %.2f attack %08x delay %.2f open %.2f +38 %08x frame %.3f (was %.3f)\n",
            (double)(plat_now_ns() / 100000ull) / 10.0, sys ? (double)LDF(sys + 0x68) : 0.0, at, (double)LDF(at + 0x5c),
            (double)LDF(at + 0x60), LD32(at + 0x38), (double)LDF(owner + 0x4fc), (double)LDF(owner + 0x500));
    g_plcoll_at = ours ? at : 0;
    fnhook_call_original(c, 0x36c0c0u);
    g_plcoll_at = 0;
}
static void plcoll_hit(Cpu *c) {
    if (g_plcoll_at) LOG("[plcoll] hit: attack %08x open %.2f\n", g_plcoll_at, (double)LDF(g_plcoll_at + 0x60));
    static int drop = -1;  // (=2: the hit is dropped, so the attack goes on testing that target while it overlaps)
    if (drop < 0) drop = atoi(getenv("MHGU_PLCOLL")) == 2;
    if (g_plcoll_at && drop) { c->r[0] = 0; return; }
    uint32_t at = g_plcoll_at, tgt = c->r[2], info = at ? LD32(tgt + 0x1428u) : 0;
    fnhook_call_next(c, 0x370968u);
    if (at && info) {  // (the parts' damage this update, status +0x488, and where the monster is and faces)
        char buf[160];
        int n = 0;
        for (int k = 0; k < 8; k++) n += snprintf(buf + n, sizeof buf - (size_t)n, " %d", (int16_t)LD16(info + 0x488 + 2 * k));
        uint32_t pl = local_player();
        LOG("[plcoll] after: +38 %08x parts%s react %u yaw %d em %.0f %.0f pl %.0f %.0f\n", LD32(at + 0x38), buf, LD8(info + 0x484),
            (int)(LD32(info + 0x54) & 0xffff), (double)LDF(info + 0x40), (double)LDF(info + 0x48), pl ? (double)LDF(pl + 0x40) : 0.0,
            pl ? (double)LDF(pl + 0x48) : 0.0);
    }
}

void dummy_init(void) {
    if (getenv("MHGU_PLCOLL")) {
        fnhook_set(0x36c0c0u, hle_register("dummy:plcoll", plcoll_test));
        fnhook_set_outer(0x370968u, hle_register("dummy:plhit", plcoll_hit));
    }
    if (getenv("MHGU_COLLTRACE")) {
        fnhook_set(0x36bb38u, hle_register("dummy:coll", coll_test));
        fnhook_set(0x368198u, hle_register("dummy:sphere", shape_sphere));
        fnhook_set(0x367b5cu, hle_register("dummy:capsule", shape_capsule));
        fnhook_set(0x3683dcu, hle_register("dummy:three", shape_three));
    }
    // (it replaces atkstep.c's hook on the same function: debugging only)
    if (getenv("MHGU_ATKTRACE") && *getenv("MHGU_ATKTRACE") == '1') fnhook_set(FN_ATK_UPDATE, hle_register("dummy:atk", atk_update));
    const char *e = getenv("MHGU_SUMMON");
    if (e && *e && atoi(e) > 0) {
        g_summon = (float)atoi(e);
        e = getenv("MHGU_SUMMON_KEEPHP");
        g_keephp = e && *e && atoi(e);
        e = getenv("MHGU_SUMMON_STICK");
        g_stick = e && *e ? (float)atoi(e) : 0;
        fnhook_set(FN_COLLISION, hle_register("dummy:collision", collision));
        return;
    }
    e = getenv("MHGU_DUMMY");
    if (!e || !*e || !atoi(e)) return;
    if (atoi(e) > 1) g_dist = (float)atoi(e);
    const char *s = getenv("MHGU_DUMMY_SLOT");
    if (s && *s) g_slot = atoi(s) & 3;
    s = getenv("MHGU_DUMMY_FACE");
    g_face = s && *s && atoi(s);
    s = getenv("MHGU_DUMMY_SCALE");
    g_scale = s && *s ? strtof(s, NULL) : 0;
    s = getenv("MHGU_DUMMY_AIM");
    g_aim_set = s && *s, g_aim = g_aim_set ? strtof(s, NULL) : 0;
    fnhook_set(FN_COLLISION, hle_register("dummy:collision", collision));
    fnhook_set(FN_EN_DAMAGE, hle_register("dummy:damage", damage));
    LOG("[dummy] the zone's large monster stands %.0f cm in front of the hunter, outside the camp\n", (double)g_dist);
}
