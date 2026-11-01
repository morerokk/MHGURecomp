// nn::hid on the host: the game's controller (player 1, a Pro Controller / FullKey style, docked) reads the state the
// window loop sets (input_set, runtime/ui.c: keyboard and SDL gamepads). Vibration, six-axis and keyboard calls are
// accepted but inert. The SDK's own hid client (shared memory, applet resource) is never used.
#include <string.h>

#include "../hle.h"
#include "../kernel.h"
#include "chat.h"
#include "input.h"
#include "../net/discord.h"
#include "../net/net.h"

static volatile uint64_t g_buttons;
static volatile int32_t g_sticks[4];  // lx, ly, rx, ry (-32767..32767, y up)
static int64_t sampling;

volatile uint64_t g_a_pressed_ns;

void input_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    if ((buttons & ~g_buttons) & (1ull << BTN_A)) g_a_pressed_ns = plat_now_ns();
    g_buttons = buttons;
    g_sticks[0] = lx, g_sticks[1] = ly, g_sticks[2] = rx, g_sticks[3] = ry;
}

void input_get(uint64_t *buttons, int32_t sticks[4]) {
    *buttons = g_buttons;
    for (int i = 0; i < 4; i++) sticks[i] = g_sticks[i];
}

// The state of player 1's pad as GetNpadState gives it: the buttons (a stick tilted past half is also a button, bits
// 16-23) and the four sticks
uint64_t input_npad_state(int32_t sticks[4]) {
    uint64_t b = g_buttons;
    int32_t lx = g_sticks[0], ly = g_sticks[1], rx = g_sticks[2], ry = g_sticks[3];
    if (lx < -16000) b |= 1ull << 16;
    if (ly > 16000) b |= 1ull << 17;
    if (lx > 16000) b |= 1ull << 18;
    if (ly < -16000) b |= 1ull << 19;
    if (rx < -16000) b |= 1ull << 20;
    if (ry > 16000) b |= 1ull << 21;
    if (rx > 16000) b |= 1ull << 22;
    if (ry < -16000) b |= 1ull << 23;
    sticks[0] = lx, sticks[1] = ly, sticks[2] = rx, sticks[3] = ry;
    return b;
}

// NpadFullKeyState (and the other styles: same layout): i64 sampling, u64 buttons, left stick {i32 x, y}, right
// stick, u32 attributes. GetNpadState(state *, const NpadId &)
static void fill_state(Cpu *c) {
    emsync_tick();  // (multiplayer small monster sync: on the game's thread, once a frame is plenty)
    hpscale_tick();  // (the hub host's [Monster HP] settings to the others)
    chat_tick(c);  // (a line typed after "/": sent as a shoutout is, runtime/hle/chat.c)
    presence_tick();  // (Discord Rich Presence, once a second: runtime/hle/presence.c)
    uint8_t *s = ARGP(0);
    uint32_t id = LD32(ARG(1));
    memset(s, 0, 40);
    int64_t n = __atomic_add_fetch(&sampling, 1, __ATOMIC_RELAXED);
    memcpy(s, &n, 8);
    if (id != 0) return;  // docked: only player 1
    int32_t st[4];
    uint64_t b = input_npad_state(st);
    int32_t lx = st[0], ly = st[1], rx = st[2], ry = st[3];
    memcpy(s + 8, &b, 8);
    memcpy(s + 16, &lx, 4);
    memcpy(s + 20, &ly, 4);
    memcpy(s + 24, &rx, 4);
    memcpy(s + 28, &ry, 4);
    uint32_t attr = 1;  // IsConnected
    memcpy(s + 32, &attr, 4);
}
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj", hid_state_fullkey) { fill_state(c); }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj", hid_state_dual) { fill_state(c); }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyLeftStateERKj", hid_state_left) { fill_state(c); }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj", hid_state_handheld) { fill_state(c); }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_17NpadJoyRightStateERKj", hid_state_right) { fill_state(c); }

HLE_FUNC("_ZN2nn3hid15GetNpadStyleSetERKj", hid_style_set) { RET(LD32(ARG(0)) == 0 ? 1 : 0); }  // FullKey, player 1

// BindNpadStyleSetUpdateEvent(id, SystemEventType *, clear mode): signalled once, so the game sees a controller.
HLE_FUNC("_ZN2nn3hid27BindNpadStyleSetUpdateEventERKjPNS_2os15SystemEventTypeENS3_14EventClearModeE", hid_bind_style_event) {
    uint32_t w, r;
    KEvent *ev = kernel_create_event_pair(&w, &r);
    pthread_mutex_lock(&g_klock);
    kernel_signal(ev);
    pthread_mutex_unlock(&g_klock);
    uint32_t clear_mode = c->r[2];
    c->r[0] = c->r[1];
    c->r[1] = r;
    c->r[2] = 0;
    c->r[3] = clear_mode;
    guest_call(c, guest_sym("_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE"));
}

