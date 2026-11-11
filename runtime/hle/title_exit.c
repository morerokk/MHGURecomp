// B on the title screen's Main Menu asks to quit, as closing the window does (runtime/ui.c): the settings menu's
// Exit confirmation. The title sequence's own update (vtable slot 9, 0x87b544) keeps its state at +4; 0x11 is the
// Main Menu (docs/research/title-menu.md). Only there: the screens this menu leads to keep their own Back.
#include "../hle.h"
#include "../app_actions.h"
#include "../overlay.h"
#include "input.h"

#define FN_TITLE_UPDATE 0x87b544u  // the title sequence's update (r0 = the title object)
#define TITLE_STATE     4u         // u8: the state the update switches on
#define STATE_MAIN_MENU 0x11u

static void title_update(Cpu *c) {
    uint32_t title = c->r[0];  // (saved first: r0 is the return value after the call)
    fnhook_call_original(c, FN_TITLE_UPDATE);

    int32_t sticks[4];
    uint64_t buttons;
    input_get(&buttons, sticks);
    static uint64_t was;
    static bool swallow;  // (the B that cancelled the confirmation: wait for its release)
    bool down = (buttons >> BTN_B) & 1;
    bool pressed = down && !((was >> BTN_B) & 1);
    was = buttons;  // (kept fresh in every state: a B held over from another screen is no press here)

    if (overlay_menu_open()) { swallow = true; return; }  // (B there is the menu's, not the game's)
    if (swallow) { if (!down) swallow = false; return; }

    if (title < 0x08000000u || title >= 0xF0000000u || (title & 3)) return;
    if (LD8(title + TITLE_STATE) != STATE_MAIN_MENU) return;
    if (pressed) ui_request_exit_menu();
}

void title_exit_init(void) {
    fnhook_set(FN_TITLE_UPDATE, hle_register("titleexit:update", title_update));
}
