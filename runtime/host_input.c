// Host controls -> the Switch controller the game reads (runtime/hle/input.c).
//
// Keyboard and mouse: the keys and mouse buttons of the settings' [Keybinds] (runtime/config.c; by default WASD left
//   stick, Left Shift held: half tilt, walking; left click = X, right click = A, Space = B, R = Y, Q = L, E = R,
//   Tab = ZL, T = ZR, Enter = Plus, Backspace = Minus, arrow keys = d-pad, Ctrl = left stick click, Alt = right stick
//   click), mouse movement right stick (camera; click in the window to capture the mouse, Esc releases it). While the
//   game shows a text entry (names, chat) the keyboard types instead
//   (runtime/textinput.c). / ([Controls] ChatKey) opens a line of chat, Enter sends it, also in a quest (runtime/hle/chat.c).
//   F11 switches windowed <-> full screen, F12 saves a screenshot, Shift+F12 the next 8
//   frames (exact rendered pixels, run\shots\), F10 dumps every render pass of the next 2 frames (debug, slow), F9 toggles how colour clears are done (debug).
// Gamepad (SDL): face buttons by position as on a Switch controller (the bottom one is B), or by their labels
//   (settings FaceButtons, runtime/config.c; MHGU_PAD_POSITIONAL=0|1 overrides).
//   Every controller is open and the one a button was last pressed on is read (a Steam Deck with a controller plugged
//   in has two). SDL calls other devices a gamepad too (a Steam Controller's puck, an analog keyboard's mouse
//   interface): MHGU_PAD=<part of the name> keeps only yours, and the log lists every one it finds.
// Scripted input (tests): MHGU_INPUT="frame:BUTTON:frames,..." holds BUTTON from that presented frame for that many
//   frames. BUTTON: A B X Y L R ZL ZR PLUS MINUS LEFT UP RIGHT DOWN LSTICK RSTICK, or STICK_UP/DOWN/LEFT/RIGHT and
//   RSTICK_UP/DOWN/LEFT/RIGHT (full tilt).
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL3/SDL.h>

#include "config.h"
#include "menu_confirm.h"
#include "forge.h"
#include "hle/chat.h"
#include "hle/input.h"
#include "overlay.h"
#include "rt.h"
#include "textinput.h"

#define B(x) (1ull << (x))

static bool keys[SDL_SCANCODE_COUNT];
static SDL_Gamepad *pad;  // the controller read: of those open, the one a button was last pressed on
#define MAX_PADS 8
static struct { SDL_Gamepad *pad; uint64_t opened_ns; } pads[MAX_PADS];
static int npads;

// [Keybinds] (config.h BIND_*): the Switch button each presses (-1: the left stick's directions, walking)
static const int bind_buttons[BINDS] = {
    [BIND_MOVE_UP] = -1, [BIND_MOVE_DOWN] = -1, [BIND_MOVE_LEFT] = -1, [BIND_MOVE_RIGHT] = -1, [BIND_WALK] = -1,
    [BIND_A] = BTN_A, [BIND_B] = BTN_B, [BIND_X] = BTN_X, [BIND_Y] = BTN_Y, [BIND_L] = BTN_L, [BIND_R] = BTN_R,
    [BIND_ZL] = BTN_ZL, [BIND_ZR] = BTN_ZR, [BIND_PLUS] = BTN_PLUS, [BIND_MINUS] = BTN_MINUS, [BIND_DPAD_UP] = BTN_UP,
    [BIND_DPAD_DOWN] = BTN_DOWN, [BIND_DPAD_LEFT] = BTN_LEFT, [BIND_DPAD_RIGHT] = BTN_RIGHT, [BIND_LSTICK] = BTN_LSTICK,
    [BIND_RSTICK] = BTN_RSTICK,
};
static uint32_t mouse_held;  // the mouse buttons held (bit: SDL_BUTTON_*), while captured
static bool bind_held(int bind) {
    for (int k = 0; k < BIND_KEYS; k++) {
        int code = config_live_bind(bind, k);
        if (code > 0 && code < SDL_SCANCODE_COUNT && keys[code]) return true;
        if (code > KEY_MOUSE && code <= KEY_MOUSE + SDL_BUTTON_X2 && (mouse_held >> (code - KEY_MOUSE) & 1)) return true;
    }
    return false;
}

// ---- mouse: captured (relative mode) after a click in the window, Esc lets it go. Its movement is the right stick:
// the speed it moves at, smoothed over a few milliseconds, as the stick's tilt (full tilt at MOUSE_FULL_SPEED counts a
// second, times the settings' MouseSensitivity). Its buttons press what [Keybinds] says.
#define MOUSE_FULL_SPEED 1500.0f
#define MOUSE_SMOOTH_S 0.025f  // (the averaging time: mice report at 125-8000 Hz, the game reads at its frame rate)
// The camera ignores the right stick below about 39% tilt (any direction: a round dead zone; measured 2026-10-23, 38%
// still, 40% turning slowly, then faster up to full tilt): the mouse's tilt starts there, so slow movements turn too.
#define MOUSE_DEADZONE 0.40f
static bool mouse_grab;       // the player's choice: clicked in (true) or Esc / focus lost (false)
static bool mouse_captured;   // relative mode on now (grabbed, and no Forge menu open)
static bool mouse_no_capture; // relative mode refused (WSLg, some Wayland compositors): clicks are buttons, no mouse look
static float mouse_dx, mouse_dy;  // counts moved since the last update
// (tests: the control file's "mousemove <x> <y> <ms>", a mouse moving at that many counts a second, captured or not)
static float test_vx, test_vy;
static uint64_t test_until;

// ---- scripted input
typedef struct { uint64_t start, len; uint64_t buttons; int stick, x, y; } ScriptStep;
#define MAX_STEPS 4096
static ScriptStep script[MAX_STEPS];
static int nscript = -1;

static const char *const button_names[] = {"A", "B", "X", "Y", "LSTICK", "RSTICK", "L", "R", "ZL", "ZR", "PLUS",
                                           "MINUS", "LEFT", "UP", "RIGHT", "DOWN"};

