// Mashing out of a stun (and the other holds mashed out of: the 13 callers of 0x47f590) at high frame rates. The
// stun (act 0x40:026, its update 0x4bcfd0) lasts up to 4 rounds of 750 (60 fps frames, counted down by the step);
// every update in which 0x47f590 says the player mashed adds 1 to the round's count (byte 6 of player +4), and 13
// (fewer in the later rounds) end the round early. 0x47f590 is 1 when the input state (player +0x13fc: buttons in the
// low half, the left stick's direction in bits 16-19) has, since the last update (+0x1400), a button (mask 0x5f0) or
// a stick direction that wasn't there (+0x13f8, +0x1406: state & ~last), on the local player (+0x13cc bit 0). The
// Switch's 30 updates a second see the controller every 33 ms: at most 30 a second, a quick press or a stick turn
// between two looks isn't seen at all, two in one look count once. At high frame rates every press and every turn
// of the stick counted (measured, fast mashing from the control file: the 4 rounds took 0.83 s at 120 fps, 1.5 s at
// 30).
// Here the game's time is cut into 30 fps updates (2.0 of the player's step +0x1c, 60 fps frames): at the update
// that starts one, the input state is compared with its copy from the start of the last one, as the 30 fps game does,
// and in the updates between nothing counts. (Counting any change within the 30 fps update instead still counted
// more than the 30 fps game.) At 30 fps and below the original. MHGU_NO_MASHRATE=1: the original per-update count
// (for comparing).
#include <math.h>
#include <stdlib.h>

#include "../hle.h"

#define FN_MASH   0x47f590u    // (player) -> 1: mashed this update
#define PL_STEP   0x1cu        // player: its step (f32, 60 fps frames)
#define PL_LOCAL  0x13ccu      // player: bit 0 the local player's
#define PL_STATE  0x13fcu      // player: the input state (+0x1400 the last update's)
#define MASH_BITS 0x00ff05f0u  // (the stick's directions, the buttons 0x47f590 counts)
#define SLOTS     4

static struct { uint32_t pl, state; uint64_t update; float t; bool got; } g_slot[SLOTS];

static void mash(Cpu *c) {
    uint32_t pl = c->r[0];
    float step = LDF(pl + PL_STEP);
    if (!(step > 0.0f && step < 2.0f)) {
        fnhook_call_original(c, FN_MASH);
        return;
    }
    int s = 0;
    while (s < SLOTS - 1 && g_slot[s].pl && g_slot[s].pl != pl) s++;
    if (g_slot[s].pl != pl || g_present_count - g_slot[s].update > 1) {  // (a hold starting: from the last update)
        g_slot[s].pl = pl, g_slot[s].state = LD32(pl + PL_STATE + 4), g_slot[s].t = 2.0f - step;
        g_slot[s].update = g_present_count - 1;
    }
    if (g_slot[s].update != g_present_count) {  // (once per update, however many callers ask)
        g_slot[s].update = g_present_count;
        g_slot[s].t += step;
        g_slot[s].got = false;
        if (g_slot[s].t >= 2.0f) {  // a 30 fps update starts: a look at the controller
            uint32_t state = LD32(pl + PL_STATE);
            g_slot[s].got = (LD8(pl + PL_LOCAL) & 1) && (state & ~g_slot[s].state & MASH_BITS);
            g_slot[s].state = state;
            g_slot[s].t = fmodf(g_slot[s].t, 2.0f);
        }
    }
    c->r[0] = g_slot[s].got;
}

void mashrate_init(void) {
    const char *e = getenv("MHGU_NO_MASHRATE");
    if (e && *e == '1') return;
    fnhook_set(FN_MASH, hle_register("mashrate:mash", mash));
}
