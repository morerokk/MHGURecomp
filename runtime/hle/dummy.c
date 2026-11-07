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
    if (!pl || !sen || !LD8(pl + OBJ_ZONE)) return;
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
    LOG("[dummy] summoned the large monster %.0f cm in front of the hunter, zone %u\n", (double)g_summon, LD8(pl + OBJ_ZONE));
}

static void collision(Cpu *c) {
    if (g_summon > 0) {
        uint32_t pl = local_player();
        if (pl) ST16(pl + 0x13d6, 5000), ST16(pl + 0x13da, 5000);  // (the hunter's HP and its red part, above any one hit: it lasts as long as the test)
        summon_once();
        // The hunter stays where the first summon found it: knocked out of the area it would be put back in the camp.
        static uint32_t pin_pl;
        static float pin[3];
        if (pl && g_summoned) {
            if (pin_pl != pl) pin_pl = pl, pin[0] = LDF(pl + OBJ_POS), pin[1] = LDF(pl + OBJ_POS + 4), pin[2] = LDF(pl + OBJ_POS + 8);
            STF(pl + OBJ_POS, pin[0]), STF(pl + OBJ_POS + 4, pin[1]), STF(pl + OBJ_POS + 8, pin[2]);
        }
    } else {
        hold(c);
    }
    fnhook_call_original(c, FN_COLLISION);
}

static void damage(Cpu *c) {
    if (c->r[0] == g_dummy) {
        LOG("[dummy] hit: part %u, damage %d, kind %u\n", c->r[1], (int)c->r[2], c->r[3]);
        c->r[2] = 1;
    }
    fnhook_call_original(c, FN_EN_DAMAGE);
}

void dummy_set(float dist, float turn) {
    if (dist > 0) g_dist = dist;
    g_turn = turn;
    g_pin_state = 1;
    LOG("[dummy] now %.0f cm in front, turned %.0f degrees from the hunter's heading\n", (double)g_dist, (double)g_turn);
}

void dummy_init(void) {
    const char *e = getenv("MHGU_SUMMON");
    if (e && *e && atoi(e) > 0) {
        g_summon = (float)atoi(e);
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
    fnhook_set(FN_COLLISION, hle_register("dummy:collision", collision));
    fnhook_set(FN_EN_DAMAGE, hle_register("dummy:damage", damage));
    LOG("[dummy] the zone's large monster stands %.0f cm in front of the hunter, outside the camp\n", (double)g_dist);
}
