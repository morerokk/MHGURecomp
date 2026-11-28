// Attack windows at high frame rates: an attack's first and last hit tests where the 30 fps game makes them.
//
// An attack (the generic attack object; its update 0x36919c) has a delay (+0x5c) and then an open window (+0x60), both
// in frames of 60 fps, counted down each update by the step (times its owner's motion speed, +0x50c, when +0x31 bit 1
// is set). The update that takes the delay to 0 or below moves the rest into the window (open += delay, delay = 0), and
// the attack tests for hits in that same update; an attack made with no delay tests in the update that made it, after
// one countdown. It is switched off (+0x05 = 0) at the start of the first update that finds the window at or below 0,
// so the update that took it below 0 still tests, with the shapes wherever that update has moved them.
//
// So the 30 fps game tests an attack first at the first whole number of its 2-frame steps (times the speed) at or past
// the delay, never sooner than one step after the attack started, and last up to one step past the window's end. At
// higher rates both edges move by up to a step: the first test comes sooner, the last one earlier.
//
// The end (monster attacks): Hellblade Glavenus' black-tail tail slam (7:247) reaches the ground at the very end of its
// window, and the hunter standing at the base of the tail was hit on that last test at 30 fps (the tail 1.85 frames
// further along: 71-128 units into the hunter's capsule in the rig) and missed at 60 (0.65 frames: from 8 units into it
// to 229 short) (reported 2026-11-26 with a video, 60 against 30 fps, "loses its hit from the base of the tail to the
// black part"). A monster attack's window is stretched, when it starts, to end where the 30 fps game's last test is.
// The hunter's own attacks were left alone there: every one measured (Great Sword, Charge Blade: delays 4-60, windows
// 8-22) is a whole number of 2-frame steps at speed 1, so their last test is at the window's end at 30 and 60 fps alike.
//
// The start (hunter weapon attacks and monster attacks): Critical Juncture's counter (Long Sword Hunter Art; the stance
// is action 0010:060, the counter 0010:061) makes its slash with no delay (window 15) in the hunter's update after the
// monster's hit, and the slash tests once, in that update: 2 frames after the hit at 30 fps, 1 at 60. Against
// Hellblade Glavenus' lunging spin (7:251, ~54 units a frame) that is a different part of the monster in front of the
// blade: the reporter (2026-11-28, two GIFs, 30 against 60 fps) got the head and a flinch at 30 fps and the body or
// tail, often without a flinch, at 60. Rig (one Glavenus, the frame rate switched live between attempts): the slash's
// first test at open 13 at 30 fps and 14 at 60, the monster 54 units further along at 30 (x 509 against 455 with the
// same heading); counters on part 1 5 of 12 at 30 fps, 0 of 10 at 60. Here, when a window starts, its delay is raised
// to that first 30 fps test (the window shortened by as much, so its end stays), and the update that reaches it tests
// first, as the 30 fps game's does. Only the weapon's own attacks (+0x10 == 0): shells, kinsects, phials and the like
// keep their timing (shellstep.c handles bow and bowgun shots). A monster attack's whole-step delay at speed 1.05 (2.1
// frames a 30 fps update, 1.05 a 60 fps one) already falls on the same update at 60 fps; at other speeds and rates it
// can move by up to a step.
//
// At 30 fps and below nothing changes. MHGU_NO_ATKSTEP=1: the original; MHGU_LOG_ATKSTEP=1: each window start;
// 2: every attack counting down too. MHGU_ATKTRACE=1 (dummy.c) replaces this hook.
#include <math.h>
#include <stdlib.h>

#include "../hle.h"

#define FN_ATK_UPDATE 0x36919cu  // (attack): the generic attack update
#define AT_ALIVE      0x05u      // attack: u8, 0 once switched off
#define AT_OWNER      0x0cu      // attack: its owner
#define AT_VIA        0x10u      // attack: u8, 0: a hunter's weapon itself; else made by something it threw or fired
#define AT_FLAGS      0x31u      // attack: u8, bit 1 = its countdowns go by the owner's motion speed
#define AT_DELAY      0x5cu      // attack: f32, frames until its window opens
#define AT_OPEN       0x60u      // attack: f32, frames its window stays open
#define AT_KIND       0x6du      // attack: u8, 1 = a hunter's, 2 = a monster's
#define OWN_SPEED     0x50cu     // owner: f32, its motion speed
#define G_SYS         0x2326764u // the system object: +0x68 the step (60 fps frames)
#define KIND_HUNTER   1
#define KIND_MONSTER  2
#define SWITCH_STEP   2.0f
#define EPS           0.001f
#define SLOTS         256        // (one seen again after its entry gave way would be taken for a new window)

