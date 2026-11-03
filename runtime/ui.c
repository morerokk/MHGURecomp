// The game window and the host main thread's event loop (the guest runs on its own threads). The window is created
// before the guest starts so the renderer can attach to it from the game's graphics thread.
//   MHGU_HIDDEN=1     no visible window (test runs; controls then come only from MHGU_INPUT)
// Display mode, size and the 3D resolution come from the settings (runtime/config.c, mhgurecomp.ini).
#include <stdlib.h>

#include <SDL3/SDL.h>

#include "config.h"
#include "app_actions.h"
#include "overlay.h"
#include "hle/chat.h"
#include "mods.h"
#include "rt.h"

static int launch_argc;
static char **launch_argv;
static SDL_AtomicInt requested_action, action_failed;
void ui_capture_launch_args(int argc, char **argv) { launch_argc = argc; launch_argv = argv; }
void ui_request_action(bool restart) { SDL_SetAtomicInt(&requested_action, restart ? 2 : 1); }
void ui_request_import(int what) { SDL_SetAtomicInt(&requested_action, 2 + what); }
static SDL_AtomicInt away;
bool ui_window_away(void) { return SDL_GetAtomicInt(&away) != 0; }
#ifdef __ANDROID__
bool android_import(int what, bool running);  // (runtime/platform/android.c)
int android_take_picked(char *out, size_t size);
void android_config_loaded(void);
#endif
int ui_take_picked_folder(char *out, size_t size) {
#ifdef __ANDROID__
    return android_take_picked(out, size);
#else
    (void)out, (void)size;
    return 0;
#endif
}
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

// Full screen as the settings say (exclusive: the monitor mode closest to Resolution; else borderless desktop), or
// windowed. The cursor only shows in a window, or over a menu or the chat box (they show it only as they open, so it
// stays shown when full screen comes while one is open: F11, Alt+Enter).
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
        SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
    if (on && !overlay_menu_open() && !chat_active()) SDL_HideCursor(); else SDL_ShowCursor();
}

// The window's icon. Windows takes it from the exe (extras/icon/mhgurecomp.rc) and Android from the launcher icon, so only
// Linux sets one: extras/icon/mhgurecomp-128.png, built into the program (the path is relative to the repo, where make runs).
#if !defined(_WIN32) && !defined(__ANDROID__)
__asm__(".pushsection .rodata\n.globl mhgu_icon_png\nmhgu_icon_png:\n.incbin \"extras/icon/mhgurecomp-128.png\"\n"
        ".globl mhgu_icon_png_end\nmhgu_icon_png_end:\n.popsection");
extern const unsigned char mhgu_icon_png[], mhgu_icon_png_end[];
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "platform.h"

