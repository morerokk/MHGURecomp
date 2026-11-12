// Controller state the game reads (runtime/hle/input.c), set by the window loop.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "../actions.h"

// Switch button bits (NpadButton): A 0, B 1, X 2, Y 3, LStick 4, RStick 5, L 6, R 7, ZL 8, ZR 9, Plus 10, Minus 11,
// Left 12, Up 13, Right 14, Down 15. Sticks: -32767..32767, y up.
enum { BTN_A, BTN_B, BTN_X, BTN_Y, BTN_LSTICK, BTN_RSTICK, BTN_L, BTN_R, BTN_ZL, BTN_ZR, BTN_PLUS, BTN_MINUS,
       BTN_LEFT, BTN_UP, BTN_RIGHT, BTN_DOWN };
void input_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry);
void input_get(uint64_t *buttons, int32_t sticks[4]);  // the controller state the game sees
uint64_t input_npad_state(int32_t sticks[4]);  // what GetNpadState gives player 1: buttons (stick directions too), sticks
extern volatile uint64_t g_a_pressed_ns;  // when A was last pressed (plat_now_ns; 0: never)
// Rumble the game asks for (hid SendVibrationValues, runtime/services/misc.c): low / high frequency motor strength,
// 0..65535 each, for the window loop to send to the controller (runtime/host_input.c).
void input_set_rumble(uint16_t low, uint16_t high);
uint32_t input_rumble(void);  // low << 16 | high
// Game actions (runtime/actions.h): the host stages the set for the next input_set (which publishes both together),
// the game thread latches the one that was current when it last polled the pad (runtime/hle/input_view.c).
void input_act_stage(const ActSnap *s);
void input_act_latch(ActSnap *out);  // the pulses are given once
