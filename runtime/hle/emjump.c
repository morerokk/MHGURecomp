// Monster jumps that move by a fixed amount per game update (tools/gen.py PATCHES, "Monster jump physics").
// Some actions move the monster with the velocity / acceleration in its status block (*(monster + 0x1428): +0x40
// position, +0x10 velocity, +0x20 acceleration) and integrate them once per update, without the frame step:
//     pos += vel; vel += acc; acc.y += -0.2   (the last one in some of them)
// tuned for the Switch's 30 updates a second. At 60 fps the jump plays twice as fast, at 120 four times (the same
// height, a quarter of the airtime): Volvidon's body press, the Arzuros / Lagombi / Volvidon hops, a Zinogre move.
// The patched stores advance the same sequence by s = step / 2 updates instead (step: the engine's frame step in
// 60 fps frames, so s = 1 at 30 fps): Newton's forward differences, exact for this cubic sequence, so the monster
// is where the original 30 fps game has it at every 30 fps frame. s is at most 1 (below 30 fps as the original).
// The stores come in the original order (all positions, then velocities, then the acceleration), so each one still
// sees the values from before the update.
#include <stdlib.h>

#include "../hle.h"

#define G_SYSTEM 0x2326764u  // engine system object *: +0x68 the frame step in 60 fps frames

static float step_s(void) {
    static int off = -1;  // MHGU_NO_EMJUMP=1: the original per-update jumps (for comparing)
    if (off < 0) off = getenv("MHGU_NO_EMJUMP") && *getenv("MHGU_NO_EMJUMP") == '1';
    if (off) return 1.0f;
    uint32_t sys = LD32(G_SYSTEM);
    float s = sys ? LDF(sys + 0x68) * 0.5f : 1.0f;
    return s > 0.0f && s < 1.0f ? s : 1.0f;
}

// r0: the status block; axis 0..2; jerk: the acceleration's change per update on that axis
void emjump_pos(Cpu *c, int axis, float jerk) {
    uint32_t b = c->r[0] + 4u * (uint32_t)axis;
    float s = step_s(), c2 = s * (s - 1) * 0.5f, c3 = c2 * (s - 2) / 3.0f;
    STF(b + 0x40, LDF(b + 0x40) + s * LDF(b + 0x10) + c2 * LDF(b + 0x20) + c3 * jerk);
}

void emjump_vel(Cpu *c, int axis, float jerk) {
    uint32_t b = c->r[0] + 4u * (uint32_t)axis;
    float s = step_s();
    STF(b + 0x10, LDF(b + 0x10) + s * LDF(b + 0x20) + s * (s - 1) * 0.5f * jerk);
}

void emjump_acc(Cpu *c, int axis, float jerk) {
    uint32_t b = c->r[0] + 4u * (uint32_t)axis;
    STF(b + 0x20, LDF(b + 0x20) + step_s() * jerk);
}

// Uroktor's leap out of the ground (ems040, action 7:44; the action's update 0x13085e8, sub-state 1 -> 2): when the
// drill motion ends it stores a launch speed (size x 12.5) to the vertical velocity, moves the monster once with it
// (0x27d718: pos += vel * t + acc * t^2 / 2, t = its step, monster +0x1c, 2 at 30 fps), then sets the velocity to 25
// and the gravity to -0.6 for the arc. The airborne sub-state lands as soon as the monster is below ground + size x
// a species height (0x2bf5a0, ~150 above the ground), rising or not. At 30 fps the launch lifts it 200, above that
// line, and it arcs up to ~720 and lands on the way down; at 240 fps it lifts 25, so it "lands" the next update and
// snaps from the drill into standing. The patched store (`vstr s0, [r0, #0x14]` at 0x1308838, r0 the status block,
// r4 the monster) stores the speed that makes that one move as long as the 30 fps update's: 2v + 2a.
void emjump_launch(Cpu *c) {
    static int off = -1;
    if (off < 0) off = getenv("MHGU_NO_EMJUMP") && *getenv("MHGU_NO_EMJUMP") == '1';
    uint32_t b = c->r[0];
    float v = c->v.f[0], t = LDF(c->r[4] + 0x1cu), a = LDF(b + 0x24);
    if (!off && t > 0.0f && t < 2.0f) v = (2.0f * v + 2.0f * a - 0.5f * t * t * a) / t;
    STF(b + 0x14, v);
}
