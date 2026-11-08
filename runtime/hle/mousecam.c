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
//
// While aiming (the Bow's R, a Bowgun's aim...) the stick turns the hunter instead and the camera follows: the
// handler isn't called, the hunter's turn is applied by 0x4a1738 (r0 = the player, r1 = which weapon's aim), to the
// hunter's yaw +0x14a0 and aim pitch +0x14b8 / +0x14bc (a float, -100 to 100; +0x2714, 16-bit units, for mode 1). The
// aim camera (0x226e84) takes +0x9ec from that yaw and +0x9e8 from that pitch, spread over its aim limits (camera
// parameters 0x1d up and 0x1e down, degrees), and steps +0x9d8 / +0x9dc straight toward them. So 0x4a1738 is hooked
// too, with the invert option for aiming (options byte +0x66), adding to the eye's angles as well (no lag).
//
// Where neither runs (villages, whose camera doesn't turn, or other cameras) host_input.c gives the mouse to the
// right stick as before (mousecam_active).
#include <pthread.h>

#include "../config.h"
#include "../hle.h"
#include "mousecam.h"

#define FN_CAMERA_STICK 0x2255ccu  // uQuestCamera's right stick handler: r0 = the camera, r1 = the invert bits
#define FN_AIM_TURN     0x4a1738u  // the hunter's turn while aiming: r0 = the player, r1 = the mode
#define FN_CAM_PARAM    0x21eadcu  // (camera, parameter): s0 = the camera's parameter
#define G_OPTIONS       0x1a8ca48u  // the game's options object *
#define OPT_AIM_INVERT  0x66u      // options byte: the invert bits while aiming
#define QC_PLAYER       0x85cu
#define QC_EYE_PITCH    0x9d8u
#define QC_EYE_YAW      0x9dcu
#define QC_PITCH_NOW    0x9e0u
#define QC_YAW_NOW      0x9e4u
#define QC_PITCH_TO     0x9e8u
#define QC_YAW_TO       0x9ecu
#define QC_PARAM_AIM_UP   0x1du
#define QC_PARAM_AIM_DOWN 0x1eu
#define PL_AIM_YAW      0x14a0u
#define PL_AIM_PITCH    0x14b8u    // (the float, and +4 the copy the camera reads)
#define PL_AIM_PITCH2   0x14bcu
#define PL_AIM_LEAN     0x2714u    // (mode 1's pitch: 16-bit units, +-AIM_LEAN_MAX)
#define AIM_LEAN_MAX    0x2000
#define AIM_PITCH_MAX   100.0f
#define UNITS_PER_DEG   (65536.0f / 360.0f)
#define PITCH_MAX       0x3f00     // (the stick handler's clamp)
#define MOUSECAM_DEG_PER_COUNT 0.08f
#define ACTIVE_NS 200000000ull     // the handler ran this recently: the mouse is the camera's

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static float g_dx, g_dy;           // counts moved since the handler last ran (x right, y down)
static volatile uint64_t g_seen_ns;  // when a handler last ran
static float g_rest_yaw, g_rest_pitch;  // (the fractions of a unit left over, so slow movements add up)
static uint32_t g_cam;             // (the game's thread) the camera the stick handler last had
static uint32_t g_stick_runs;      // (the game's thread) how many times it ran

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

// The mouse's movement since the last update, as whole units of yaw and pitch (false: it didn't move).
static bool take_turn(uint32_t invert, int *iy, int *ip) {
    uint64_t now = plat_now_ns(), seen = g_seen_ns;
    g_seen_ns = now;
    pthread_mutex_lock(&g_lock);
    float dx = g_dx, dy = g_dy;
    g_dx = g_dy = 0;
    pthread_mutex_unlock(&g_lock);
    if (!seen || now - seen >= ACTIVE_NS) g_rest_yaw = g_rest_pitch = 0, dx = dy = 0;  // (moved while it didn't run)
    if (dx == 0 && dy == 0) return false;
    float s = MOUSECAM_DEG_PER_COUNT * UNITS_PER_DEG * config_live_mouse_sensitivity();
    float yaw = -dx * s, pitch = -dy * s;  // (right turns the yaw down, up raises the pitch: as the stick)
    if (invert & 1) pitch = -pitch;
    if (invert & 2) yaw = -yaw;
    yaw += g_rest_yaw, pitch += g_rest_pitch;
    *iy = (int)yaw, *ip = (int)pitch;
    g_rest_yaw = yaw - (float)*iy, g_rest_pitch = pitch - (float)*ip;
    return true;
}

