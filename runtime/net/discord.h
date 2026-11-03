// Discord Rich Presence: what the player is doing, on their Discord profile (ini [Discord]). discord.c talks to the
// Discord app on this computer; runtime/hle/presence.c reads the game and says what to show.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// What Discord shows under the application's name ("Monster Hunter GU Recomp"). Empty strings are left out.
typedef struct {
    char details[128];     // the first line ("Hunting Khezu, Volvidon", "Bherna Village")
    char state[128];       // the second line ("Jurassic Frontier, Area 4"); the party size goes after it
    char large_image[64];  // an image's key or https URL; its hover text
    char large_text[128];
    char small_image[64];  // (the small round picture on the large one)
    char small_text[128];
    int64_t start;         // Unix time the elapsed timer counts from (0: none)
    int party, party_max;  // "(2 of 4)" (0: none)
    char party_id[64];     // the same for every player of a party
} DiscordActivity;

void discord_init(void);
// Called whenever the game's state may have changed (cheap when nothing did): sent to Discord at once, or as soon
// as Discord's rate limit allows. NULL: nothing is shown.
void discord_set_activity(const DiscordActivity *a);
// runtime/hle/presence.c: the game's state into an activity (each frame, from the game's input polling)
void presence_tick(void);

#ifdef __cplusplus
}
#endif
