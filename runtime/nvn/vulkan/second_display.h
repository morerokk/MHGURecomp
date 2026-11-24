// The bottom screen's panel on a second display (bottom_hud.c makes its picture): second_display.c (what every platform
// shows), second_display_android.c (Android's second display: android/java/.../DualScreen.java),
// second_display_desktop.c (PC: an SDL window on another monitor).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "../../config.h"  // (MHGU_DUAL_SCREEN)

#ifdef MHGU_DUAL_SCREEN
#ifdef __cplusplus
extern "C" {
#endif
union SDL_Event;

// [Dual Screen] SecondDisplay or its options changed (the start, the settings menu's Save): applied by the UI thread at its
// next turn (second_display_tick). Any thread. (config_second_display_request, for code that has no renderer.)
void second_display_apply(void);

// The UI thread, each turn of its loop (ui.c ui_run): a change asked for applied, and on PC the window kept up (made,
// moved, its picture shown) while the setting is on.
void second_display_tick(void);

// The UI thread, each event before the game sees it: true when it was the bottom screen window's own (a touch or a click
// on it, its window events), which the game must not see. Android: always false (its touches come through Java).
bool second_display_event(const union SDL_Event *e);

// Is the bottom screen's own window open (PC)? While it is, closing the game's window sends no SDL_EVENT_QUIT (that
// comes with the last window), so ui.c takes the game window's close request for it.
bool second_display_window_open(void);

// ---- the picture, the same on every platform (second_display.c)
// What the panel shows, w x h RGBA8 (opaque): the game's newest bottom picture when there is one, the background alone
// before the first and a while after the last (out of a quest), with the wallpaper round the panels. Call it as often as
// the panel can change (60 times a second); true when frame changed. One thread only, with its own state.
typedef struct {
    uint64_t hud_seq;  // (the game's last bottom picture shown, 0 none yet)
    bool empty_shown;
} SecondDisplayState;
bool second_display_compose(uint32_t *frame, uint32_t w, uint32_t h, SecondDisplayState *st);

// The wallpaper to show now, w x h (upright, RGBA as the bottom picture, scaled to cover), from wallpapers/ beside the
// game (Android: in the app's files folder): the [Dual Screen] Wallpaper, or when it is empty each BMP there in turn,
// 30 s each, fading over 1 s into the next. All of them load once, when the setting changes. NULL: Background is not
// Wallpaper or no file. *changed: not the last call's (every call during a fade). One thread only (the one composing,
// or bottom_hud.c's test mode).
const uint32_t *second_display_wallpaper(uint32_t w, uint32_t h, bool *changed);
#ifdef __cplusplus
}
#endif
#endif