HLE_FUNC("_ZN2nn3hid14InitializeNpadEv", hid_init_npad) { (void)c; }
HLE_FUNC("_ZN2nn3hid18InitializeKeyboardEv", hid_init_keyboard) { (void)c; }
HLE_FUNC("_ZN2nn3hid16GetKeyboardStateEPNS0_13KeyboardStateE", hid_keyboard_state) {
    int64_t n = __atomic_add_fetch(&sampling, 1, __ATOMIC_RELAXED);
    memset(ARGP(0), 0, 0x38);
    memcpy(ARGP(0), &n, 8);
}
HLE_FUNC("_ZN2nn3hid22SetSupportedNpadIdTypeEPKjj", hid_set_ids) { (void)c; }
HLE_FUNC("_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", hid_set_styles) { (void)c; }
HLE_FUNC("_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE", hid_set_hold) { (void)c; }
HLE_FUNC("_ZN2nn3hid18GetNpadJoyHoldTypeEv", hid_get_hold) { RET(1); }  // horizontal
HLE_FUNC("_ZN2nn3hid29SetNpadHandheldActivationModeENS0_26NpadHandheldActivationModeE", hid_set_handheld_mode) { (void)c; }
HLE_FUNC("_ZN2nn3hid29GetNpadHandheldActivationModeEv", hid_get_handheld_mode) { RET(0); }
HLE_FUNC("_ZN2nn3hid20GetNpadJoyAssignmentERKj", hid_joy_assignment) { RET(0); }  // dual
HLE_FUNC("_ZN2nn3hid14DisconnectNpadERKj", hid_disconnect) { (void)c; }
// GetNpadControllerColor(color *, id) / (left *, right *, id): Result
HLE_FUNC("_ZN2nn3hid22GetNpadControllerColorEPNS0_19NpadControllerColorERKj", hid_color) {
    static const uint32_t col[2] = {0xFF2D2D2D, 0xFFE6E6E6};  // main, sub
    memcpy(ARGP(0), col, 8);
    RET(0);
}
HLE_FUNC("_ZN2nn3hid22GetNpadControllerColorEPNS0_19NpadControllerColorES2_RKj", hid_color2) {
    static const uint32_t col[2] = {0xFF2D2D2D, 0xFFE6E6E6};
    memcpy(ARGP(0), col, 8);
    memcpy(ARGP(1), col, 8);
    RET(0);
}
// ShowControllerSupport(result *, const arg &): Result. Result info: player count, selected id.
HLE_FUNC("_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE", hid_show_support) {
    if (ARG(0)) { ST32(ARG(0), 1); ST32(ARG(0) + 4, 0); }
    RET(0);
}

// ---- six-axis sensors: present, always at rest
HLE_FUNC("_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", hid_sixaxis_handles) {
    int n = (int)ARG(1) < 2 ? (int)ARG(1) : 2;
    uint32_t id = LD32(ARG(2));
    for (int i = 0; i < n; i++) ST32(ARG(0) + 4u * (uint32_t)i, (id & 0xFF) << 8 | (uint32_t)i | 0x03000000u);
    RET(n);
}
HLE_FUNC("_ZN2nn3hid18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE", hid_sixaxis_start) { (void)c; }
HLE_FUNC("_ZN2nn3hid17StopSixAxisSensorERKNS0_19SixAxisSensorHandleE", hid_sixaxis_stop) { (void)c; }
HLE_FUNC("_ZN2nn3hid21IsSixAxisSensorAtRestERKNS0_19SixAxisSensorHandleE", hid_sixaxis_rest) { RET(1); }
HLE_FUNC("_ZN2nn3hid21GetSixAxisSensorStateEPNS0_18SixAxisSensorStateERKNS0_19SixAxisSensorHandleE", hid_sixaxis_state) {
    uint8_t *s = ARGP(0);
    memset(s, 0, 0x60);
    int64_t n = __atomic_add_fetch(&sampling, 1, __ATOMIC_RELAXED);
    memcpy(s + 8, &n, 8);
    float one = 1.0f;  // acceleration 1 g down, identity orientation
    memcpy(s + 0x18 + 8, &one, 4);
}

// ---- vibration: the SDK's vibration player / mixer graph stays recompiled and sends its values every 5 ms through
// hid SendVibrationValues (runtime/services/misc.c), which sets the rumble the window loop gives the controller.
static volatile uint32_t g_rumble;
void input_set_rumble(uint16_t low, uint16_t high) { __atomic_store_n(&g_rumble, (uint32_t)low << 16 | high, __ATOMIC_RELAXED); }
uint32_t input_rumble(void) { return __atomic_load_n(&g_rumble, __ATOMIC_RELAXED); }
HLE_FUNC("_ZN2nn3hid25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE", hid_init_vibration) { (void)c; }
HLE_FUNC("_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", hid_vibration_handles) {
    int n = (int)ARG(1) < 2 ? (int)ARG(1) : 2;
    uint32_t id = LD32(ARG(2));
    for (int i = 0; i < n; i++) ST32(ARG(0) + 4u * (uint32_t)i, (id & 0xFF) << 8 | (uint32_t)i | 0x03000000u);
    RET(n);
}
HLE_FUNC("_ZN2nn3hid22GetVibrationDeviceInfoEPNS0_19VibrationDeviceInfoERKNS0_21VibrationDeviceHandleE", hid_vibration_info) {
    uint32_t h = LD32(ARG(1));
    ST32(ARG(0), 1);      // VibrationDeviceType_LinearResonantActuator
    ST32(ARG(0) + 4, h & 1);  // position: left / right
}
HLE_FUNC("_ZN2nn3hid23GetActualVibrationValueEPNS0_14VibrationValueERKNS0_21VibrationDeviceHandleE", hid_vibration_actual) {
    static const float v[4] = {0, 160.0f, 0, 320.0f};  // amplitude low, frequency low, amplitude high, frequency high
    memcpy(ARGP(0), v, 16);
}
