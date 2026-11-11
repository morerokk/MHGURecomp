// B on the title screen's Main Menu asks to quit, as closing the window does (runtime/ui.c): the settings menu's
// Exit confirmation. The title sequence's own update (vtable slot 9, 0x87b544) keeps its state at +4; 0x11 is the
// Main Menu and 0x10 the screen it arrives from (docs/research/title-menu.md). Elsewhere the menu's own Back applies.
#include "../hle.h"
#include "../app_actions.h"
#include "../overlay.h"
#include "input.h"

#define FN_TITLE_UPDATE 0x87b544u  // the title sequence's update (r0 = the title object)
#define TITLE_STATE     4u         // u8: the state the update switches on
#define STATE_MENU_IN   0x10u
#define STATE_MAIN_MENU 0x11u

static void title_update(Cpu *c) {
    uint32_t title = c->r[0];  // (saved first: r0 is the return value after the call)
    // +4 before the call, not after: state 0x11's handler is what acts on the pad, and B there leaves the screen in
    // the same update, so afterwards +4 holds where it went back to and the guard below would never match
    bool valid = title >= 0x08000000u && title < 0xF0000000u && !(title & 3);
    uint8_t state = valid ? LD8(title + TITLE_STATE) : 0xffu;
    // 0x10 included: it is where the Main Menu arrives from, and the game itself does nothing with B there
    bool main_menu = valid && (state == STATE_MAIN_MENU || state == STATE_MENU_IN);
    fnhook_call_original(c, FN_TITLE_UPDATE);

    int32_t sticks[4];
    uint64_t buttons;
    input_get(&buttons, sticks);
    static uint64_t was;
    static bool swallow;  // (the B that cancelled the confirmation: wait for its release)
    static bool armed;    // (a B pressed on the Main Menu: the request waits for its release)
    bool down = (buttons >> BTN_B) & 1;
    bool pressed = down && !((was >> BTN_B) & 1);
    was = buttons;  // (kept fresh in every state: a B held over from another screen is no press here)

    if (overlay_menu_open()) { swallow = true; armed = false; return; }  // (B there is the menu's, not the game's)
    if (swallow) {
        if (!down) { swallow = false; armed = false; }
        return;
    }

    // The request waits for the release: the settings menu opens on the next presented frame, and a B still held by
    // then is its Cancel at the main view (runtime/settings_panel.cpp, GamepadFaceRight), closing the question again
    if (main_menu && pressed) armed = true;
    if (armed && !down) { armed = false; ui_request_exit_menu(); }
}

void title_exit_init(void) {
    fnhook_set(FN_TITLE_UPDATE, hle_register("titleexit:update", title_update));
}
