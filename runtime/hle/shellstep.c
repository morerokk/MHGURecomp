// Pierce shots at high frame rates: hits spaced along a shot as on the Switch (30 updates a second).
//
// Bowguns. A bowgun shell (uShellPlw04_000; its update is vtable slot 9, 0x66c880) that hits a monster stands still for
// the update that takes the hit, counts it (+0x15d8) and starts a 4-frame timer (+0x15d4, counted down by the step)
// after which it slows to 0.7 of its speed; the next hit can land 6 frames (0.1 s) later. All of that runs on the step
// except the stop: it lasts one update, which on the Switch is 2 frames of 60 fps, but 1 at 60 fps and 1/4 at 240 fps.
// So between two hits the shell flew 4 frames' worth on the Switch and 5 or 5.75 here, about a quarter further, and a
// Pierce shot's later hits left a small part sooner: on a Tigrex shot from the front, 3 hits in the head at 30 fps,
// the third in the neck at 60 fps (reported 2026-11-24; measured: 253 units from the first hit to the second at 30
// fps, 311 at 60). Here the stop lasts 2 frames whatever the frame rate: after the update that took the hit, the shell
// keeps still for the rest of the 2 frames (its movement in each update cut by the part of its step still inside it).
// Its age (+0x1378, frames flown, which the update that takes a hit doesn't add to) stands still with it: Pierce's
// critical distance goes by it, so with the age running on, a third hit landed 1-2 frames "older" than on the Switch at
// 60 fps and fell out of the window (reported after the first fix: 47, 47, 31 = the head without the x1.5).
//
// The Bow. Its arrows (uShellPlw10_000, update 0x67957c) don't stop on a hit. A hit sets a timer (+0x15ec, from the
// shot's data: 4 frames for the Pierce arrows measured, stored as 4 minus the step by the update that takes the hit)
// counted down by the step; the update in which it runs out moves the arrow slower (its velocity +0x1010, per frame,
// times a factor from the data: 0.7 measured) and sets a 1-frame wait (+0x1404, counted down by the step) after which
// the next hit can land. On the Switch the update that empties the timer covers frames 2 to 4 after the hit, so the
// arrow flew slow for half of each gap (2 + 2 x 0.7 = 3.4 frames' worth), and the next update's step of 2 ends the wait
// at once: the hit lands at frame 4. At 60 fps only the last frame was slow (3.7), the hit at frame 4; at 240 fps the
// last quarter, and the wait held the hit back 3 more updates, to frame 4.75 (4.45 frames' worth). Here an arrow is
// slowed from where the 30 fps game slows it (frame 2 for a 4-frame timer) by the factor last seen; when the game's
// timer runs out (the factor is read then, from the velocity) what was let through beyond it is taken back and the
// wait loses what the 30 fps game's next update would take off it, so the next hit lands at frame 4; the part of that
// update past frame 4 (at 144 fps it ends at 4.17) moves nothing, so the arrow is where the 30 fps game has it then.
//
// Both: the critical distance. A shell's update works out its attack's motion value from its age (+0x1378): x1.5 from
// one age to another (Pierce S Lv1 shells measured: 14 to 26 frames; Pierce arrows: 16 to 32). On the Switch its updates
// end at multiples of 2 frames of age, and a hit is found where one ends: its age is the next multiple of 2 after the
// shot reached the part. Here a hit lands at the first update after that, up to 1.5 frames younger at 120 fps, 13 or
// 13.5 where the 30 fps game has 14, outside the window (reported after the fixes above: still 47, 47, 31 at 120 fps).
// So each update runs with the 30 fps game's age at the end of its update this one ends in (`grid_before`).
//
// At 30 fps and below the original. MHGU_NO_SHELLSTEP=1: the original for both (for comparing). MHGU_LOG_SHELLSTEP=1:
// what each does; 2: every update of every bowgun shell and arrow too.
#include <math.h>
#include <stdlib.h>

#include "../hle.h"

#define FN_W04_UPDATE 0x66c880u  // (shell): a bowgun shell's update (uShellPlw04_000, vtable 0x1960820 slot 9)
#define FN_W10_UPDATE 0x67957cu  // (shell): a Bow arrow's update (uShellPlw10_000, vtable 0x1961870 slot 9)
#define SH_STEP       0x1cu      // shell: its step (f32, 60 fps frames)
#define SH_POS        0x40u      // shell: its position
#define SH_HITS       0x15d8u    // bowgun shell: u16, the monster hits it has taken (the u16 after it is something else)
#define SH_AGE        0x1378u    // shell: f32, frames it has flown (not counting the update that took a hit)
#define SH_AGE_WAS    0x137cu    // shell: f32, that at the start of the last update
#define SH_MV         0x13e0u    // shell: s16, its attack's motion value (worked out by its update; x1.5 at critical distance)
#define AR_VEL        0x1010u    // arrow: its velocity, per frame of 60 fps
#define AR_SLOW       0x15ecu    // arrow: f32, the timer a hit starts; the update it runs out in slows the arrow
#define AR_REARM      0x1404u    // shell: f32, set to 1 frame when that timer runs out, counted down by the step; the
                                 // next hit can land once it is 0
