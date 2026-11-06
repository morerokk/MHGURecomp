// A monster's stall timers at high frame rates. While it hunts a target it can see (0x2c3d08), monster +0x1414 and
// +0x1418 count how long it has gone on without settling that, and past 300 and 1800 they set bits 0x10 and 0x20 of
// monster +0x1408, which make it reposition. They rise by a fixed amount per update (+0x1418 by 60 in some places of the
// zone's map), tuned for the Switch's 30 updates a second: at 240 fps they fill 8x as fast, so a monster gives up on
// closing the gap after a fraction of the time and runs off to the side of the hunter instead.
// Here the amount is scaled by the monster's step (monster +0x1c: 2.0 at 30 fps), so they fill as in the 30 fps game. At
// 30 fps the same as the original. tools/gen.py PATCHES, the two adds. MHGU_NO_EMSTALL=1: the original (for comparing).
#include <stdlib.h>

#include "../hle.h"

static int g_off = -1;

float emstall_scale(uint32_t en) {
    if (g_off < 0) {
        const char *e = getenv("MHGU_NO_EMSTALL");
        g_off = e && *e == '1';
    }
    return g_off ? 1.0f : LDF(en + 0x1c) / 2.0f;
}
