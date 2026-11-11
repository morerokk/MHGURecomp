// Game-action rebinding, the game side (runtime/actions.h): hooks on the engine's button translators.
#pragma once
#include <stdbool.h>
#include <stdint.h>

void input_view_apply(void);         // (any thread: the settings menu's Save) the profile changed: hook the game if it is on now and was not
// Loads [Actions*] and, with Enable=1, hooks the translators. The hunt's pause menu handler is hooked either way (only
// timed, for [Mouse] MouseCapture = Hunt: input_view_hunt_menu_open)
void input_view_init(void);
bool input_view_dash_zr(void);       // the game's Control Type makes ZR the dash role (any thread; a frame late)
int input_view_weapon(void);         // the weapon class the local hunter holds, 0..14 (-1: none seen)
const char *input_view_context(void);  // "FIELD", "TOWN" or "MENU" (the last frames)
bool input_view_hunt_menu_open(void);  // a hunt's pause menu (or the pouch...) is open: its handler stopped running (any thread)
void input_view_hunt_menu_reset(void); // (the host, when no hunt is running: a new hunt starts with no menu)
void input_view_status(void);        // log the layer's state (control command "actstat")