#define STOP_FRAMES   2.0f       // how long the 30 fps game holds a shell on a hit: one update, step 2.0
#define SWITCH_STEP   2.0f
#define HIT_FLIGHT    4.0f       // frames a shell flies from a hit to its next (6 frames, the first 2 standing still)
#define MAX_HELD      16

static struct { uint32_t shell; float left; } g_held[MAX_HELD];  // shells still inside a stop, and how much of it is left
// Bowgun shells whose next hit is still to come, and their age at the last
static struct { uint32_t shell; float age; } g_last[MAX_HELD];
// Arrows since a hit: frames since it (at the end of the last update), the frame the 30 fps game slows the arrow from
// and the one its next hit lands at, of the frames after the slow-down so far all of them and those weighted by the
// factor they were slowed by here, and whether the game's timer has run out.
static struct { uint32_t shell; float since, slow_from, hit_at, after, after_slowed; bool slowed; } g_arrow[MAX_HELD];
static float g_factor = 1.0f;  // the slow-down factor last seen (1 before any: nothing slowed early, all taken back)
static int g_log;

static int held_slot(uint32_t s) {
    for (int i = 0; i < MAX_HELD; i++)
        if (g_held[i].shell == s) return i;
    return -1;
}

static int last_slot(uint32_t s) {
    for (int i = 0; i < MAX_HELD; i++)
        if (g_last[i].shell == s) return i;
    return -1;
}

// The age the update runs with: the 30 fps game's at the end of its update this one ends in. Its updates end at whole
// 2-frame steps of age, so that is where it sees a hit's age, and the motion value goes by the age (Pierce's critical
// distance: x1.5 from a set age to another, both multiples of 2 frames for the shots measured). Here a hit can land up
// to a step after the shell reaches the part, up to 1.5 frames younger than the 30 fps game's at 120 fps, outside the
// window if that is where it starts (reported: the same three hits, 47, 47, 31 at 120 fps). `due`, if > 0, is the age
// at which the next hit is due (4 frames of flight after the last): an update past it ends there, as a 30 fps update
// does, and the age keeps that (at 144 fps a wait for the next hit rounds up to whole updates, 6.25 frames, and the
// age would run 1/4 frame ahead per hit). Returns what to take off again after the update (`grid_after`).
typedef struct { float age0, end, end30, off; } Grid;
static Grid grid_before(uint32_t s, float step, float due) {
    Grid g = {LDF(s + SH_AGE), 0.0f, 0.0f, 0.0f};
    g.end = g.end30 = g.age0 + step;
    if (!(step > 0.0f && step < SWITCH_STEP)) return g;
    if (due > 0.0f && g.age0 < due - 0.001f && g.end > due) g.end30 = due;
    // (steps vary a little with the frame pacing: an age a hair past a 2-frame point is at it; and a hair past it after
    // the update, which compares it with ages that are multiples of 2, rather than a rounding error short of it)
    g.off = SWITCH_STEP * ceilf((g.end30 - 0.05f) / SWITCH_STEP) - g.end + 0.001f;
    STF(s + SH_AGE, g.age0 + g.off);
    return g;
}
static void grid_after(uint32_t s, const Grid *g) {
    if (g->off == 0.0f && g->end30 == g->end) return;
    float age = LDF(s + SH_AGE) - g->off;
    if (age > g->age0 + 0.001f) age -= g->end - g->end30;  // (it flew: up to the hit that is due)
    STF(s + SH_AGE, age), STF(s + SH_AGE_WAS, LDF(s + SH_AGE_WAS) - g->off);
}