// One step: BUTTON (or STICK_<dir> / RSTICK_<dir>) held from presented frame `start` for `len` frames.
static bool add_step(const char *name, uint64_t start, uint64_t len) {
    ScriptStep st = {start, len, 0, -1, 0, 0};
    const char *dir = NULL;
    if (!strncmp(name, "STICK_", 6)) st.stick = 0, dir = name + 6;
    else if (!strncmp(name, "RSTICK_", 7)) st.stick = 1, dir = name + 7;
    if (dir) {
        // UP, DOWN, LEFT, RIGHT or a diagonal (UPLEFT, DOWNRIGHT...)
        int dx = strstr(dir, "LEFT") ? -1 : strstr(dir, "RIGHT") ? 1 : 0, dy = strstr(dir, "DOWN") ? -1 : strstr(dir, "UP") ? 1 : 0;
        int m = dx && dy ? 23170 : 32767;  // (diagonals: full deflection, not beyond the circle)
        st.x = dx * m, st.y = dy * m;
    } else {
        int b = -1;
        for (int i = 0; i < 16; i++) if (!strcmp(name, button_names[i])) b = i;
        if (b < 0) return false;
        st.buttons = B(b);
    }
    if (nscript == MAX_STEPS) {  // (control file sessions: drop the steps that are over)
        int k = 0;
        for (int i = 0; i < nscript; i++) if (script[i].start + script[i].len > g_present_count) script[k++] = script[i];
        nscript = k;
        if (nscript == MAX_STEPS) return false;
    }
    script[nscript++] = st;
    return true;
}

// Press BUTTON (as in MHGU_INPUT) from the next presented frame for `frames` frames. UI thread only (with
// host_input_update), like the control file.
bool host_input_press(const char *button, unsigned frames) {
    return add_step(button, g_present_count + 1, frames);
}

static void script_parse(void) {
    nscript = 0;
    const char *s = getenv("MHGU_INPUT");
    if (!s) return;
    char *copy = strdup(s), *save = NULL;
    for (char *tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        char name[32];
        unsigned long long start, len;
        if (sscanf(tok, "%llu:%31[^:]:%llu", &start, name, &len) != 3 || !add_step(name, start, len))
            LOG("[input] bad MHGU_INPUT step '%s'\n", tok);
    }
    free(copy);
    LOG("[input] scripted input: %d steps\n", nscript);
}

