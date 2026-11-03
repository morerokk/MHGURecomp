// (debug) MHGU_DUMMY=1: a training dummy. Outside the camp (nothing can be hit there) the zone's first large monster stands in
// front of the hunter, wherever the hunter faces, in the hunter's area, with its motion stopped, and every hit on it takes 1 HP, so
// it lasts. Each hit is logged with the damage it would have done: for testing attacks without chasing a monster. MHGU_DUMMY=<n>,
// n > 1: its distance in cm instead of 240 (at 350 some attacks don't reach a Volvidon). A good target: quest 404, a Volvidon.
// Hooked: the collision manager's update (just before hits are decided) and the monster's HP damage.
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

static uint32_t local_player(void) {
    uint32_t gc = LD32(G_GAMECONTROL), sp = LD32(G_SPLAYER);
    uint32_t slot = gc ? LD32(gc + 0x30) : 4;
    return sp && slot < 4 ? LD32(sp + 0x1c + 4 * slot) : 0;
}

static void hold(Cpu *c) {
    uint32_t pl = local_player(), sen = LD32(G_SENEMY), en = 0;
    g_dummy = 0;
    if (!pl || !sen || !LD8(pl + OBJ_ZONE)) return;
    for (int k = 0; k < MAX_EN && !en; k++) {
        uint32_t e = LD32(sen + 0x1c + 4u * (uint32_t)k), info = e ? LD32(e + EN_INFO) : 0;
        if (info && LD16(info + 0x1b6) < 10000) en = e;
    }
    if (!en) return;
    float qx = LDF(pl + OBJ_ROT), qy = LDF(pl + OBJ_ROT + 4), qz = LDF(pl + OBJ_ROT + 8), qw = LDF(pl + OBJ_ROT + 12);
    float fx = 2 * (qx * qz + qw * qy), fz = 1 - 2 * (qx * qx + qy * qy);  // the hunter's forward, in the ground plane
    uint32_t pos = LD32(en + EN_INFO) + 0x40;
    STF(pos, LDF(pl + OBJ_POS) + fx * g_dist);
    STF(pos + 4, LDF(pl + OBJ_POS + 4));
    STF(pos + 8, LDF(pl + OBJ_POS + 8) + fz * g_dist);
    ST8(en + OBJ_ZONE, LD8(pl + OBJ_ZONE));
    uint32_t saved[16];
    float s0 = c->v.f[0];
    for (int i = 0; i < 16; i++) saved[i] = c->r[i];
    c->r[0] = en, c->v.f[0] = 0;
    guest_call(c, FN_EN_SPEED);
    for (int i = 0; i < 16; i++) c->r[i] = saved[i];
    c->v.f[0] = s0;
    g_dummy = en;
}

static void collision(Cpu *c) {
    hold(c);
    fnhook_call_original(c, FN_COLLISION);
}

static void damage(Cpu *c) {
    if (c->r[0] == g_dummy) {
        LOG("[dummy] hit: part %u, damage %d, kind %u\n", c->r[1], (int)c->r[2], c->r[3]);
        c->r[2] = 1;
    }
    fnhook_call_original(c, FN_EN_DAMAGE);
}

void dummy_init(void) {
    const char *e = getenv("MHGU_DUMMY");
    if (!e || !*e || !atoi(e)) return;
    if (atoi(e) > 1) g_dist = (float)atoi(e);
    fnhook_set(FN_COLLISION, hle_register("dummy:collision", collision));
    fnhook_set(FN_EN_DAMAGE, hle_register("dummy:damage", damage));
    LOG("[dummy] the zone's large monster stands %.0f cm in front of the hunter, outside the camp\n", (double)g_dist);
}