static void update(Cpu *c) {
    uint32_t s = c->r[0];
    float step = LDF(s + SH_STEP), p0[3] = {LDF(s + SH_POS), LDF(s + SH_POS + 4), LDF(s + SH_POS + 8)};
    uint16_t hits = LD16(s + SH_HITS);
    int l = last_slot(s);
    // (forgotten once the next hit is no longer due, or for a new shell where an old one was)
    if (l >= 0 && !(LDF(s + SH_AGE) >= g_last[l].age && LDF(s + SH_AGE) < g_last[l].age + HIT_FLIGHT)) g_last[l].shell = 0, l = -1;
    Grid g = grid_before(s, step, l >= 0 ? g_last[l].age + HIT_FLIGHT : 0.0f);
    fnhook_call_original(c, FN_W04_UPDATE);
    grid_after(s, &g);
    float p[3] = {LDF(s + SH_POS), LDF(s + SH_POS + 4), LDF(s + SH_POS + 8)};
    int k = held_slot(s);
    if (LD16(s + SH_HITS) != hits && step > 0.0f && step < STOP_FRAMES) {
        // (this update took a hit and stood still for it: the rest of the stop is still to come)
        if (l < 0) l = last_slot(0);
        if (l < 0) l = 0;
        g_last[l].shell = s, g_last[l].age = LDF(s + SH_AGE);
        if (k < 0) k = held_slot(0);
        if (k < 0) k = 0;  // (more than MAX_HELD shells held at once: the oldest entry gives way)
        g_held[k].shell = s, g_held[k].left = STOP_FRAMES - step;
        if (g_log) LOG("[shellstep] %08x hit %u: held %.3f more frames\n", s, LD16(s + SH_HITS), (double)g_held[k].left);
        return;
    }
    if (k >= 0) {
        float hold = step > 0.0f ? fminf(g_held[k].left, step) : 0.0f, keep = step > 0.0f ? 1.0f - hold / step : 1.0f;
        g_held[k].left -= hold;
        if (!(g_held[k].left > 0.0f)) g_held[k].shell = 0;
        for (int i = 0; i < 3; i++) STF(s + SH_POS + 4u * (uint32_t)i, p0[i] + (p[i] - p0[i]) * keep);
        // (its age stands still too, as in the update that took the hit: it starts the update from +0x137c)
        STF(s + SH_AGE, LDF(s + SH_AGE_WAS) + (LDF(s + SH_AGE) - LDF(s + SH_AGE_WAS)) * keep);
        if (g_log) LOG("[shellstep] %08x held %.3f of step %.3f, %.3f left\n", s, (double)hold, (double)step, (double)g_held[k].left);
    }
    if (g_log >= 2) {
        float dx = LDF(s + SH_POS) - p0[0], dy = LDF(s + SH_POS + 4) - p0[1], dz = LDF(s + SH_POS + 8) - p0[2];
        LOG("[shellstep] %08x at %.1f %.1f %.1f, moved %.2f, step %.3f, hits %u, age %.3f, was %.3f, mv %d\n", s, (double)LDF(s + SH_POS),
            (double)LDF(s + SH_POS + 4), (double)LDF(s + SH_POS + 8), (double)sqrtf(dx * dx + dy * dy + dz * dz), (double)step,
            LD16(s + SH_HITS), (double)LDF(s + SH_AGE), (double)LDF(s + SH_AGE_WAS), (int)(int16_t)LD16(s + SH_MV));
    }
}

