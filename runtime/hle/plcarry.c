// Running with something carried at high frame rates (Bherna's Moofy). The carry run's update (action function
// 0x494200, motions 0xc91 / 0xc92; held R in a village) moves the hunter by its animation, and on top of that, every
// update the stick is held, pushes them 2.5 units along their facing (0x494308: `pos += (sin, 0, cos) * 2.5`) with no
// frame step, tuned for the Switch's 30 updates a second. At 240 fps the push is 8x as far a second: the hunter runs
// comically fast; walking (no push) is right. 0x49028c is the same code with motion 0x138f, scaled the same way.
// Here the 2.5 (`vmov.f32 s6, #2.5`) is scaled by the player's step (player +0x1c, 60 fps frames: 2.0 at 30 fps), so
// the push covers as much ground a second as in the 30 fps game; at 30 fps it is the original. tools/gen.py PATCHES.
// MHGU_NO_PLCARRY=1: the original (for comparing).
#include <stdlib.h>

#include "../hle.h"

static int g_off = -1;

float plcarry_push(uint32_t pl) {
    if (g_off < 0) {
        const char *e = getenv("MHGU_NO_PLCARRY");
        g_off = e && *e == '1';
    }
    float step = LDF(pl + 0x1c);
    if (g_off || !(step > 0.0f && step < 2.0f)) return 2.5f;
    return 2.5f * step / 2.0f;
}