static int clamp_pitch(int p) { return p < -PITCH_MAX ? -PITCH_MAX : p > PITCH_MAX ? PITCH_MAX : p; }

static void camera_stick(Cpu *c) {
    uint32_t cam = c->r[0], invert = c->r[1];
    fnhook_call_original(c, FN_CAMERA_STICK);
    g_cam = cam, g_stick_runs++;
    int iy, ip;
    if (!take_turn(invert, &iy, &ip) || !sane(cam)) return;
    if (iy) {
        ST32(cam + QC_YAW_TO, (LD32(cam + QC_YAW_TO) + (uint32_t)iy) & 0xffffu);
        ST32(cam + QC_YAW_NOW, (LD32(cam + QC_YAW_NOW) + (uint32_t)iy) & 0xffffu);
    }
    if (ip) {
        int to = (int16_t)LD32(cam + QC_PITCH_TO), next = clamp_pitch(to + ip);
        int now_p = clamp_pitch((int16_t)LD32(cam + QC_PITCH_NOW) + next - to);
        ST32(cam + QC_PITCH_TO, (uint32_t)next & 0xffffu);
        ST32(cam + QC_PITCH_NOW, (uint32_t)now_p & 0xffffu);
        if (next != to + ip) g_rest_pitch = 0;  // (at the limit)
    }
}

static float cam_param(Cpu *c, uint32_t cam, uint32_t id) {
    c->r[0] = cam, c->r[1] = id;
    guest_call(c, FN_CAM_PARAM);
    return c->v.f[0];
}

// The hunter's aim pitch moved by ip camera units; the units the camera's pitch really moved (0 at the limits).
static int aim_pitch(Cpu *c, uint32_t cam, uint32_t pl, int ip) {
    int up = (int)(cam_param(c, cam, QC_PARAM_AIM_UP) * UNITS_PER_DEG + 0.5f);
    int down = (uint16_t)(int)(cam_param(c, cam, QC_PARAM_AIM_DOWN) * UNITS_PER_DEG + 0.5f);
    int range = (uint16_t)(up + down);  // (as the aim camera's: its pitch = -down + (aim + 100) * range / 200)
    if (!range) return 0;
    float units_per_aim = (float)range / (2.0f * AIM_PITCH_MAX);
    float was = LDF(pl + PL_AIM_PITCH), aim = was + (float)ip / units_per_aim;
    if (aim > AIM_PITCH_MAX) aim = AIM_PITCH_MAX, g_rest_pitch = 0;
    if (aim < -AIM_PITCH_MAX) aim = -AIM_PITCH_MAX, g_rest_pitch = 0;
    STF(pl + PL_AIM_PITCH, aim);
    STF(pl + PL_AIM_PITCH2, aim);
    return (int)((aim - was) * units_per_aim);
}

static void aim_turn(Cpu *c) {
    uint32_t pl = c->r[0], mode = c->r[1], cam = g_cam;
    fnhook_call_original(c, FN_AIM_TURN);
    static uint32_t last_runs;
    bool stick_ran = g_stick_runs != last_runs;  // (some options keep the stick on the camera while aiming)
    last_runs = g_stick_runs;
    if (stick_ran || !sane(pl) || !sane(cam) || LD32(cam + QC_PLAYER) != pl) return;
    int iy, ip;
    if (!take_turn(LD8(LD32(G_OPTIONS) + OPT_AIM_INVERT), &iy, &ip)) return;
    if (iy) {
        ST32(pl + PL_AIM_YAW, (LD32(pl + PL_AIM_YAW) + (uint32_t)iy) & 0xffffu);
        ST32(cam + QC_EYE_YAW, (LD32(cam + QC_EYE_YAW) + (uint32_t)iy) & 0xffffu);
    }
    if (!ip) return;
    if (mode == 1) {
        int lean = (int16_t)LD16(pl + PL_AIM_LEAN) + ip;
        ST16(pl + PL_AIM_LEAN, lean < -AIM_LEAN_MAX ? -AIM_LEAN_MAX : lean > AIM_LEAN_MAX ? AIM_LEAN_MAX : lean);
    } else {
        int moved = aim_pitch(c, cam, pl, ip);
        ST32(cam + QC_EYE_PITCH, (LD32(cam + QC_EYE_PITCH) + (uint32_t)moved) & 0xffffu);
    }
}

void mousecam_init(void) {
    fnhook_set(FN_CAMERA_STICK, hle_register("mousecam:camera_stick", camera_stick));
    fnhook_set(FN_AIM_TURN, hle_register("mousecam:aim_turn", aim_turn));
}