static int arrow_slot(uint32_t s) {
    for (int i = 0; i < MAX_HELD; i++)
        if (g_arrow[i].shell == s) return i;
    return -1;
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static void arrow_update(Cpu *c) {
    uint32_t s = c->r[0];
    float step = LDF(s + SH_STEP), timer0 = LDF(s + AR_SLOW);
    float p0[3] = {LDF(s + SH_POS), LDF(s + SH_POS + 4), LDF(s + SH_POS + 8)};
    float v0[3] = {LDF(s + AR_VEL), LDF(s + AR_VEL + 4), LDF(s + AR_VEL + 8)};
    int k = arrow_slot(s);
    Grid g = grid_before(s, step, k >= 0 ? LDF(s + SH_AGE) + g_arrow[k].hit_at - g_arrow[k].since : 0.0f);
    fnhook_call_original(c, FN_W10_UPDATE);
    grid_after(s, &g);
    float timer = LDF(s + AR_SLOW);
    k = arrow_slot(s);
    if (step > 0.0f && step < SWITCH_STEP && timer > timer0 + 0.001f) {
        // (a hit started the timer: its length is what is left plus this update's step; the 30 fps game's update that
        // empties it starts at the last whole update before its end, and its next hit lands when that update ends)
        float len = timer + step, from = SWITCH_STEP * ceilf((len - SWITCH_STEP) / SWITCH_STEP - 0.001f);
        if (k < 0) k = arrow_slot(0);
        if (k < 0) k = 0;  // (more than MAX_HELD arrows at once: the oldest entry gives way)
        g_arrow[k].shell = s, g_arrow[k].since = step, g_arrow[k].slow_from = from > 0.0f ? from : 0.0f;
        g_arrow[k].hit_at = g_arrow[k].slow_from + SWITCH_STEP;
        g_arrow[k].after = g_arrow[k].after_slowed = 0.0f, g_arrow[k].slowed = false;
        if (g_log) LOG("[shellstep] arrow %08x hit: timer %.3f frames, slowed from frame %.3f, next hit at %.3f\n", s, (double)len,
                       (double)g_arrow[k].slow_from, (double)g_arrow[k].hit_at);
    } else if (k >= 0) {
        float t0 = g_arrow[k].since, t1 = t0 + step;
        g_arrow[k].since = t1;
        // (the parts of this update after the 30 fps game's slow-down, and after its next hit, until that hit is due)
        float slow = clampf(fminf(t1, g_arrow[k].hit_at) - fmaxf(t0, g_arrow[k].slow_from), 0.0f, step);
        float wait = clampf(t1 - fmaxf(t0, g_arrow[k].hit_at), 0.0f, step);
        float keep = 1.0f, over = 0.0f;
        if (!g_arrow[k].slowed && timer0 > 0.0f && !(timer > 0.0f)) {
            // (the game slowed it in this update: the factor, and what this run let through after the 30 fps game's
            // slow-down beyond that factor, taken back along the velocity it had)
            float n0 = sqrtf(v0[0] * v0[0] + v0[1] * v0[1] + v0[2] * v0[2]);
            float n1 = sqrtf(LDF(s + AR_VEL) * LDF(s + AR_VEL) + LDF(s + AR_VEL + 4) * LDF(s + AR_VEL + 4) +
                             LDF(s + AR_VEL + 8) * LDF(s + AR_VEL + 8));
            float f = n0 > 0.0f ? n1 / n0 : 1.0f;
            over = g_arrow[k].after_slowed - f * g_arrow[k].after;
            if (f > 0.0f && f < 1.0f) g_factor = f;
            // (and the 1-frame wait it starts before the next hit can land: the 30 fps game's next update takes 2 off it)
            float wait0 = LDF(s + AR_REARM);
            if (wait0 > 0.0f) STF(s + AR_REARM, fmaxf(0.0f, wait0 - (SWITCH_STEP - step)));
            g_arrow[k].slowed = true;
            if (g_log) LOG("[shellstep] arrow %08x slowed at frame %.3f: factor %.3f, took back %.3f frames\n", s, (double)t0, (double)f, (double)over);
        } else if (!g_arrow[k].slowed && !(timer > 0.0f)) {
            g_arrow[k].shell = 0;  // (the timer ended some other way)
        } else if (!g_arrow[k].slowed && slow > 0.0f) {
            keep -= slow * (1.0f - g_factor) / step;
            g_arrow[k].after += slow, g_arrow[k].after_slowed += slow * g_factor;
        }
        if (g_arrow[k].shell) {
            keep -= wait / step;  // (past the 30 fps game's next hit: kept still for it)
            if (keep < 0.0f) keep = 0.0f;
            for (int i = 0; i < 3; i++) {
                float a = LDF(s + SH_POS + 4u * (uint32_t)i);
                STF(s + SH_POS + 4u * (uint32_t)i, p0[i] + (a - p0[i]) * keep - v0[i] * over);
            }
            if (g_log && (keep < 1.0f || over != 0.0f))
                LOG("[shellstep] arrow %08x frame %.3f: moved %.3f of its step %.3f\n", s, (double)t0, (double)keep, (double)step);
            if (t1 >= g_arrow[k].hit_at - 0.001f) g_arrow[k].shell = 0;  // (its next hit is due: nothing more to do)
        }
    }
    if (g_log >= 2) {
        float dx = LDF(s + SH_POS) - p0[0], dy = LDF(s + SH_POS + 4) - p0[1], dz = LDF(s + SH_POS + 8) - p0[2];
        LOG("[shellstep] arrow %08x at %.1f %.1f %.1f, moved %.2f, step %.3f, timer %.3f, age %.3f, mv %d\n", s, (double)LDF(s + SH_POS),
            (double)LDF(s + SH_POS + 4), (double)LDF(s + SH_POS + 8), (double)sqrtf(dx * dx + dy * dy + dz * dz), (double)step,
            (double)timer, (double)LDF(s + SH_AGE), (int)(int16_t)LD16(s + SH_MV));
    }
}

void shellstep_init(void) {
    const char *e = getenv("MHGU_LOG_SHELLSTEP");
    g_log = e && *e ? atoi(e) : 0;
    e = getenv("MHGU_NO_SHELLSTEP");
    if (e && *e == '1') return;
    fnhook_set(FN_W04_UPDATE, hle_register("shellstep:update", update));
    fnhook_set(FN_W10_UPDATE, hle_register("shellstep:arrow", arrow_update));
}
