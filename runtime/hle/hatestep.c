// Monsters' attention scores and noticing meters at high frame rates. Two per-update functions of a monster do
//     value += (int)(rate * step)
// with the step in 60 fps frames (monster +0x1c): 2.0 on the Switch's 30 updates a second. The rates are small, so above
// 30 fps the cast loses them: at 240 fps (step 0.25) any rate from -3 to 3 adds nothing, and at 60 fps a step just under
// 1.0 still takes 1 off a rate.
// - 0x2bbf98, the attention scores: a score per hunter, Palico and enemy (status block *(monster + 0x1428): +0x254, +0x264,
//   +0x274); the target pickers (0x2c5b14) take the highest. The rate comes from the species' table (distance to the
//   target and so on, 0x2bb828; a Jaggi near a hunter: 3 or 4). Without this fix, scores no longer rise with closeness
//   nor fall with distance at 240 fps, and those that have risen stay.
// - 0x2badac, the noticing meters: a meter per hunter (status +0x29c..+0x2a8) and Palico (+0x2ac..+0x2b8), 0..20000.
//   The rate is by the hunter's distance (+150 near ... -1 or -2 far, out of sight or in another area); a calm monster
//   whose meter passes 20000 notices that hunter (0x2bb5f0 in 0x27f0xx): it leaves what it was doing for its "noticed
//   something" routine (mode 3, a Blangonga looks around, action 1:5). At 240 fps the -2 a meter loses each update far
//   from the hunters became 0, so a meter filled during a fight never drained: a monster that fled to its nest to sleep
//   noticed the hunter two areas away as soon as it got there, looked around and walked off instead (seen 2026-11-20 in
//   Arctic Ridge area 3; at 60 and 120 fps it slept). While the meters' reset flag (status +0x2bc) is set the function
//   only empties them, every update, as in the original.
// Here a monster's time is cut into 30 fps updates (2.0 of its step): each function runs once for each with the step
// 2.0, so the values move as in the 30 fps game. At 30 fps and below the original. MHGU_NO_HATESTEP=1 /
// MHGU_NO_NOTICESTEP=1: the original per-update add of each (for comparing).
#include <math.h>
#include <stdlib.h>

#include "../hle.h"

#define FN_HATE    0x2bbf98u  // (monster): the update of its attention scores
#define FN_NOTICE  0x2badacu  // (monster): the update of its noticing meters
#define EN_STEP    0x1cu      // monster: its step (f32, 60 fps frames)
#define EN_INFO    0x1428u    // monster: its status block
#define ST_RESET   0x2bcu     // status: u8, the noticing meters' reset flag
#define SLOTS      32

typedef struct { uint32_t en; uint64_t seen; float t; } Slot;
static Slot g_hate[SLOTS], g_notice[SLOTS];

// Runs fn (monster in r0) only when the monster's time has reached another 30 fps update, with the step 2.0.
static void in_30fps_updates(Cpu *c, uint32_t fn, Slot *tab) {
    uint32_t en = c->r[0];
    float step = LDF(en + EN_STEP);
    if (!(step > 0.0f && step < 2.0f)) {
        fnhook_call_original(c, fn);
        return;
    }
    int s = 0, old = 0;
    while (s < SLOTS && tab[s].en != en) {
        if (tab[s].seen < tab[old].seen) old = s;
        s++;
    }
    if (s == SLOTS) {  // (new here: its first update starts a 30 fps update)
        s = old;
        tab[s].en = en, tab[s].t = 2.0f - step;
    }
    tab[s].seen = g_present_count;
    tab[s].t += step;
    if (tab[s].t < 2.0f) return;
    tab[s].t = fmodf(tab[s].t, 2.0f);
    STF(en + EN_STEP, 2.0f);
    fnhook_call_original(c, fn);
    STF(en + EN_STEP, step);
}

static void hate(Cpu *c) { in_30fps_updates(c, FN_HATE, g_hate); }

static void notice(Cpu *c) {
    uint32_t info = LD32(c->r[0] + EN_INFO);
    if (info && LD8(info + ST_RESET)) {  // (emptying the meters, no step)
        fnhook_call_original(c, FN_NOTICE);
        return;
    }
    in_30fps_updates(c, FN_NOTICE, g_notice);
}

static bool off(const char *name) {
    const char *e = getenv(name);
    return e && *e == '1';
}

void hatestep_init(void) {
    if (!off("MHGU_NO_HATESTEP")) fnhook_set(FN_HATE, hle_register("hatestep:hate", hate));
    if (!off("MHGU_NO_NOTICESTEP")) fnhook_set(FN_NOTICE, hle_register("hatestep:notice", notice));
}
