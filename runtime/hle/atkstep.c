// Monster attack windows at high frame rates: an attack's last hit test where the 30 fps game makes it.
//
// An attack (the generic attack object; its update 0x36919c) has a delay (+0x5c) and then an open window (+0x60), both
// in frames of 60 fps, counted down each update by the step (times its owner's motion speed, +0x50c, when +0x31 bit 1
// is set). It is switched off (+0x05 = 0) at the start of the first update that finds the window at or below 0, so the
// update that took it below 0 still tests for hits, with the monster's shapes wherever that update has moved them:
// on the Switch up to 2 frames (times the speed) past the window's end, at 60 fps up to 1, at 240 fps up to 1/4. Most
// attacks don't mind; Hellblade Glavenus' black-tail tail slam (7:247) does: its tail reaches the ground at the very
// end of its window, and the hunter standing at the base of the tail was hit on that last test at 30 fps (the tail
// 1.85 frames further along: 71-128 units into the hunter's capsule in the rig) and missed at 60 (0.65 frames: from 8
// units into it to 229 short) (reported 2026-11-26 with a video, 60 against 30 fps, "loses its hit from the base of the
// tail to the black part").
//
// Here a monster attack's window is stretched, when it starts, to end where the 30 fps game's last test is: at the
// first multiple of its step (2 frames times the speed) from the attack's start at or past the window's end. The
// update that reaches that point then tests last, with the shapes where the 30 fps game has them (exactly at 60 and
// 120 fps; at 144 fps up to 0.4 frames past it). Earlier tests are as before (more of them than at 30 fps).
//
// Monster attacks only (+0x6d == 2). The hunter's own attacks run through the same update (and by the motion speed,
// +0x31 = 7), but every one measured (Great Sword, Charge Blade: delays 4-60, windows 8-22) is a whole number of
// 2-frame steps at speed 1, so their last test is at the window's end at 30 and 60 fps alike: left alone. (Hitstop
// slows the countdown for ~10 frames, speed 0.1, and can leave a window up to ~0.4 frames off that grid at 30 fps,
// after the swing has already hit: no difference worth copying.)
// At 30 fps and below nothing changes. MHGU_NO_ATKSTEP=1: the original; MHGU_LOG_ATKSTEP=1: each window start;
// 2: every monster attack counting down too. MHGU_ATKTRACE=1 (dummy.c) replaces this hook.
#include <math.h>
#include <stdlib.h>

#include "../hle.h"

#define FN_ATK_UPDATE 0x36919cu  // (attack): the generic attack update
#define AT_ALIVE      0x05u      // attack: u8, 0 once switched off
#define AT_OWNER      0x0cu      // attack: its owner
#define AT_FLAGS      0x31u      // attack: u8, bit 1 = its countdowns go by the owner's motion speed
#define AT_DELAY      0x5cu      // attack: f32, frames until its window opens
#define AT_OPEN       0x60u      // attack: f32, frames its window stays open
#define AT_KIND       0x6du      // attack: u8, 2 = a monster's
#define OWN_SPEED     0x50cu     // owner: f32, its motion speed
#define G_SYS         0x2326764u // the system object: +0x68 the step (60 fps frames)
#define KIND_MONSTER  2
#define SWITCH_STEP   2.0f
#define SLOTS         256        // (monster attacks only; one seen again after its entry gave way would be stretched
                                 // from where it is, not from its start)

static struct { uint32_t at; float delay, open; } g_seen[SLOTS];  // attacks seen, their countdowns after the last update
static unsigned g_next;
static int g_log;

static void update(Cpu *c) {
    uint32_t at = c->r[0], sys = LD32(G_SYS);
    uint8_t kind = LD8(at + AT_KIND);
    if (kind != KIND_MONSTER) { fnhook_call_original(c, FN_ATK_UPDATE); return; }
    float step = sys ? LDF(sys + 0x68) : SWITCH_STEP, delay = LDF(at + AT_DELAY), open = LDF(at + AT_OPEN);
    int k = 0;
    while (k < SLOTS && g_seen[k].at != at) k++;
    // (a window starts: its delay or its open time went up since the last update, or an attack not seen before)
    bool start = k == SLOTS || delay > g_seen[k].delay + 0.001f || (!(delay > 0.0f) && open > g_seen[k].open + 0.001f);
    if (g_log >= 2 && (delay > 0.0f || open > 0.0f))  // (=2: every monster attack counting down, each update)
        LOG("[atkstep] %08x delay %.2f open %.2f alive %u start %d step %.2f\n", at, (double)delay, (double)open, LD8(at + AT_ALIVE),
            start, (double)step);
    if (start && step > 0.0f && step < SWITCH_STEP && LD8(at + AT_ALIVE) && open > 0.0f) {
        uint32_t owner = LD32(at + AT_OWNER);
        float speed = (LD8(at + AT_FLAGS) & 2) && owner ? LDF(owner + OWN_SPEED) : 1.0f;
        float grid = SWITCH_STEP * speed, end = (delay > 0.0f ? delay : 0.0f) + open;
        if (grid > 0.01f) {
            float last = grid * ceilf(end / grid - 0.001f), more = last - end;
            if (more > 0.001f) STF(at + AT_OPEN, open + more);
            if (g_log)
                LOG("[atkstep] attack %08x (kind %u, +0x38 %08x, +0x31 %02x): delay %.2f, open %.2f -> %.2f (speed %.3f)\n", at, kind,
                    LD32(at + 0x38), LD8(at + AT_FLAGS), (double)delay, (double)open, (double)(more > 0.001f ? open + more : open), (double)speed);
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
