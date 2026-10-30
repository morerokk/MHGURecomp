// The game window and the host main thread's event loop (the guest runs on its own threads). The window is created
// before the guest starts so the renderer can attach to it from the game's graphics thread.
//   MHGU_HIDDEN=1     no visible window (test runs; controls then come only from MHGU_INPUT)
// Display mode, size and the 3D resolution come from the settings (runtime/config.c, mhgurecomp.ini).
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "config.h"
#include "app_actions.h"
#include "overlay.h"
#include "mods.h"
#include "rt.h"

void backend_quit(void);  // (runtime/nvn: vulkan/device.c, backend_null.c)
static void quit(void) {
    LOG("window closed\n");
    backend_quit();  // (before exit(): runtime/nvn/vulkan/device.c exit_handler says why)
    exit(0);
}
static int launch_argc;
static char **launch_argv;
static SDL_AtomicInt requested_action, action_failed;
void ui_capture_launch_args(int argc, char **argv) { launch_argc = argc; launch_argv = argv; }
void ui_request_action(bool restart) { SDL_SetAtomicInt(&requested_action, restart ? 2 : 1); }
bool ui_action_failed(void) { return SDL_SetAtomicInt(&action_failed, 0) != 0; }
static bool restart_application(void) {
    char exe[4096], wait[80];
    if (!plat_exe_path(exe, sizeof exe)) return false;
    const char **args = calloc((size_t)launch_argc + 3, sizeof *args);
    if (!args) return false;
    int n = 0;
    args[n++] = exe;
    for (int i = 1; i < launch_argc; ++i)
        if (strncmp(launch_argv[i], "--recomp-wait=", 14)) args[n++] = launch_argv[i];
    snprintf(wait, sizeof wait, "--recomp-wait=%lu", plat_process_id());
    args[n++] = wait;
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)args);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_INHERITED);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_INHERITED);
    SDL_Process *child = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    free(args);
    if (!child) { LOG("[ui] restart failed: %s\n", SDL_GetError()); return false; }
    LOG("[ui] restart child %lld waiting for %lu\n", (long long)SDL_GetNumberProperty(
        SDL_GetProcessProperties(child), SDL_PROP_PROCESS_PID_NUMBER, 0), plat_process_id());
    SDL_DestroyProcess(child);
    return true;
}

volatile uint64_t g_present_count;
static SDL_Window *window;

SDL_Window *ui_window(void) { return window; }

// The window's size when windowed: the settings' size (or the last windowed size), no larger than the desktop's free
// area with the window frame.
static int win_w, win_h;
static void fit_desktop(int *w, int *h) {
    SDL_Rect r;
    if (!SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &r)) return;
    int top = 0, left = 0, bottom = 0, right = 0;
    if (window) SDL_GetWindowBordersSize(window, &top, &left, &bottom, &right);
    if (!top) top = 32, left = right = bottom = 8;  // (not known before the window exists: typical Windows 10 frame)
    r.w -= left + right, r.h -= top + bottom;
    if (*w <= r.w && *h <= r.h) return;
    int fw = r.w, fh = r.w * *h / *w;
    if (fh > r.h) fh = r.h, fw = r.h * *w / *h;
    LOG("[ui] %dx%d does not fit the desktop: window %dx%d\n", *w, *h, fw, fh);
    *w = fw, *h = fh;
}

// The window in the picture's shape, so it shows no bars: the desktop or the window manager may make it narrower than
// asked (macOS Stage Manager keeps a strip at the left: 1480x833 came out 1291x833), and the other side follows.
// Resizing by hand keeps the shape too (the aspect ratio lock set at creation).
static void fit_picture(void) {
    float a = (float)g_config.render_w / (float)g_config.render_h;
    int w = 0, h = 0, pw = 0, ph = 0;
    SDL_SyncWindow(window);
    SDL_GetWindowSize(window, &w, &h);
    if (!w || !h || (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN)) return;
    int fw = w, fh = h;
    if ((float)w / (float)h > a + 0.01f) fw = (int)((float)h * a + 0.5f);
    else if ((float)w / (float)h < a - 0.01f) fh = (int)((float)w / a + 0.5f);
    if (fw != w || fh != h) {
        SDL_SetWindowSize(window, fw, fh);
        SDL_SyncWindow(window);
    }
    SDL_GetWindowSize(window, &fw, &fh);
    SDL_GetWindowSizeInPixels(window, &pw, &ph);
    LOG("[ui] window %dx%d (%dx%d pixels)%s\n", fw, fh, pw, ph, fw != w || fh != h ? ", narrowed to the picture's shape" : "");
}