// MHGU_CONTROL=<file>: commands appended to the file while the game runs (one per line), for driving it from outside:
//   <BUTTON> [frames] [delay]  press now, or that many presented frames from now (default 8 frames); STICK_UP 30 etc.
//                          as in MHGU_INPUT. Lines written together start from the same frame: a timed sequence.
//   shot <name>            save the next presented frame as shots/<name>.png
//   burst <n> <name>       save the next n presented frames as shots/<name>_<i>.png
//   dump [n]               save colour target 0 (or the depth of a depth-only pass) of every render pass of the next
//                          frame (shots/pass_*.png), or of the next n frames (shots/f<i>_pass_*.png for i >= 1)
//   hudtrace               log every HUD quad of the next frame: its layer, place, and where 16:10 / ultrawide moved it
//   frame                  log the current frame number
//   type <text>            type into the game's text entry (runtime/textinput.c); textkey enter|escape|backspace
//   mousemove <x> <y> <ms> mouse look as if the mouse moved at that many counts a second (y down) for that long
char g_shot_request[256];  // (runtime/nvn/vulkan/device.c saves it)
char g_burst_name[200];
int g_burst_left, g_burst_index;
volatile int g_dump_request;
volatile int g_hudtrace_request;  // (debug) "hudtrace": nvn/vulkan/exec.c logs the next frame's HUD quads
static void control_poll(void) {
    static const char *path = (const char *)1;
    static long done;
    static uint64_t last_ns;
    if (path == (const char *)1) path = getenv("MHGU_CONTROL");
    if (!path) return;
    uint64_t now = plat_now_ns();
    if (now - last_ns < 50000000ull) return;
    last_ns = now;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size < done) done = 0;  // (file replaced)
    if (size > done) {
        fseek(f, done, SEEK_SET);
        char line[300];
        while (ftell(f) < size && fgets(line, sizeof line, f)) {
            if (!strchr(line, '\n') && ftell(f) >= size) break;  // (partial last line: next time)
            done = ftell(f);
            char cmd[32] = "", arg[256] = "", arg2[200] = "";
            if (sscanf(line, "%31s %255s %199s", cmd, arg, arg2) < 1) continue;
            if (!strcmp(cmd, "msaa")) { void vk_set_msaa(uint32_t); vk_set_msaa((uint32_t)atoi(arg)); continue; }
            if (!strcmp(cmd, "gpuprof")) { extern volatile int g_gpuprof_request; g_gpuprof_request = 1; continue; }
            if (!strcmp(cmd, "hudtrace")) { g_hudtrace_request = 1; continue; }
            if (!strcmp(cmd, "dump")) { g_dump_request = atoi(arg) > 0 ? atoi(arg) : 1; continue; }
            if (!strcmp(cmd, "memsave")) {  // memsave <guest hex address> <hex bytes> <file>: raw guest memory to a file
                char path[200] = "";
                sscanf(line, "%*s %*s %*s %199s", path);
                uint32_t a = (uint32_t)strtoul(arg, NULL, 16), n = (uint32_t)strtoul(arg2, NULL, 16);
                FILE *o = path[0] && n <= (16u << 20) ? fopen(path, "wb") : NULL;
                if (o) { fwrite(G2H(a), 1, n, o); fclose(o); }
                LOG("[memsave] %08x+%x -> %s%s\n", a, n, path, o ? "" : " (failed)");
                continue;
            }
            if (!strcmp(cmd, "testhook")) {  // testhook <hex address>: a Forge plugin's kind of hook there (forge.c)
                forge_test_hook((uint32_t)strtoul(arg, NULL, 16));
                continue;
            }
            if (!strcmp(cmd, "interpwatch")) {  // interpwatch <guest hex addr> <file of hex entries>: run those game
                // functions (and all game code they call) in the interpreter, logging what changes the word at addr
                void interp_watch(uint32_t, int);
                void interp_add_range(uint32_t, uint32_t);
                int dispatch_interpret(uint32_t, uint32_t);
                FILE *fl = fopen(arg2, "r");
                char fn[32];
                int n = 0;
                interp_add_range(0x200000, 0x1800000);
                while (fl && fgets(fn, sizeof fn, fl)) {
                    uint32_t e = (uint32_t)strtoul(fn, NULL, 16);
                    if (e && dispatch_interpret(e, e + 4) > 0) n++;
                }
                if (fl) fclose(fl);
                interp_watch((uint32_t)strtoul(arg, NULL, 16), 40);
                LOG("[watch] %s: %d functions interpreted\n", arg, n);
                continue;
            }
            if (!strcmp(cmd, "interpread")) {  // interpread <lo hex>:<hi hex> <file of hex entries>: as interpwatch, but
                // logging the instructions that read the range (once each)
                void interp_read_watch(uint32_t, uint32_t);
                void interp_add_range(uint32_t, uint32_t);
                int dispatch_interpret(uint32_t, uint32_t);
                FILE *fl = fopen(arg2, "r");
                char fn[32];
                int n = 0;
                interp_add_range(0x200000, 0x1800000);
                while (fl && fgets(fn, sizeof fn, fl)) {
                    uint32_t e = (uint32_t)strtoul(fn, NULL, 16);
                    int r = e ? dispatch_interpret(e, e + 4) : -2;
                    if (r > 0) n++;
                    else {
                        extern GuestFn dispatch_lookup(uint32_t);
                        LOG("[readwatch] %x: not interpreted (%d, lookup %p)\n", e, r, (void *)dispatch_lookup(e));
                    }
                }
                if (!fl) LOG("[readwatch] can't open %s\n", arg2);
                if (fl) fclose(fl);
                const char *colon = strchr(arg, ':');
                interp_read_watch((uint32_t)strtoul(arg, NULL, 16), colon ? (uint32_t)strtoul(colon + 1, NULL, 16) : 0);
                LOG("[readwatch] %s: %d functions interpreted\n", arg, n);
                continue;
            }
            if (!strcmp(cmd, "emact")) {  // emact <group> <index> [mode]: start a monster action (hle/emtrace.c)
                void emtrace_request(uint32_t, uint32_t, uint32_t);
                char arg3[16] = "";
                sscanf(line, "%*s %*s %*s %15s", arg3);
                emtrace_request((uint32_t)atoi(arg), (uint32_t)atoi(arg2), (uint32_t)atoi(arg3));
                continue;
            }
            if (!strcmp(cmd, "emsweep")) {  // emsweep <group> <first> <last>: each monster action in turn (emtrace.c)
                void emtrace_sweep(uint32_t, uint32_t, uint32_t);
                char arg3[16] = "";
                sscanf(line, "%*s %*s %*s %15s", arg3);
                emtrace_sweep((uint32_t)atoi(arg), (uint32_t)atoi(arg2), (uint32_t)atoi(arg3));
                continue;
            }
            if (!strcmp(cmd, "poke")) {  // poke <guest hex address> <hex word>: write guest memory (reverse engineering)
                uint32_t a = (uint32_t)strtoul(arg, NULL, 16) & ~3u, v = (uint32_t)strtoul(arg2, NULL, 16);
                LOG("[poke] %08x: %08x -> %08x\n", a, LD32(a), v);
                ST32(a, v);
                continue;
            }
            if (!strcmp(cmd, "fntrace")) {  // fntrace <lo hex> <hi hex> [n] [r0 lo hex] [r0 hi hex]: log the next n calls of
                                            // guest functions there (only those with r0 in [r0 lo, r0 hi) when given)
                void fntrace_set(uint32_t, uint32_t, int, uint32_t, uint32_t);
                char arg3[16] = "", arg4[16] = "", arg5[16] = "";
                sscanf(line, "%*s %*s %*s %15s %15s %15s", arg3, arg4, arg5);
                fntrace_set((uint32_t)strtoul(arg, NULL, 16), (uint32_t)strtoul(arg2, NULL, 16), arg3[0] ? atoi(arg3) : 200,
                            (uint32_t)strtoul(arg4, NULL, 16), (uint32_t)strtoul(arg5, NULL, 16));
                continue;
            }
            if (!strcmp(cmd, "fnnew")) {  // fnnew <lo hex> <hi hex> [learn ms]: log first calls only (dispatch.c)
                void fnnew_set(uint32_t, uint32_t, int);
                char arg3[16] = "";
                sscanf(line, "%*s %*s %*s %15s", arg3);
                fnnew_set((uint32_t)strtoul(arg, NULL, 16), (uint32_t)strtoul(arg2, NULL, 16), atoi(arg3));
                continue;
            }
            if (!strcmp(cmd, "find")) {  // find <hex bytes>: log where they are in committed guest memory (first 32)
                uint8_t pat[100];
                int np = 0;
                for (const char *q = arg; q[0] && q[1] && np < 100; q += 2) {
                    char hx[3] = {q[0], q[1], 0};
                    pat[np++] = (uint8_t)strtoul(hx, NULL, 16);
                }
                int hits = 0;
                for (uint64_t a = 0x1000; np && a < GUEST_SPACE && hits < 32;) {
                    uint64_t end;
                    bool ok = plat_committed(GUEST_BASE + a, &end);
                    end -= GUEST_BASE;
                    if (end > GUEST_SPACE) end = GUEST_SPACE;
                    if (ok && end - a >= (uint64_t)np) {
                        const uint8_t *m = G2H((uint32_t)a);
                        for (uint64_t i = 0; i + np <= end - a && hits < 32; i++)
                            if (m[i] == pat[0] && !memcmp(m + i, pat, np)) LOG("[find] %08llx\n", (unsigned long long)(a + i)), hits++;
                    }
                    a = end;
                }
                LOG("[find] %d hit(s)\n", hits);
                continue;
            }
            if (!strcmp(cmd, "peek")) {  // peek <guest hex address> [words]: log guest memory (reverse engineering)
                uint32_t a = (uint32_t)strtoul(arg, NULL, 16) & ~3u;
                int nw = arg2[0] ? atoi(arg2) : 8;
                for (int k = 0; k < nw && k < 256; k += 4) {
                    char line[96];
                    int len = snprintf(line, sizeof line, "[peek] %08x:", a + 4 * k);
                    for (int j = k; j < nw && j < k + 4; j++) len += snprintf(line + len, sizeof line - len, " %08x", LD32(a + 4 * j));
                    LOG("%s\n", line);
                }
                continue;
            }
            if (!strcmp(cmd, "burst")) {
                snprintf(g_burst_name, sizeof g_burst_name, "%s", arg2[0] ? arg2 : "burst");
                g_burst_index = 0;
                g_burst_left = atoi(arg) > 0 ? atoi(arg) : 4;
                continue;
            }
            if (!strcmp(cmd, "type")) {
                char *t = line + strspn(line, " \t") + 4;
                t += strspn(t, " \t");
                t[strcspn(t, "\r\n")] = 0;
                textinput_type(t);
                continue;
            }
            if (!strcmp(cmd, "sdlquit")) { // Same path as the window close button / Alt+F4 (runtime tests).
                SDL_Event event = {0}; event.type = SDL_EVENT_QUIT; SDL_PushEvent(&event);
                continue;
            }
            if (!strcmp(cmd, "sdlkey")) {  // sdlkey [ctrl+|alt+]<SDL key name>: a key pressed and released (Insert: Forge's
                                           // menu; alt+Return: full screen <-> window)
                bool ctrl = !SDL_strncasecmp(arg, "ctrl+", 5), alt = !SDL_strncasecmp(arg, "alt+", 4);
                SDL_Scancode sc = SDL_GetScancodeFromName(arg + (ctrl ? 5 : alt ? 4 : 0));
                SDL_Event e = {0};
                e.key.type = SDL_EVENT_KEY_DOWN, e.key.scancode = sc, e.key.key = SDL_GetKeyFromScancode(sc, 0, false);
                e.key.down = true, e.key.mod = ctrl ? SDL_KMOD_LCTRL : alt ? SDL_KMOD_LALT : 0;
                SDL_PushEvent(&e);
                e.key.type = SDL_EVENT_KEY_UP, e.key.down = false;
                SDL_PushEvent(&e);
                if (ctrl || alt) {  // (then the modifier's own release, as a keyboard sends it: no modifier left)
                    e.key.scancode = ctrl ? SDL_SCANCODE_LCTRL : SDL_SCANCODE_LALT, e.key.key = ctrl ? SDLK_LCTRL : SDLK_LALT;
                    e.key.mod = 0;
                    SDL_PushEvent(&e);
                }
                LOG("[control] key %s (%d)\n", arg, (int)sc);
                continue;
            }
            if (!strcmp(cmd, "vpad")) {  // vpad: attach an SDL virtual controller (tests: picking the controller)
                static SDL_Joystick *vpads[4];
                static int nv;
                if (!strcmp(arg, "") && nv < 4) {
                    SDL_VirtualJoystickDesc d;
                    SDL_INIT_INTERFACE(&d);
                    d.type = SDL_JOYSTICK_TYPE_GAMEPAD, d.naxes = SDL_GAMEPAD_AXIS_COUNT, d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
                    d.name = nv ? "Virtual pad 2" : "Virtual pad 1";
                    d.vendor_id = 0x1234, d.product_id = (Uint16)(nv + 1);  // (SDL names it by its mapping: one per id)
                    SDL_JoystickID id = SDL_AttachVirtualJoystick(&d);
                    vpads[nv] = id ? SDL_OpenJoystick(id) : NULL;
                    LOG("[control] vpad %d: %s\n", nv, vpads[nv] ? "attached" : SDL_GetError());
                    nv++;
                } else {  // vpad <n> <SDL button number> <0|1>: that button of virtual controller n up or down
                    int n = atoi(arg), down = 0, btn = atoi(arg2);
                    sscanf(line, "%*s %*s %*s %d", &down);
                    if (n >= 0 && n < nv && vpads[n]) SDL_SetJoystickVirtualButton(vpads[n], btn, down != 0);
                }
                continue;
            }
            if (!strcmp(cmd, "sdltext")) {  // sdltext <text>: typed text, as the keyboard sends it (not the game's entry)
                static char text[256];
                snprintf(text, sizeof text, "%s", arg);
                SDL_Event e = {0};
                e.text.type = SDL_EVENT_TEXT_INPUT, e.text.text = text;
                SDL_PushEvent(&e);
                continue;
            }
            if (!strcmp(cmd, "mouse")) {  // mouse <x> <y> [click]: the pointer to (x, y) in the window, clicked there
                char arg3[16] = "";
                sscanf(line, "%*s %*s %*s %15s", arg3);
                SDL_Event e = {0};
                e.motion.type = SDL_EVENT_MOUSE_MOTION, e.motion.x = (float)atof(arg), e.motion.y = (float)atof(arg2);
                SDL_PushEvent(&e);
                if (!strcmp(arg3, "click")) {
                    SDL_Event b = {0};
                    b.button.type = SDL_EVENT_MOUSE_BUTTON_DOWN, b.button.button = SDL_BUTTON_LEFT, b.button.down = true;
                    b.button.x = e.motion.x, b.button.y = e.motion.y, b.button.clicks = 1;
                    SDL_PushEvent(&b);
                    b.button.type = SDL_EVENT_MOUSE_BUTTON_UP, b.button.down = false;
                    SDL_PushEvent(&b);
                }
                continue;
            }
            if (!strcmp(cmd, "mousemove")) {  // mousemove <x> <y> <ms>: mouse look at that speed (counts a second)
                char arg3[16] = "";
                sscanf(line, "%*s %*s %*s %15s", arg3);
                test_vx = (float)atof(arg), test_vy = (float)atof(arg2);
                test_until = plat_now_ns() + (uint64_t)atoi(arg3) * 1000000ull;
                continue;
            }
            if (!strcmp(cmd, "set") || !strcmp(cmd, "unset")) {  // set <Key> <Value...> / unset <Quest<n>>: as the
                                                                   // Settings window does (config_set / config_remove)
                const char *v = strstr(line + strlen(cmd), arg);
                char value[256] = "";
                if (v) snprintf(value, sizeof value, "%s", v + strlen(arg) + strspn(v + strlen(arg), " \t"));
                value[strcspn(value, "\r\n")] = 0;
                if (!strcmp(cmd, "unset")) config_remove(arg);
                else if (config_set(arg, value) && config_live(arg)) ui_setting_changed(arg);
                else if (!config_check(arg, value)) LOG("[control] set %s: \"%s\" refused\n", arg, value);
                continue;
            }
#ifdef __APPLE__
            if (!strcmp(cmd, "settingsshot")) {  // settingsshot <folder>: the Settings window's tabs as PNGs (macOS)
                void plat_settings_snapshot(const char *dir);
                plat_settings_snapshot(arg[0] ? arg : ".");
                continue;
            }
#endif
            if (!strcmp(cmd, "textkey")) {
                textinput_key(!strcmp(arg, "enter") ? TI_ENTER : !strcmp(arg, "escape") ? TI_CANCEL : TI_NONE);
                continue;
            }
            if (!strcmp(cmd, "shot")) {
                snprintf(g_shot_request, sizeof g_shot_request, "shots/%s.png", arg[0] ? arg : "control");
            } else if (!strcmp(cmd, "frame")) {
                LOG("[control] frame %llu\n", (unsigned long long)g_present_count);
            } else {
                unsigned long n = arg[0] ? strtoul(arg, NULL, 10) : 8, delay = strtoul(arg2, NULL, 10);
                uint64_t at = g_present_count + 1 + delay;
                if (!add_step(cmd, at, n ? n : 8)) LOG("[control] unknown command '%s'\n", cmd);
                else LOG("[control] %s %lu at frame %llu\n", cmd, n ? n : 8, (unsigned long long)at);
            }
        }
    }
    fclose(f);
}
// ---- events
// Which controller the game reads. Every one is open, and a button pressed on one makes it the one read: the first
// one SDL listed was not always the one in the player's hands (a Steam Deck's own controls and a controller plugged
// into it, or Steam Input's virtual controller beside the real one). SDL calls other devices a gamepad as well (a
// Steam Controller's puck, an analog keyboard's mouse interface), which would read as stuck buttons and sticks: so
// every device and every change is logged, and MHGU_PAD=<part of the name, case-insensitive> says which one to use.
static bool pad_matches(SDL_JoystickID id) {
    const char *sel = getenv("MHGU_PAD");
    if (!sel || !*sel) return true;
    unsigned vid = 0, pid = 0;  // "28de:1142" matches the ids, anything else part of the name
    if (sscanf(sel, "%4x:%4x", &vid, &pid) == 2)
        return SDL_GetGamepadVendorForID(id) == vid && SDL_GetGamepadProductForID(id) == pid;
    const char *name = SDL_GetGamepadNameForID(id);
    return name && SDL_strcasestr(name, sel);
}

