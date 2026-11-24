// The bottom screen's panel on Android's second display: second_display_android.c (the Java side:
// android/java/io/github/morerokk/mhgurecomp/DualScreen.java).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "../../config.h"  // (MHGU_DUAL_SCREEN)

#ifdef MHGU_DUAL_SCREEN
// Has the device a second display the bottom screen can go on (one the game's window isn't on)? DualScreen.java tells,
// and again whenever a display comes or goes. The settings menu greys [Dual Screen] SecondDisplay out without one.
bool second_display_available(void);

// [Dual Screen] SecondDisplay as it is now (at the start, after the settings menu's Save): the bottom screen shown on the
// second display, or taken off it (the HUD pieces go back to the main screen). Any thread.
void second_display_apply(void);

// The wallpaper to show now, w x h (upright, RGBA as the bottom picture, scaled to cover), from wallpapers/ in the app's
// files folder: the [Dual Screen] Wallpaper, or when it is empty each BMP there in turn, 30 s each, fading over 1 s into
// the next. All of them load once, when the setting changes. NULL: Background is not Wallpaper or no file.
// *changed: not the last call's (every call during a fade). One thread only (the panel's, or bottom_hud.c's test mode).
const uint32_t *second_display_wallpaper(uint32_t w, uint32_t h, bool *changed);
#endif