static struct { uint32_t at; float delay, open; } g_seen[SLOTS];  // attacks seen, their countdowns after the last update
static unsigned g_next;
static int g_log;

static void update(Cpu *c) {
    uint32_t at = c->r[0], sys = LD32(G_SYS);
    uint8_t kind = LD8(at + AT_KIND);
    if (kind != KIND_MONSTER && (kind != KIND_HUNTER || LD8(at + AT_VIA) != 0)) { fnhook_call_original(c, FN_ATK_UPDATE); return; }
    float step = sys ? LDF(sys + 0x68) : SWITCH_STEP, delay = LDF(at + AT_DELAY), open = LDF(at + AT_OPEN);
    int k = 0;
    while (k < SLOTS && g_seen[k].at != at) k++;
    // (a window starts: its delay or its open time went up since the last update, or an attack not seen before)
    bool start = k == SLOTS || delay > g_seen[k].delay + EPS || (!(delay > 0.0f) && open > g_seen[k].open + EPS);
    if (g_log >= 2 && (delay > 0.0f || open > 0.0f))  // (=2: every attack counting down, each update)
        LOG("[atkstep] %08x kind %u delay %.2f open %.2f alive %u start %d step %.2f\n", at, kind, (double)delay, (double)open,
            LD8(at + AT_ALIVE), start, (double)step);
    if (start && step > 0.0f && step < SWITCH_STEP && LD8(at + AT_ALIVE) && open > 0.0f) {
        uint32_t owner = LD32(at + AT_OWNER);
        float speed = (LD8(at + AT_FLAGS) & 2) && owner ? LDF(owner + OWN_SPEED) : 1.0f;
        float grid = SWITCH_STEP * speed, d0 = delay > 0.0f ? delay : 0.0f, end = d0 + open;
        if (grid > 0.01f) {
            // the first test: the first whole number of steps at or past the delay, at least one; half an update short
            // of it, so the update ending nearest it takes the delay below 0 (not one late for a float error)
            float first = grid * fmaxf(1.0f, ceilf(d0 / grid - EPS)) - 0.5f * step * speed;
            if (first > d0 + EPS && first < end) delay = first, open = end - first;
            // the last test (monster attacks): the first whole number of steps at or past the window's end
            if (kind == KIND_MONSTER) {
                float last = grid * ceilf(end / grid - EPS);
                if (last > end + EPS) open += last - end;
            }
            if (delay != LDF(at + AT_DELAY) || open != LDF(at + AT_OPEN)) {
                if (g_log)
                    LOG("[atkstep] attack %08x (kind %u, +0x38 %08x, +0x31 %02x, speed %.3f): delay %.2f open %.2f -> delay %.2f open %.2f\n",
                        at, kind, LD32(at + 0x38), LD8(at + AT_FLAGS), (double)speed, (double)LDF(at + AT_DELAY), (double)LDF(at + AT_OPEN),
                        (double)delay, (double)open);
                STF(at + AT_DELAY, delay), STF(at + AT_OPEN, open);
            }
        }
    }
    fnhook_call_original(c, FN_ATK_UPDATE);
    if (k == SLOTS) k = (int)(g_next++ % SLOTS);
    g_seen[k].at = at, g_seen[k].delay = LDF(at + AT_DELAY), g_seen[k].open = LDF(at + AT_OPEN);
}

void atkstep_init(void) {
    const char *e = getenv("MHGU_LOG_ATKSTEP");
    g_log = e && *e ? atoi(e) : 0;
    e = getenv("MHGU_NO_ATKSTEP");
    if (e && *e == '1') return;
    fnhook_set(FN_ATK_UPDATE, hle_register("atkstep:update", update));
}