static void log_pad(SDL_JoystickID id, const char *tag) {
    const char *name = SDL_GetGamepadNameForID(id);
    LOG("[input] %s: %s (%04x:%04x), SDL mapping %s\n", tag, name ? name : "?", SDL_GetGamepadVendorForID(id),
        SDL_GetGamepadProductForID(id), SDL_GetGamepadMappingForID(id) ? "known" : "NONE (SDL guesses the buttons)");
}

static int find_pad(SDL_JoystickID id) {
    for (int i = 0; i < npads; i++)
        if (SDL_GetGamepadID(pads[i].pad) == id) return i;
    return -1;
}

static void open_pad(SDL_JoystickID id) {
    if (find_pad(id) >= 0) return;
    if (!pad_matches(id)) { log_pad(id, "gamepad skipped (MHGU_PAD)"); return; }
    if (npads == MAX_PADS) { log_pad(id, "gamepad skipped (too many)"); return; }
    SDL_Gamepad *p = SDL_OpenGamepad(id);
    if (!p) { LOG("[input] gamepad %u failed to open: %s\n", (unsigned)id, SDL_GetError()); return; }
    pads[npads].pad = p, pads[npads++].opened_ns = plat_now_ns();
    if (!pad) pad = p, log_pad(id, "gamepad in use");
    else log_pad(id, "gamepad open too (a button pressed on it makes it the one in use)");
}

