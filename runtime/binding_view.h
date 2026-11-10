// The visual binding view of the Game actions screen: a controller drawn as line art (or a keyboard and a mouse) with
// every bound input lit and an arrow from it to a label naming the actions on it. The selected input glows, so the
// player sees what they are setting; the inputs held right now glow too; clicking an input on the drawing hands it to
// the screen to bind. Pure ImGui draw-list code: no game state, no settings panel internals, no input handling beyond
// the mouse over its own rectangle.
//
// ---------------------------------------------------------------------------------------------------------------
// INTEGRATION GUIDE (phase 2: the Game actions screen, runtime/settings_panel.cpp)
//
// 1. Build the data once per frame (or when the bindings change; the module caches its layout by content):
//      std::vector<BVAction> acts;  std::vector<BVInput> ins;
//      for each input code that has an action on it for the shown scheme and layout:
//          ins.push_back({code, &acts[first], count, conflict});   // acts of one input are contiguous
//      BVAction.label is the SHORT name the chip prints ("Dodge", "Confirm"): it must outlive the call; id is the
//      action id (act_find / ActId) and selected marks the row being edited (its input glows, its line is lit).
//    The input codes are the runtime's (runtime/actions.h): ACT_PAD + SDL_GAMEPAD_BUTTON_*, ACT_PAD_LT / ACT_PAD_RT,
//    SDL scancodes, KEY_MOUSE + SDL_BUTTON_*, KEY_WHEEL_UP / DOWN. The sticks' four directions have codes of their
//    own (BV_STICK_*, not bindable yet: they show once something uses them). Pass the EFFECTIVE binding of each
//    action (act_effective: its own, the one it follows, or the default's Switch button turned into the position
//    the device gives it) so the drawing is complete before the player has bound anything.
// 2. Fill BVParams:
//      p.scheme   = BV_CONTROLLER or BV_KEYBOARD (the scheme the screen shows: the keyboard scheme draws the
//                   keyboard and the mouse; mouse buttons and the wheel are part of it)
//      p.inputs / p.n_inputs
//      p.selected_input = the code of the input the selected row reads now (0: none): glows, pulses while capturing
//      p.capturing      = the screen is waiting for the player to press an input (the glow breathes)
//      p.focus_input    = an input the controller-only focus is on (e.g. the first input of the focused row): ring
//      p.held / p.n_held = inputs physically down this frame (act_phys_held for each code, or the host's states)
//      p.time_s     = ImGui::GetTime()
//      p.colors     = bv_colors_from(panel, line, text, text_dim, accent, accent_bright, warn, live) from the
//                     menu's Theme (the three themes and light panels are handled by the colours alone)
//      p.font_px    = the menu's base font size in pixels (ImGui::GetFontSize() inside the panel): everything scales
//                     from it, so a menu scale of 150% or a 4K monitor needs nothing else.
// 3. Size and draw it inside the panel's window, where a row would go:
//      float w = ImGui::GetContentRegionAvail().x;
//      float h = bv_height(p, w);                     // the height it wants for this width (cached layout)
//      BVResult r = bv_draw("##bindview", w, h, p);   // advances the cursor by w x h
//      if (r.clicked)  -> bind r.clicked to the selected action (the same path as a captured press)
//      r.hovered       -> the input under the mouse (0 none), to show its text in the help box if wanted
//    Clicking a key the recomp keeps for itself (Esc, F1, Insert, F7-F12: key_reserved) is reported in r.reserved
//    and never as clicked; those keys are drawn dimmed. p.click_hint is the last line of the tooltip ("Click to bind
//    it to Dodge", or nullptr when no row is being edited). bv_input_center() gives where an input is drawn.
// 4. Controller-only menus: nothing is focusable in the drawing (it takes no navigation focus); set p.focus_input
//    to the input of the focused row and p.selected_input to the input of the row being captured.
// 5. Theme colours (settings_panel.cpp Theme): panel = t.panel, line = t.text_dim, text = t.text, text_dim = t.text_dim
//    (a notch darker is fine), accent = t.accent, accent_bright = t.accent_bright, warn = t.warn, live = a green or
//    blue that is not the accent (the previews use (120,240,170), (150,214,150), (110,220,255) and (24,124,170)).
// 6. Build: add $(O)/rt/binding_view.o to RT_OBJ in the Makefile next to settings_panel.o, with the same rule
//      $(O)/rt/binding_view.o: runtime/binding_view.cpp runtime/binding_view.h runtime/config.h runtime/keymap.h
//      	@mkdir -p $(dir $@)
//      	$(CXX) $(IMGUIFLAGS) -Wall -Iruntime -c $< -o $@
//    and the same object to the test-settings-panel prerequisites (settings_panel.o will then need it). The module
//    uses ImGui, SDL3's scancode names and the key_reserved() of keymap.h; nothing of the runtime is linked.
//    New texts for the Spanish catalogue: none (the chips print the screen's own action labels and bv_input_name's
//    English input names: translate those two in settings_locale.h if the screen passes them through tr()).
// 7. Preview without the game: `make preview-bindings` renders every state to PNG files (obj/binding-preview/, one
//    scenario: make preview-bindings ONLY=pad_typical) and runs a mouse test (hover, click, reserved key).
//    Cost: a layout is built when the width, the font size or the bindings change (about 1 to 6 ms, 17 ms for 66
//    keys bound), never for a hover, a selection or a held key; drawing is a few hundred draw-list shapes.
// ---------------------------------------------------------------------------------------------------------------
#pragma once
#include "imgui.h"

