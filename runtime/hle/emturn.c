// Monster turns that divide by the frame step instead of multiplying by it (tools/gen.py PATCHES, "Monster turns").
// Duramboros (em055), action 7:32 (a charge that ends in a long turn on the spot), 0x10dc714: each update it turns
// (status +0x54, 65536 = a full turn) by
//     C / ((C2 / speed) * step / 2)
// where its neighbour block 0x10dc6a8 has C / (C2 / speed) * step / 2. Both are the same at 30 fps (step / 2 = 1);
// above that this one turns more per update the shorter the update: at 240 fps 8x per update, 64x in real time
// (measured: -124 a 30 fps update, -992 a 240 fps update; the turn the 30 fps game ends after ~20 degrees spun ~3
// full turns). The patch on its vdiv multiplies the quotient by (step / 2)^2, giving C / (C2 / speed) * step / 2 as
// in the neighbour. The game truncates the turn to an integer per update; above 30 fps the fraction is carried to the
// next update (per monster), so the turn adds up to the 30 fps one. step / 2 is at most 1 (below 30 fps as the
// original). MHGU_NO_EMTURN=1: the original (for comparing).
#include <math.h>
#include <stdlib.h>

#include "../hle.h"

static bool off(void) {
    static int o = -1;
    if (o < 0) o = getenv("MHGU_NO_EMTURN") && *getenv("MHGU_NO_EMTURN") == '1';
    return o;
}

// The fraction of a turn left over from a monster's last update (en: the monster).
static float carry_for(uint32_t en, float add, bool keep) {
    static struct { uint32_t en; float frac; } t[8];
    static unsigned next;
    int s = 0;
    while (s < 8 && t[s].en != en) s++;
    if (s == 8) {
        if (!keep) return add;
        s = (int)(next++ & 7), t[s].en = en, t[s].frac = 0;
    }
    float v = add + t[s].frac;
    t[s].frac = keep ? v - truncf(v) : 0;
    return v;
}

// The vdiv `q = a / b` of the turn, with half_step = step / 2 (the game's s2); en: the monster.
float emturn_div(uint32_t en, float a, float b, float half_step) {
    float q = a / b;
    if (off() || !(half_step > 0.0f && half_step < 1.0f)) return carry_for(en, q, false);
    return carry_for(en, q * half_step * half_step, true);
}