// A button pressed on another open controller: that one is read from now on. Not in the first half second after it
// opened (a device whose buttons read as held sends their presses then, and would take over at startup).
static void pad_pressed(SDL_JoystickID id) {
    int i = find_pad(id);
    if (i < 0 || pads[i].pad == pad || plat_now_ns() - pads[i].opened_ns < 500000000ull) return;
    pad = pads[i].pad;
    log_pad(id, "gamepad in use (a button pressed on it)");
}

static void close_pad(SDL_JoystickID id) {
    int i = find_pad(id);
    if (i < 0) return;
    bool was = pads[i].pad == pad;
    const char *name = SDL_GetGamepadName(pads[i].pad);  // (its id is gone already: log_pad would find nothing)
    LOG("[input] gamepad removed%s: %s\n", was ? " (it was in use)" : "", name ? name : "?");
    SDL_CloseGamepad(pads[i].pad);
    pads[i] = pads[--npads];
    if (was) {
        pad = npads ? pads[0].pad : NULL;
        if (pad) log_pad(SDL_GetGamepadID(pad), "gamepad in use");
    }
}

static void pick_pad(void) {  // over every controller SDL sees now: those MHGU_PAD allows (all without it), first in use
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int i = 0; i < n; i++) open_pad(ids[i]);
    SDL_free(ids);
}