// Full screen as the settings say (exclusive: the monitor mode closest to Resolution; else borderless desktop), or
// windowed. The cursor only shows in a window.
static void set_fullscreen(bool on) {
    bool was = SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN;
    if (on && !was) SDL_GetWindowSize(window, &win_w, &win_h);
    if (on && g_config.display_mode == DISPLAY_FULLSCREEN) {
        SDL_DisplayMode m;
        SDL_DisplayID d = SDL_GetDisplayForWindow(window);
        if (SDL_GetClosestFullscreenDisplayMode(d, g_config.width, g_config.height, 0.0f, false, &m)) {
            SDL_SetWindowFullscreenMode(window, &m);
            LOG("[ui] fullscreen %dx%d @ %.0f Hz\n", m.w, m.h, m.refresh_rate);
        } else {
            SDL_SetWindowFullscreenMode(window, NULL);
            LOG("[ui] no fullscreen mode near %dx%d (%s): borderless instead\n", g_config.width, g_config.height,
                SDL_GetError());
        }
    } else if (on) {
        SDL_SetWindowFullscreenMode(window, NULL);
    }
    SDL_SetWindowFullscreen(window, on);
    if (!on && was) {  // (SDL restores the frame's size as the client size after an exclusive mode: set it again)
        SDL_SyncWindow(window);
        fit_desktop(&win_w, &win_h);
        SDL_SetWindowSize(window, win_w, win_h);
        fit_picture();
        SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
    if (on) SDL_HideCursor(); else SDL_ShowCursor();
}

void ui_init(void) {
    if (!SDL_Init(SDL_INIT_VIDEO)) FATAL("SDL_Init: %s\n", SDL_GetError());
    int sdl = SDL_GetVersion();
#ifdef MHGU_SDL_BUNDLED
    extern char g_sdl_choice[];  // (runtime/platform/linux_sdl.c)
    LOG("[ui] SDL %d.%d.%d, %s; video %s\n", SDL_VERSIONNUM_MAJOR(sdl), SDL_VERSIONNUM_MINOR(sdl),
        SDL_VERSIONNUM_MICRO(sdl), g_sdl_choice, SDL_GetCurrentVideoDriver());
#else
    LOG("[ui] SDL %d.%d.%d; video %s\n", SDL_VERSIONNUM_MAJOR(sdl), SDL_VERSIONNUM_MINOR(sdl), SDL_VERSIONNUM_MICRO(sdl),
        SDL_GetCurrentVideoDriver());
#endif
    config_load();
    // Initial background input policy. SDL documents this hint as changeable at runtime.
    // Subsequent saved changes are consumed by the host thread in ui_run().
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, g_config.background_input ? "1" : "0");
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) FATAL("SDL_InitSubSystem: %s\n", SDL_GetError());
    win_w = g_config.width, win_h = g_config.height;
    fit_desktop(&win_w, &win_h);
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#ifndef MHGU_NULL_BACKEND
    flags |= SDL_WINDOW_VULKAN;
#endif
    if (getenv("MHGU_HIDDEN")) flags |= SDL_WINDOW_HIDDEN;
    window = SDL_CreateWindow("Monster Hunter Generations Ultimate", win_w, win_h, flags);
    if (!window) FATAL("SDL_CreateWindow: %s\n", SDL_GetError());
    float a = (float)g_config.render_w / (float)g_config.render_h;
    SDL_SetWindowAspectRatio(window, a, a);
    fit_picture();
    if (g_config.display_mode != DISPLAY_WINDOWED) set_fullscreen(true);
#ifdef __APPLE__
    plat_menu_init();
#endif
}

// A live setting was changed (the Settings menu: config_set has put it in g_config and the live values): apply what
// isn't read as the game goes - the window mode. (MSAA, VSync and the controller in the background follow the live
// values: device.c frame_begin, ui_run.)
void ui_setting_changed(const char *key) {
    if (!strcmp(key, "Mode")) {
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) set_fullscreen(false);  // (Borderless <-> Fullscreen too)
        if (g_config.display_mode != DISPLAY_WINDOWED) set_fullscreen(true);
    }
}

// Alt+Enter: windowed <-> the full-screen mode of the settings (Windowed in the settings: borderless).
void ui_toggle_fullscreen(void) {
    set_fullscreen(!(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
}

int ui_run(void) {
    int background = config_live_background_input();
    for (;;) {
        int wanted_background = config_live_background_input();
        if (wanted_background != background) {
            SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, wanted_background ? "1" : "0");
            background = wanted_background;
        }
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 4)) {
            do {
                if (e.type == SDL_EVENT_QUIT) {
                    if (!config_confirm_exit() && !overlay_settings_dirty()) quit();
                    overlay_request_close();
                    continue;
                }
                host_input_event(&e);
            } while (SDL_PollEvent(&e));
        }
        int action = SDL_SetAtomicInt(&requested_action, 0);
        if (action == 1) quit();
        if (action == 2) {
#ifdef __APPLE__
            if (plat_restart_game()) quit();  // (the app again through LaunchServices: darwin_menu.m)
#else
            if (restart_application()) quit();
#endif
            SDL_SetAtomicInt(&action_failed, 1);
        }
        host_input_update();
        cheats_frame();
    }
}