enum BVScheme { BV_CONTROLLER = 0, BV_KEYBOARD = 1 };

// Codes for the sticks' directions (runtime/actions.h's ACT_PAD + 0x48...: free there): reserved for stick actions.
#define BV_STICK_L_UP    (0x2000 + 0x48)
#define BV_STICK_L_DOWN  (0x2000 + 0x49)
#define BV_STICK_L_LEFT  (0x2000 + 0x4a)
#define BV_STICK_L_RIGHT (0x2000 + 0x4b)
#define BV_STICK_R_UP    (0x2000 + 0x4c)
#define BV_STICK_R_DOWN  (0x2000 + 0x4d)
#define BV_STICK_R_LEFT  (0x2000 + 0x4e)
#define BV_STICK_R_RIGHT (0x2000 + 0x4f)

struct BVAction {
    int id;             // the action's id (the caller's)
    const char *label;  // short name for the chip
    bool selected;      // the row being edited
};
struct BVInput {
    int code;                 // the runtime's input code
    const BVAction *actions;  // the actions on it, contiguous
    int n;
    bool conflict;            // more than one of them in one context
};
struct BVColors {
    ImU32 panel;          // what the drawing sits on (also the text colour on a lit input)
    ImU32 line;           // outlines of the unlit inputs
    ImU32 text, text_dim;
    ImU32 accent, accent_bright;  // the selection's glow
    ImU32 warn;                   // conflicts
    ImU32 live;                   // inputs held right now
};
BVColors bv_colors_from(ImU32 panel, ImU32 line, ImU32 text, ImU32 text_dim, ImU32 accent, ImU32 accent_bright,
                        ImU32 warn, ImU32 live);

struct BVParams {
    int scheme = BV_CONTROLLER;
    const BVInput *inputs = nullptr;
    int n_inputs = 0;
    int selected_input = 0;
    int focus_input = 0;
    bool capturing = false;
    const int *held = nullptr;
    int n_held = 0;
    float time_s = 0;
    const char *click_hint = "Click to give it to the selected action.";  // last line of an input's tooltip (nullptr: none)
    BVColors colors = {};
    float font_px = 13.f;
};
struct BVResult {
    int clicked = 0;   // the input the player clicked (0 none)
    int hovered = 0;   // the input under the mouse (0 none)
    bool reserved = false;  // the click or hover is on a key the recomp keeps for itself
};

float bv_height(const BVParams &p, float width);
// Where an input is drawn, relative to the widget's top left corner (for height == bv_height): false if it has no
// place on this drawing (a key the keyboard does not show). For a pointer, a tooltip or a test.
bool bv_input_center(const BVParams &p, float width, int code, ImVec2 *out);
BVResult bv_draw(const char *id, float width, float height, const BVParams &p);
// "South button", "Left Shift", "Mouse 4", "Wheel up": what the chip's first line says
void bv_input_name(int code, char *buf, int size);