// A Linux executable can't carry an icon for the file manager, so the desktop finds the game through a launcher:
// $XDG_DATA_HOME/applications/mhgurecomp.desktop (~/.local/share/applications) and the icon beside it in
// icons/hicolor/128x128/apps/mhgurecomp.png. Written at each start when missing or out of date (the folder moved), so the
// app menu starts this copy: mhgurecomp-updater when it is beside the game (it starts the game, and with Updates = Off
// only that), else the game. A launcher without the X-MHGURecomp line is the player's own and left alone. The windows'
// app id is the launcher's name (SDL: the executable's name; the updater sets it), so the taskbar shows the icon too.
static bool write_if_changed(const char *path, const void *data, size_t size) {
    FILE *f = fopen(path, "rb");
    if (f) {
        char old[8192];
        size_t n = fread(old, 1, sizeof old, f);
        fclose(f);
        if (n == size && !memcmp(old, data, size)) return true;
    }
    char tmp[PATH_MAX + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (!(f = fopen(tmp, "wb"))) return false;
    bool ok = fwrite(data, 1, size, f) == size;
    if (fclose(f) != 0) ok = false;
    if (ok && rename(tmp, path) != 0) ok = false;
    if (!ok) unlink(tmp);
    return ok;
}

static void mkdirs(char *path) {  // (each folder of path, which is a folder)
    for (char *p = path + 1; *p; ++p)
        if (*p == '/') { *p = 0; mkdir(path, 0755); *p = '/'; }
    mkdir(path, 0755);
}

static void install_launcher(void) {
    if (getenv("MHGU_HIDDEN") || getenv("MHGU_NO_LAUNCHER")) return;
    char exe[PATH_MAX], data[PATH_MAX], dir[PATH_MAX + 64], path[PATH_MAX + 96];
    if (!plat_exe_path(exe, sizeof exe)) return;
    if (strpbrk(exe, "\"`$\\%\n\r")) { LOG("[ui] no app menu launcher: the game's path has a \" ` $ \\ or %% in it\n"); return; }
    const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    if (xdg && *xdg == '/') snprintf(data, sizeof data, "%s", xdg);
    else if (home && *home == '/') snprintf(data, sizeof data, "%s/.local/share", home);
    else return;
    char *slash = strrchr(exe, '/');
    char run[PATH_MAX + 32];
    snprintf(run, sizeof run, "%.*s/mhgurecomp-updater", (int)(slash - exe), exe);
    if (access(run, X_OK) != 0) snprintf(run, sizeof run, "%s", exe);

    snprintf(path, sizeof path, "%s/applications/mhgurecomp.desktop", data);
    FILE *f = fopen(path, "rb");
    if (f) {
        char old[8192];
        size_t n = fread(old, 1, sizeof old - 1, f);
        fclose(f);
        old[n] = 0;
        if (!strstr(old, "\nX-MHGURecomp=true\n")) return;
    }
    snprintf(dir, sizeof dir, "%s/icons/hicolor/128x128/apps", data);
    mkdirs(dir);
    snprintf(path, sizeof path, "%s/mhgurecomp.png", dir);
    if (!write_if_changed(path, mhgu_icon_png, (size_t)(mhgu_icon_png_end - mhgu_icon_png))) {
        LOG("[ui] no app menu launcher: can't write %s\n", path);
        return;
    }
    char entry[3 * PATH_MAX + 512];
    int n = snprintf(entry, sizeof entry,
                     "[Desktop Entry]\nType=Application\nName=MHGU Recomp\nComment=Monster Hunter Generations Ultimate, "
                     "recompiled\nExec=\"%s\"\nPath=%.*s\nIcon=mhgurecomp\nTerminal=false\nCategories=Game;\n"
                     "StartupWMClass=mhgurecomp\nX-MHGURecomp=true\n",
                     run, (int)(slash - exe), exe);
    if (n <= 0 || (size_t)n >= sizeof entry) return;
    snprintf(dir, sizeof dir, "%s/applications", data);
    mkdirs(dir);
    snprintf(path, sizeof path, "%s/mhgurecomp.desktop", dir);
    if (!write_if_changed(path, entry, (size_t)n)) LOG("[ui] no app menu launcher: can't write %s\n", path);
}

static void set_window_icon(void) {
    install_launcher();
    SDL_Surface *s = SDL_LoadPNG_IO(SDL_IOFromConstMem(mhgu_icon_png, (size_t)(mhgu_icon_png_end - mhgu_icon_png)), true);
    if (!s) { LOG("[ui] window icon: %s\n", SDL_GetError()); return; }
    SDL_SetWindowIcon(window, s);
    SDL_DestroySurface(s);
}
#else
static void set_window_icon(void) {}
#endif

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
#ifdef __ANDROID__
    if (g_config.save_dir[0]) g_save_dir = g_config.save_dir;  // ([Android] SaveFolder: MHGU_SAVE_DIR still wins, fs.c)
    android_config_loaded();
#endif
    // Initial background input policy. SDL documents this hint as changeable at runtime.
    // Subsequent saved changes are consumed by the host thread in ui_run().
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, g_config.background_input ? "1" : "0");
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) FATAL("SDL_InitSubSystem: %s\n", SDL_GetError());
    win_w = g_config.width, win_h = g_config.height;
#ifdef __ANDROID__
    // (a resizable window may turn any way on Android, whatever the manifest says: landscape only. The window
    // fills the screen there, so the settings' size does not apply.)
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
#else
    fit_desktop(&win_w, &win_h);
#endif
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#ifndef MHGU_NULL_BACKEND
    flags |= SDL_WINDOW_VULKAN;
#endif
    if (getenv("MHGU_HIDDEN")) flags |= SDL_WINDOW_HIDDEN;
    window = SDL_CreateWindow("Monster Hunter Generations Ultimate", win_w, win_h, flags);
    if (!window) FATAL("SDL_CreateWindow: %s\n", SDL_GetError());
    set_window_icon();
    if (g_config.display_mode != DISPLAY_WINDOWED) set_fullscreen(true);
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
                    if (!config_confirm_exit() && !overlay_settings_dirty()) { LOG("window closed\n"); exit(0); }
                    overlay_request_close();
                    continue;
                }
                host_input_event(&e);
            } while (SDL_PollEvent(&e));
        }
        int action = SDL_SetAtomicInt(&requested_action, 0);
        if (action == 1) exit(0);
        if (action == 2) {
            if (restart_application()) exit(0);
            SDL_SetAtomicInt(&action_failed, 1);
        }
#ifdef __ANDROID__
        // (on this thread: SDL's own, which Java made. The app closes after an import: the game has the old files open)
        if (action >= 3 && action <= 9) {
            SDL_SetAtomicInt(&away, 1);  // (the picker takes the window's surface away: the renderer draws nothing)
            // (after 1 and 2 the app closes, the game has the old files open; the others leave it running)
            if (android_import(action - 2, true) && action <= 4) { LOG("[ui] imported: closing\n"); exit(0); }
            SDL_SetAtomicInt(&away, 0);
        }
#endif
        host_input_update();
        cheats_frame();
    }
}