// While a line of chat is typed the pointer shows, and dragging the box moves it (kept in chat_box.txt)
static bool chat_mouse(const SDL_Event *e) {
    static bool dragging;
    static float last_x, last_y;
    if (!chat_active()) {
        dragging = false;
        return false;
    }
    int w = 0, h = 0;
    SDL_Window *win = ui_window();
    if (win) SDL_GetWindowSize(win, &w, &h);
    if (w <= 0 || h <= 0) return false;
    switch (e->type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (e->button.button == SDL_BUTTON_LEFT && chat_in_box(e->button.x / w, e->button.y / h))
            dragging = true, last_x = e->button.x, last_y = e->button.y;
        return true;
    case SDL_EVENT_MOUSE_MOTION:
        if (dragging) {
            chat_move((e->motion.x - last_x) / w, (e->motion.y - last_y) / h);
            last_x = e->motion.x, last_y = e->motion.y;
        }
        return true;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (dragging && e->button.button == SDL_BUTTON_LEFT) {
            dragging = false;
            chat_save_position();
        }
        return true;
    case SDL_EVENT_MOUSE_WHEEL:
        return true;
    }
    return false;
}

void host_input_event(const SDL_Event *e) {
    if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) pad_pressed(e->gbutton.which);
    if (overlay_event(e)) return;  // (Forge's menu open: keyboard and mouse are its)
    if (chat_mouse(e)) return;  // (typing a line of chat: the mouse drags its box)
    if (textinput_event(e)) return;  // (typing into a text entry: not controller buttons)
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP:
        if (e->key.scancode < SDL_SCANCODE_COUNT) keys[e->key.scancode] = e->type == SDL_EVENT_KEY_DOWN;
        if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat) {  // F12: screenshot, Shift+F12: 8 consecutive frames (run\shots\), F11: windowed <-> full screen
            static int nshot;
            uint64_t t = (uint64_t)time(NULL);
            if ((e->key.scancode == SDL_SCANCODE_RETURN || e->key.scancode == SDL_SCANCODE_KP_ENTER) && (e->key.mod & SDL_KMOD_ALT)) {
                keys[e->key.scancode] = false;  // (Alt+Enter: windowed <-> full screen, not a button press)
                keys[SDL_SCANCODE_LALT] = keys[SDL_SCANCODE_RALT] = false;
                ui_toggle_fullscreen();
            } else if ((int)e->key.scancode == config_live_chat_key() && config_live_chat_key()) {
                keys[e->key.scancode] = false;
                chat_open();  // (a line to type, sent as chat: runtime/hle/chat.c)
            } else if (e->key.scancode == SDL_SCANCODE_ESCAPE) {
                if (mouse_grab) LOG("[input] mouse released\n");
                mouse_grab = false;
            } else if (e->key.scancode == SDL_SCANCODE_F12 && !(e->key.mod & SDL_KMOD_SHIFT))
                snprintf(g_shot_request, sizeof g_shot_request, "shots/shot_%llu_%d.png", (unsigned long long)t, nshot++);
            else if (e->key.scancode == SDL_SCANCODE_F11) {
                ui_toggle_fullscreen();
            } else if (e->key.scancode == SDL_SCANCODE_F12) {
                snprintf(g_burst_name, sizeof g_burst_name, "burst_%llu_%d", (unsigned long long)t, nshot++);
                g_burst_index = 0;
                g_burst_left = 8;
            } else if (e->key.scancode == SDL_SCANCODE_F10) {
                g_dump_request = 2;  // (debug) every render pass of the next 2 frames, as "dump 2"
            } else if (e->key.scancode == SDL_SCANCODE_F7) {  // MSAA off / 2x / 4x / 8x (to compare; the settings set it)
                void vk_cycle_msaa(void);
                vk_cycle_msaa();
            } else if (e->key.scancode == SDL_SCANCODE_F8) {  // (debug) shadow-pass scissors: as set / none / mirrored
                extern volatile int g_shadow_scissor_mode;
                g_shadow_scissor_mode = (g_shadow_scissor_mode + 1) % 3;
                LOG("[input] F8: shadow scissor mode %d\n", g_shadow_scissor_mode);
            } else if (e->key.scancode == SDL_SCANCODE_F9) {  // (debug) colour clears: vkCmdClearAttachments <-> draws
                extern volatile int g_clear_draw;
                g_clear_draw = g_clear_draw > 0 ? 0 : 1;
                LOG("[input] F9: colour clears as %s\n", g_clear_draw ? "draws" : "vkCmdClearAttachments");
            }
        }
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (mouse_captured) mouse_dx += e->motion.xrel, mouse_dy += e->motion.yrel;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: {
        int btn = e->button.button;
        if (btn < 1 || btn > SDL_BUTTON_X2) break;
        if (!e->button.down) {
            mouse_held &= ~(1u << btn);
        } else if (!mouse_captured && !mouse_no_capture) {  // (the click that captures the mouse is not a button press)
            if (!mouse_grab) LOG("[input] capturing the mouse\n");
            mouse_grab = true;
        } else {
            mouse_held |= 1u << btn;
        }
        break;
    }
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        memset(keys, 0, sizeof keys);
        mouse_held = 0, mouse_grab = false;
        config_window_focus(false);  // ([Audio] MuteInBackground)
        if (config_editable().mute_in_background) LOG("[input] window in the background: sound muted\n");
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        config_window_focus(true);
        if (config_editable().mute_in_background) LOG("[input] window focused: sound back\n");
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
        open_pad(e->gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        close_pad(e->gdevice.which);
        break;
    }
}

static int32_t clamp_axis(int v) { return v < -32767 ? -32767 : v > 32767 ? 32767 : v; }

