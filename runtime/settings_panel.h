#pragma once
#include <stdbool.h>
// Called only by the overlay's game-thread frame builder.
void settings_panel(bool *open);

void settings_panel_request_close(void); // Thread-safe request; consumed by the game frame.
bool settings_panel_confirming(void);

void settings_panel_before_frame(bool open); // Own navigation keys before ImGui NewFrame.
void settings_panel_request_back(void); // F1 / chord: same one-level back route.
bool settings_panel_dirty(void); // Atomic for the host close-event handler.
int settings_panel_theme(void); // THEME_*: the one picked in the menu while it is open, else the saved one (game frame).
bool settings_panel_key(int scancode); // Choosing a key (keybind, chat): takes the key pressed (host event thread).
bool settings_panel_mouse(int button); // Choosing a keybind: takes the mouse button pressed away from the menu's rows.
bool settings_panel_wheel(int notches); // Choosing a keybind: takes the wheel turned (> 0 up) as Wheel Up / Down.
