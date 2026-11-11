// Host-menu policy only. Never changes the Npad state delivered to gameplay.
#pragma once
#include "actions.h"
#include "config.h"
#include <stdint.h>
// The policy the host menus use: what the game's Confirm is bound to on the controller (runtime/actions.h), else the
// [Controls] MenuConfirm setting
static inline int menu_confirm_policy_now(void) {
    int p = act_menu_confirm_policy();
    return p >= 0 ? p : config_live_menu_confirm();
}
static inline int menu_confirm_position(int policy, int original_a, int original_b, int cancel) {
    if (policy == MENU_CONFIRM_SOUTH) return cancel ? 1 : 0;
    if (policy == MENU_CONFIRM_EAST) return cancel ? 0 : 1;
    return cancel ? original_b : original_a;
}

// Semantic A/B from the physical policy; no gameplay state is written.
static inline void menu_confirm_buttons(int policy, int original_a_position,
                                        int original_b_position, int a, int b,
                                        int *confirm, int *cancel) {
    int position = menu_confirm_position(policy, original_a_position, original_b_position, 0);
    *confirm = position == original_a_position ? a : b;
    *cancel = position == original_a_position ? b : a;
}

// Suppress held face buttons when an overlay returns control to gameplay.
static inline uint64_t menu_release_buttons(uint64_t buttons, uint64_t face_mask, bool *waiting) {
    if (!*waiting) return buttons;
    if (!(buttons & face_mask)) *waiting = false;
    return buttons & ~face_mask;
}