// Capture or release the mouse as wanted, and add its movement to the right stick.
static void mouse_update(int *rx, int *ry) {
    bool want = mouse_grab && !overlay_menu_open() && !chat_active();  // (Forge's menu, the chat box: the pointer is theirs)
    SDL_Window *w = ui_window();
    if (want != mouse_captured && w) {
        if (SDL_SetWindowRelativeMouseMode(w, want) || !want) {
            mouse_captured = want;
        } else {
            LOG("[input] mouse capture failed: %s (no mouse look; clicks are buttons)\n", SDL_GetError());
            mouse_grab = false, mouse_no_capture = true;
        }
        mouse_dx = mouse_dy = 0;
    }
    static bool chat_pointer;  // (full screen hides the pointer: shown while the chat box can be dragged)
    if (chat_active() != chat_pointer && w) {
        chat_pointer = chat_active();
        if (chat_pointer) SDL_ShowCursor();
        else if (SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN) SDL_HideCursor();
    }
    static uint64_t last;
    static float vx, vy;  // the speed, smoothed: counts a second
    uint64_t now = plat_now_ns();
    float dt = last ? (float)(now - last) * 1e-9f : 0.0f;
    last = now;
    if (dt > 0.1f) dt = 0.1f;
    if (now < test_until) mouse_dx += test_vx * dt, mouse_dy += test_vy * dt;
    if (dt > 0.0f) {
        float a = 1.0f - expf(-dt / MOUSE_SMOOTH_S);
        vx += (mouse_dx / dt - vx) * a, vy += (mouse_dy / dt - vy) * a;
    }
    mouse_dx = mouse_dy = 0;
    if (!mouse_captured && now >= test_until) vx = vy = 0;
    float s = config_live_mouse_sensitivity() / MOUSE_FULL_SPEED;
    float x = vx * s, y = -vy * s;  // (the mouse's y is down, the stick's up)
    float m = sqrtf(x * x + y * y);
    if (m < 0.02f) return;
    float t = (MOUSE_DEADZONE + (1.0f - MOUSE_DEADZONE) * (m > 1.0f ? 1.0f : m)) / m;
    *rx += (int)(x * t * 32767.0f), *ry += (int)(y * t * 32767.0f);
}

// The game's rumble (input_rumble) to the controller: on a change, at most every 16 ms (the game sends a value every
// 5 ms, and a DualSense over Bluetooth takes a packet per call), renewed while it lasts, stopped at once.
static void rumble_update(void) {
    static uint32_t sent, peak;
    static uint64_t sent_ns, on_ns;
    static SDL_Gamepad *sent_pad;
    uint32_t v = input_rumble();
    uint64_t now = plat_now_ns();
    if (v && !on_ns) on_ns = now, peak = 0;  // (the log: each rumble's length and strongest motor values)
    if (v) peak = ((v >> 16) > (peak >> 16) ? v & 0xFFFF0000u : peak & 0xFFFF0000u) | ((v & 0xFFFF) > (peak & 0xFFFF) ? v & 0xFFFF : peak & 0xFFFF);
    if (!v && on_ns) {
        LOG("[input] rumble %.0f ms, low %u high %u%s\n", (now - on_ns) / 1e6, peak >> 16, peak & 0xFFFF, pad ? "" : " (no controller)");
        on_ns = 0;
    }
    if (!pad) { sent_pad = NULL; return; }
    if (pad != sent_pad) sent = 0, sent_pad = pad;
    bool change = v != sent && (v == 0 || now - sent_ns >= 16000000ull), renew = v && now - sent_ns >= 100000000ull;
    if (!change && !renew) return;
    SDL_RumbleGamepad(pad, (Uint16)(v >> 16), (Uint16)v, v ? 200 : 0);
    sent = v, sent_ns = now;
}

