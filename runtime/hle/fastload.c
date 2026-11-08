// Shorter transitions when playing alone. Three waits in the game have nothing to do with loading (timed from an area
// trace, 2026-11-08, 60 fps; the loading itself is 0.15 s for a zone, 0.7 s for a village, 1 s for a quest):
// - Departing on a quest: the town area's mode 4 (0x8bd720), state 9, shows the quest's card and counts area +0xe4 (an
//   int, 150) down by the engine step before it starts the quest's load. The int cuts the fraction off, so every
//   update takes at least 1 off: 2 - 2.5 s up to 60 fps, 150 updates above that (1 s at 150 fps).
// - Leaving a town (for a quest, another village, the Hub) and changing zone in a hunt, two things the area then polls
//   0x6dee48(sound, 1) for, on a black screen:
//   the music streams, told to stop with a fade (0x6e3ecc's last argument, ms) of 1500 ms by the town (0x6de8f4) and
//   1000 ms by the zone change (0x6de048): each of those has one caller, the transition;
//   and a counter (sound +0x1c, in the engine's 60 fps steps) to 60 or 90 by the kind of area, the voices' volume.
// Alone, state 9's count is dropped, the streams fade in STREAM_FADE_MS (about the game's screen fade, 20 steps) and
// the counter runs MUSIC_SPEEDUP times as fast. 0x6dee48 still waits until the streams and voices have stopped: nothing
// goes on earlier than the music allows, there is only less of it to wait for.
// In a network session (ldn_net_up: hosting or joined, local or over the internet) nothing changes, so that players
// stay as in step as they always were. MHGU_NO_FASTLOAD=1: the game's own timing (for comparing).
#include <stdlib.h>

#include "../hle.h"
#include "../net/net.h"

#define FN_TOWN_LEAVE   0x8bd720u   // town area, mode 4 (r0: the area)
#define AREA_STATE      0x5u        // u8: the mode's state
#define AREA_WAIT       0xe4u       // int: state 9's count
#define QUEST_WAIT      9u

#define FN_STREAM_STOP  0x6e3eccu   // (sound, stream slot, kind, fade ms)
#define STOP_TOWN       0x6dea34u   // (return addresses: in 0x6de8f4, leaving a town)
#define STOP_ZONE       0x6de210u   // (in 0x6de048, a zone change)
#define STREAM_FADE_MS  300u

#define FN_MUSIC_FADE   0x6dee48u   // (sound, 1) -> 1 when the music has faded out and stopped
#define RET_TOWN        0x8bd90cu   // (the town's mode 4, state 4)
#define RET_ZONE        0x89fcc0u   // (the quest area's zone change, 0x89fb68 state 1)
#define SND_COUNT       0x1cu       // f32: the fade's counter
#define G_SYSTEM        0x2326764u  // engine system object *: +0x68 the frame step in 60 fps frames
#define SYS_STEP        0x68u
#define MUSIC_SPEEDUP   4.5f        // (90 steps -> 20)

static void town_leave(Cpu *c) {
    uint32_t area = c->r[0];
    if (LD8(area + AREA_STATE) == QUEST_WAIT && !ldn_net_up()) ST32(area + AREA_WAIT, 0);
    fnhook_call_original(c, FN_TOWN_LEAVE);
}

static void stream_stop(Cpu *c) {
    uint32_t from = c->r[14];
    if ((from == STOP_TOWN || from == STOP_ZONE) && !ldn_net_up()) c->r[3] = STREAM_FADE_MS;
    fnhook_call_original(c, FN_STREAM_STOP);
}

static void music_fade(Cpu *c) {
    uint32_t from = c->r[14];
    if ((from == RET_TOWN || from == RET_ZONE) && !ldn_net_up()) {
        uint32_t sound = c->r[0];
        // (the game adds one step itself)
        STF(sound + SND_COUNT, LDF(sound + SND_COUNT) + (MUSIC_SPEEDUP - 1.0f) * LDF(LD32(G_SYSTEM) + SYS_STEP));
    }
    fnhook_call_original(c, FN_MUSIC_FADE);
}

void fastload_init(void) {
    const char *e = getenv("MHGU_NO_FASTLOAD");
    if (e && *e == '1') return;
    fnhook_set(FN_TOWN_LEAVE, hle_register("fastload:town_leave", town_leave));
    fnhook_set(FN_STREAM_STOP, hle_register("fastload:stream_stop", stream_stop));
    fnhook_set(FN_MUSIC_FADE, hle_register("fastload:music_fade", music_fade));
}
