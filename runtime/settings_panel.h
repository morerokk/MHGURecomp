#pragma once
#include <stdbool.h>
// Called only by the overlay's game-thread frame builder.
void settings_panel(bool *open);

void settings_panel_request_close(void); // Thread-safe request; consumed by the game frame.
bool settings_panel_confirming(void);

void settings_panel_before_frame(bool open); // Own navigation keys before ImGui NewFrame.
void settings_panel_request_back(void); // F1 / chord: same one-level back route.
bool settings_panel_take_forge(void); // Game frame: the Mods tab's "Open the Forge menu" was chosen (once).
bool settings_panel_dirty(void); // Atomic for the host close-event handler.
int settings_panel_theme(void); // THEME_*: the one picked in the menu while it is open, else the saved one (game frame).
bool settings_panel_key(int scancode); // Choosing a key (keybind, chat): takes the key pressed (host event thread).
bool settings_panel_mouse(int button); // Choosing a keybind: takes the mouse button pressed away from the menu's rows.
#ifdef __cplusplus
extern "C" {
#endif
bool settings_panel_pad_capture(void); // Choosing a controller input for a game action: the host gives the next press.
int host_input_last_device(void); // (runtime/host_input.c) 0 the controller, 1 keyboard and mouse: the last one a button was pressed on.
int host_input_sw_sources(int btn, int dev, int *out, int max); // What Tier 0 reads for a Switch button (dev 0 controller, 1 keyboard).
int host_input_held(int *out, int max); // (runtime/host_input.c) The inputs down now (scancodes, KEY_MOUSE + button, ACT_PAD + button): the drawing lights them.
bool input_view_dash_zr(void); // (runtime/hle/input_view.c) The Control Type makes ZR the dash role.
void settings_panel_pad(int code); // That press (runtime/actions.h ACT_PAD + ...; host update thread, C).
void input_view_apply(void); // (runtime/hle/input_view.c) The profile in use changed: hook the game if it is on now.
#ifdef __cplusplus
}
#endif
bool settings_panel_wheel(int notches); // Choosing a keybind: takes the wheel turned (> 0 up) as Wheel Up / Down.