void host_input_update(void) {
    if (nscript < 0) script_parse();
    rumble_update();
    control_poll();
    quickhunt_poll();  // (debug, MHGU_QUICKHUNT: runtime/hle/quickhunt.c)
    textinput_update();
    uint64_t b = 0;
    int lx = 0, ly = 0, rx = 0, ry = 0;
    static bool typing;
    if (textinput_active() != typing) {
        typing = !typing;
        memset(keys, 0, sizeof keys);  // (keys held when the entry opened are typing now, or released)
    }

    for (int i = 0; i < BINDS; i++)
        if (bind_buttons[i] >= 0 && bind_held(i)) b |= B(bind_buttons[i]);
    int kx = bind_held(BIND_MOVE_RIGHT) - bind_held(BIND_MOVE_LEFT), ky = bind_held(BIND_MOVE_UP) - bind_held(BIND_MOVE_DOWN);
    int k = bind_held(BIND_WALK) ? 16000 : 32767;
    if (kx && ky) k = k * 7071 / 10000;  // diagonals: same length as straight
    lx += kx * k, ly += ky * k;
    mouse_update(&rx, &ry);

    if (pad) {
        static const struct { SDL_GamepadButton sdl; int btn; } pad_buttons[] = {
            {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, BTN_L}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, BTN_R},
            {SDL_GAMEPAD_BUTTON_START, BTN_PLUS}, {SDL_GAMEPAD_BUTTON_BACK, BTN_MINUS},
            {SDL_GAMEPAD_BUTTON_DPAD_UP, BTN_UP}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, BTN_DOWN},
            {SDL_GAMEPAD_BUTTON_DPAD_LEFT, BTN_LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, BTN_RIGHT},
            {SDL_GAMEPAD_BUTTON_LEFT_STICK, BTN_LSTICK}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, BTN_RSTICK},
        };
        for (unsigned i = 0; i < sizeof pad_buttons / sizeof *pad_buttons; i++)
            if (SDL_GetGamepadButton(pad, pad_buttons[i].sdl)) b |= B(pad_buttons[i].btn);
        // face buttons: SDL names them by position (south, east, west, north). Settings FaceButtons: Position = the
        // Switch layout; Label = the letter on the button (PlayStation symbols as an Xbox pad's letters at that place)
        static const SDL_GamepadButton face[4] = {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST,
                                                  SDL_GAMEPAD_BUTTON_NORTH};
        static const int switch_at[4] = {BTN_B, BTN_A, BTN_Y, BTN_X}, xbox_at[4] = {BTN_A, BTN_B, BTN_X, BTN_Y};
        static int old_face_mode = -1;
        static bool wait_face_release;
        int face_mode = config_live_face_buttons();
        if (old_face_mode >= 0 && old_face_mode != face_mode) wait_face_release = true;
        old_face_mode = face_mode;
        bool held_face = false;
        int menu_positions[4] = {1, 0, 3, 2};
        for (int i = 0; i < 4; i++) {
            int btn = switch_at[i];
            if (face_mode == FACE_LABEL) {
                switch (SDL_GetGamepadButtonLabel(pad, face[i])) {
                case SDL_GAMEPAD_BUTTON_LABEL_A: btn = BTN_A; break;
                case SDL_GAMEPAD_BUTTON_LABEL_B: btn = BTN_B; break;
                case SDL_GAMEPAD_BUTTON_LABEL_X: btn = BTN_X; break;
                case SDL_GAMEPAD_BUTTON_LABEL_Y: btn = BTN_Y; break;
                default: btn = xbox_at[i]; break;
                }
            }
            int semantic = btn == BTN_A ? 0 : btn == BTN_B ? 1 : btn == BTN_X ? 2 : 3;
            menu_positions[semantic] = i;
            bool pressed = SDL_GetGamepadButton(pad, face[i]);
            held_face |= pressed;
            if (pressed && !wait_face_release) b |= B(btn);
        }
        if (!held_face) wait_face_release = false;
        overlay_face_positions(menu_positions);
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 8000) b |= B(BTN_ZL);
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 8000) b |= B(BTN_ZR);
        // sticks: SDL's y is down; a small dead zone against drift
        int ax[4] = {SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), -SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY),
                     SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX), -SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY)};
        for (int i = 0; i < 4; i++)
            if (ax[i] > -3000 && ax[i] < 3000) ax[i] = 0;
        lx += ax[0], ly += ax[1], rx += ax[2], ry += ax[3];
        static const char *dbg;  // (MHGU_PAD_DEBUG=1) every raw SDL controller change: finds a button that presses itself
        if (!dbg) dbg = getenv("MHGU_PAD_DEBUG") ? "1" : "";
        if (*dbg) {
            static uint64_t pbtn;
            static int pax[6];
            uint64_t cb = 0;
            for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; i++)
                if (SDL_GetGamepadButton(pad, (SDL_GamepadButton)i)) cb |= 1ull << i;
            int dax[6];
            for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT; i++) dax[i] = SDL_GetGamepadAxis(pad, (SDL_GamepadAxis)i);
            for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; i++)
                if ((cb ^ pbtn) & (1ull << i))
                    LOG("[paddebug] %s %s\n", SDL_GetGamepadStringForButton((SDL_GamepadButton)i),
                        (cb >> i) & 1 ? "down" : "up");
            pbtn = cb;
            for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT; i++)
                if (dax[i] != pax[i] && (dax[i] > 500 || dax[i] < -500 || pax[i] > 500 || pax[i] < -500))
                    LOG("[paddebug] axis %s %d\n", SDL_GetGamepadStringForAxis((SDL_GamepadAxis)i), dax[i]);
            for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT; i++) pax[i] = dax[i];
        }
    } else {
        static bool scanned;  // (controllers present at startup send no ADDED event to a loop that started after them)
        if (!scanned) scanned = true, pick_pad();
    }

    uint64_t frame = g_present_count;
    for (int i = 0; i < nscript; i++) {
        const ScriptStep *st = &script[i];
        if (frame < st->start || frame >= st->start + st->len) continue;
        b |= st->buttons;
        if (st->stick == 0) lx = st->x, ly = st->y;
        if (st->stick == 1) rx = st->x, ry = st->y;
    }

    // A controller can reach host settings without needing a keyboard.
    static bool settings_chord;
    bool chord = (b & (B(BTN_PLUS) | B(BTN_MINUS))) == (B(BTN_PLUS) | B(BTN_MINUS));
    if (chord && !settings_chord && !typing) overlay_toggle_settings();
    if (chord && !typing) settings_chord = true;
    if (settings_chord) {
        // Swallow releases as well: releasing one half must not activate the game's menu.
        if (!(b & (B(BTN_PLUS) | B(BTN_MINUS)))) settings_chord = false;
        b &= ~(B(BTN_PLUS) | B(BTN_MINUS));
    }
    static bool menu, menu_face_release;
    if (overlay_menu_open() != menu) {
        if (menu) menu_face_release = true;
        menu = !menu;
        memset(keys, 0, sizeof keys);  // (keys held as it opened went to the menu: nothing stays pressed)
        mouse_held = 0;
    }
    if (menu) {  // Forge's menu has the controller (as in Forge: A activates, B cancels, stick and d-pad move)
        OverlayPad p = {b & B(BTN_A), b & B(BTN_B), b & B(BTN_X), b & B(BTN_Y), b & B(BTN_UP), b & B(BTN_DOWN),
                        b & B(BTN_LEFT), b & B(BTN_RIGHT), b & B(BTN_L), b & B(BTN_R), b & B(BTN_PLUS),
                        b & B(BTN_MINUS), clamp_axis(lx) / 32767.0f, clamp_axis(ly) / 32767.0f};
        overlay_pad(&p);
        b = 0, lx = ly = rx = ry = 0;
    }

    if (!menu && menu_face_release) {
        const uint64_t face_mask = B(BTN_A) | B(BTN_B) | B(BTN_X) | B(BTN_Y);
        b = menu_release_buttons(b, face_mask, &menu_face_release);
    }
    static uint64_t was;
    uint64_t down = b & ~was;
    was = b;
    // Plus (Start) confirms, B cancels the entry; the game keeps the rest of the controller. Dropping it whole read as a
    // lock-up: with no keyboard on screen (the game's own inline keyboard needs nativeNX/arc/swkbd_eu.arc, which is
    // missing), a pad-only player had no way out - B only reopened the entry. A line of chat (runtime/hle/chat.c)
    // leaves them to the game: it is typed on the keyboard, and B mid-hunt is a dodge, not losing the line.
    if (typing && !chat_active()) {
        if (down & B(BTN_PLUS)) textinput_key(TI_ENTER);
        else if (down & B(BTN_B)) textinput_key(TI_CANCEL);
        b &= ~(B(BTN_PLUS) | B(BTN_B));
    }
    input_set(b, clamp_axis(lx), clamp_axis(ly), clamp_axis(rx), clamp_axis(ry));
}
