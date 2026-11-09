// Game-action rebinding, the game side (runtime/actions.h): hooks on the engine's button translators.
#pragma once
#include <stdbool.h>
#include <stdint.h>

void input_view_init(void);          // loads [Actions*] and, with Enable=1, hooks the translators (nothing otherwise)
bool input_view_dash_zr(void);       // the game's Control Type makes ZR the dash role (any thread; a frame late)
int input_view_weapon(void);         // the weapon class the local hunter holds, 0..14 (-1: none seen)
const char *input_view_context(void);  // "FIELD", "TOWN" or "MENU" (the last frames)
void input_view_status(void);        // log the layer's state (control command "actstat")
