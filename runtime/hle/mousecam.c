// Mouse look on the hunt camera's own angles, not through the right stick. The stick turns the camera at a speed
// (past a round dead zone of about 39% tilt, faster with more tilt), so a mouse fed into it can't make small precise
// turns; here each count moved turns the camera by a fixed angle (MOUSECAM_DEG_PER_COUNT x [Mouse]
// MouseSensitivity), as in a PC game.
//
// The quest camera (uQuestCamera, 0xb00 bytes) keeps its angles as 16-bit units (0x10000 = a full turn): +0x9e8 /
// +0x9ec the pitch / yaw it heads for, +0x9e0 / +0x9e4 where it is (smoothed toward those each update), +0x9d8 /
// +0x9dc the angles the eye is placed by (the smoothed ones, the pitch then lifted off the ground). The stick handler
// (main+0x255CC; r0 = the camera, r1 = the game's invert option: bit 0 up / down, bit 1 left / right, from options
// byte +0x64, copied to +0x97c, or +0x980's for the second camera type) adds the stick's turn to +0x9e8 / +0x9ec,
// pitch clamped to +-0x3f00 (89 degrees). The game calls it only while the player may turn the camera (not in
// cutscenes, menus, while carving...), so it is hooked: after it, the mouse's movement since the last update goes to
// both the heading and the smoothed angles (no smoothing lag), with the game's invert option.
// Where it doesn't run (villages, whose camera doesn't turn, or other cameras) host_input.c gives the mouse to the
// right stick as before (mousecam_active).
#include <pthread.h>

#include "../config.h"
#include "../hle.h"
#include "mousecam.h"

#define FN_CAMERA_STICK 0x2255ccu  // uQuestCamera's right stick handler: r0 = the camera, r1 = the invert bits
#define QC_PITCH_NOW    0x9e0u
#define QC_YAW_NOW      0x9e4u
#define QC_PITCH_TO     0x9e8u
#define QC_YAW_TO       0x9ecu
#define PITCH_MAX       0x3f00     // (the stick handler's clamp)
#define MOUSECAM_DEG_PER_COUNT 0.08f
#define ACTIVE_NS 200000000ull     // the handler ran this recently: the mouse is the camera's

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static float g_dx, g_dy;           // counts moved since the handler last ran (x right, y down)
static volatile uint64_t g_seen_ns;  // when the handler last ran

static bool sane(uint32_t a) { return a >= 0x08000000u && a < 0xF0000000u && !(a & 3); }

bool mousecam_active(void) {
    uint64_t seen = g_seen_ns;
    return seen && plat_now_ns() - seen < ACTIVE_NS;
}

void mousecam_add(float dx, float dy) {
    pthread_mutex_lock(&g_lock);
    g_dx += dx, g_dy += dy;
    pthread_mutex_unlock(&g_lock);
}

static int clamp_pitch(int p) { return p < -PITCH_MAX ? -PITCH_MAX : p > PITCH_MAX ? PITCH_MAX : p; }

static void camera_stick(Cpu *c) {
    uint32_t cam = c->r[0], invert = c->r[1];
    fnhook_call_original(c, FN_CAMERA_STICK);
    uint64_t now = plat_now_ns(), seen = g_seen_ns;
    g_seen_ns = now;
    pthread_mutex_lock(&g_lock);
    float dx = g_dx, dy = g_dy;
    g_dx = g_dy = 0;
    pthread_mutex_unlock(&g_lock);
    static float rest_yaw, rest_pitch;  // (the fractions of a unit left over, so slow movements add up)
    if (!seen || now - seen >= ACTIVE_NS) rest_yaw = rest_pitch = 0, dx = dy = 0;  // (moved while it didn't run)
    if ((dx == 0 && dy == 0) || !sane(cam)) return;
    float s = MOUSECAM_DEG_PER_COUNT * (65536.0f / 360.0f) * config_live_mouse_sensitivity();
    float yaw = -dx * s, pitch = -dy * s;  // (right turns the yaw down, up raises the pitch: as the stick)
    if (invert & 1) pitch = -pitch;
    if (invert & 2) yaw = -yaw;
    yaw += rest_yaw, pitch += rest_pitch;
    int iy = (int)yaw, ip = (int)pitch;
    rest_yaw = yaw - (float)iy, rest_pitch = pitch - (float)ip;
    if (iy) {
        ST32(cam + QC_YAW_TO, (LD32(cam + QC_YAW_TO) + (uint32_t)iy) & 0xffffu);
        ST32(cam + QC_YAW_NOW, (LD32(cam + QC_YAW_NOW) + (uint32_t)iy) & 0xffffu);
    }
    if (ip) {
        int to = (int16_t)LD32(cam + QC_PITCH_TO), next = clamp_pitch(to + ip);
        int now_p = clamp_pitch((int16_t)LD32(cam + QC_PITCH_NOW) + next - to);
        ST32(cam + QC_PITCH_TO, (uint32_t)next & 0xffffu);
        ST32(cam + QC_PITCH_NOW, (uint32_t)now_p & 0xffffu);
        if (next != to + ip) rest_pitch = 0;  // (at the limit)
    }
}

void mousecam_init(void) {
    fnhook_set(FN_CAMERA_STICK, hle_register("mousecam:camera_stick", camera_stick));
}
