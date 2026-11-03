// Monsters' attention scores at high frame rates. Each monster keeps a score per hunter, Palico and enemy (status block
// *(monster + 0x1428): +0x254, +0x264, +0x274); the target pickers (0x2c5b14) take the highest. Every update
// 0x2bbf98 adds a rate from the species' table (distance to the target and so on, 0x2bb828) as
//     score += (int)(rate * step)
// with the step in 60 fps frames (monster +0x1c): 2.0 on the Switch's 30 updates a second. The rates are small (a Jaggi
// near a hunter: 3 or 4), so above 30 fps the cast loses them: at 240 fps (step 0.25) any rate from -3 to 3 adds nothing,
// scores no longer rise with closeness nor fall with distance, and those that have risen stay. At 60 fps a step just
// under 1.0 still takes 1 off a rate.
// Here the monster's time is cut into 30 fps updates (2.0 of its step): 0x2bbf98 runs once for each with the step 2.0,
// so the scores move as in the 30 fps game. At 30 fps and below the original. MHGU_NO_HATESTEP=1: the original per-update
// add (for comparing).
#include <math.h>
#include <stdlib.h>

#include "../hle.h"

#define FN_HATE  0x2bbf98u  // (monster): the update of its attention scores
#define EN_STEP  0x1cu      // monster: its step (f32, 60 fps frames)
#define SLOTS    32

static struct { uint32_t en; uint64_t seen; float t; } g_slot[SLOTS];

static void hate(Cpu *c) {
    uint32_t en = c->r[0];
    float step = LDF(en + EN_STEP);
    if (!(step > 0.0f && step < 2.0f)) {
        fnhook_call_original(c, FN_HATE);
        return;
    }
    int s = 0, old = 0;
    while (s < SLOTS && g_slot[s].en != en) {
        if (g_slot[s].seen < g_slot[old].seen) old = s;
        s++;
    }
    if (s == SLOTS) {  // (new here: its first update starts a 30 fps update)
        s = old;
        g_slot[s].en = en, g_slot[s].t = 2.0f - step;
    }
    g_slot[s].seen = g_present_count;
    g_slot[s].t += step;
    if (g_slot[s].t < 2.0f) return;
    g_slot[s].t = fmodf(g_slot[s].t, 2.0f);
    STF(en + EN_STEP, 2.0f);
    fnhook_call_original(c, FN_HATE);
    STF(en + EN_STEP, step);
}

void hatestep_init(void) {
    const char *e = getenv("MHGU_NO_HATESTEP");
    if (e && *e == '1') return;
    fnhook_set(FN_HATE, hle_register("hatestep:hate", hate));
}
