// Bowgun Pierce shots at high frame rates. A bowgun shell (uShellPlw04_000; its update is vtable slot 9, 0x66c880) that
// hits a monster stands still for the update that takes the hit, counts it (+0x15d8) and starts a 4-frame timer
// (+0x15d4, counted down by the step) after which it slows to 0.7 of its speed; the next hit can land 6 frames (0.1 s)
// later. All of that runs on the step except the stop: it lasts one update, which on the Switch's 30 updates a second
// is 2 frames of 60 fps, but 1 at 60 fps and 1/4 at 240 fps. So between two hits the shell flew 4 frames' worth on the
// Switch and 5 or 5.75 here, about a quarter further, and a Pierce shot's later hits left a small part sooner: on a
// Tigrex shot from the front, 3 hits in the head at 30 fps, the third in the neck at 60 fps (reported 2026-11-24;
// measured: 253 units from the first hit to the second at 30 fps, 311 at 60).
// Here the stop lasts 2 frames whatever the frame rate: after the update that took the hit, the shell keeps still for
// the rest of the 2 frames (its movement in each update cut by the part of its step that is still inside the stop), so
// it is where the 30 fps game has it at each hit. At 30 fps and below the original. MHGU_NO_SHELLSTEP=1: the original
// (for comparing). MHGU_LOG_SHELLSTEP=1: each update of a held shell logged; 2: every update of every bowgun shell.
#include <math.h>
#include <stdlib.h>

#include "../hle.h"

#define FN_W04_UPDATE 0x66c880u  // (shell): a bowgun shell's update (uShellPlw04_000, vtable 0x1960820 slot 9)
#define SH_STEP       0x1cu      // shell: its step (f32, 60 fps frames)
#define SH_POS        0x40u      // shell: its position
#define SH_HITS       0x15d8u    // shell: u16, the monster hits it has taken (the u16 after it is something else)
#define STOP_FRAMES   2.0f       // how long the 30 fps game holds a shell on a hit: one update, step 2.0
#define MAX_HELD      16

static struct { uint32_t shell; float left; } g_held[MAX_HELD];  // shells still inside a stop, and how much of it is left
static int g_log;

static int held_slot(uint32_t s) {
    for (int i = 0; i < MAX_HELD; i++)
        if (g_held[i].shell == s) return i;
    return -1;
}

static void update(Cpu *c) {
    uint32_t s = c->r[0];
    float step = LDF(s + SH_STEP), p0[3] = {LDF(s + SH_POS), LDF(s + SH_POS + 4), LDF(s + SH_POS + 8)};
    uint16_t hits = LD16(s + SH_HITS);
    fnhook_call_original(c, FN_W04_UPDATE);
    float p[3] = {LDF(s + SH_POS), LDF(s + SH_POS + 4), LDF(s + SH_POS + 8)};
    int k = held_slot(s);
    if (LD16(s + SH_HITS) != hits && step > 0.0f && step < STOP_FRAMES) {
        // (this update took a hit and stood still for it: the rest of the stop is still to come)
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
        if (g_log) LOG("[shellstep] %08x held %.3f of step %.3f, %.3f left\n", s, (double)hold, (double)step, (double)g_held[k].left);
    }
    if (g_log >= 2) {
        float dx = LDF(s + SH_POS) - p0[0], dy = LDF(s + SH_POS + 4) - p0[1], dz = LDF(s + SH_POS + 8) - p0[2];
        LOG("[shellstep] %08x at %.1f %.1f %.1f, moved %.2f, step %.3f, hits %u\n", s, (double)LDF(s + SH_POS),
            (double)LDF(s + SH_POS + 4), (double)LDF(s + SH_POS + 8), (double)sqrtf(dx * dx + dy * dy + dz * dz), (double)step,
            LD16(s + SH_HITS));
    }
}

void shellstep_init(void) {
    const char *e = getenv("MHGU_LOG_SHELLSTEP");
    g_log = e && *e ? atoi(e) : 0;
    e = getenv("MHGU_NO_SHELLSTEP");
    if (e && *e == '1') return;
    fnhook_set(FN_W04_UPDATE, hle_register("shellstep:update", update));
}
