// The bottom screen (bottom_hud.c) on PC: a window of its own filling another monitor than the game's (a small touch
// screen, a dual-screen handheld's second panel under a desktop session, or any monitor). All of it on the UI thread
// (ui.c ui_run: second_display_tick, second_display_event), as SDL wants windows; the game's frame never waits for it:
// the picture is the copy bottom_hud.c left in host memory, uploaded to an SDL_Renderer texture when it changed.
//   [Dual Screen] SecondDisplay = On (settings menu, Display tab), Panels, Background, Wallpaper: config.c
// The window never takes the focus (SDL_WINDOW_NOT_FOCUSABLE): the keyboard and the mouse capture stay with the game.
// Touches on it, and left clicks, press the panels. The monitor: the first one the game's window isn't on, looked at
// again every second (a monitor plugged in or out, the game's window moved onto the bottom screen's monitor).
// The picture is made once, at that monitor's shape with its longer side 1280 at most, and scaled to the window.
//   MHGU_SECOND_DISPLAY_WINDOW=<w>x<h>   (test) an ordinary w x h window instead, on any monitor, the game's too: for a PC
//                                        with one monitor
#include "../../config.h"
#if defined(MHGU_DUAL_SCREEN) && !defined(__ANDROID__)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "../../rt.h"
#include "bottom_hud.h"
#include "second_display.h"

#define SD_LOG(...) LOG("[second-display] " __VA_ARGS__)
#define PICTURE_MAX 1280  // (the picture's longer side at most: the copy and the upload stay small; scaled up to the window)

static SDL_Window *bw;
static SDL_Renderer *br;
static SDL_Texture *bt;
static SDL_WindowID bw_id;
static SDL_DisplayID on_display;
static uint32_t pic_w, pic_h, *frame;
static SecondDisplayState st;
static bool fresh;  // (the window needs the picture again: new, shown again, resized)
static Uint64 next_frame, next_check;
static SDL_FingerID finger;  // (the one finger the panels follow, 0 none)
static bool mouse_down;
static bool dismissed;  // (its window closed from outside: not made again until the settings are saved)

bool second_display_window_open(void) { return bw != NULL; }

// The monitor for the bottom screen: the one it is on while that isn't the game's, else the first other one; 0 none.
static int test_w, test_h;  // (MHGU_SECOND_DISPLAY_WINDOW)
static SDL_DisplayID pick_display(void) {
    SDL_Window *game = ui_window();
    SDL_DisplayID g = game ? SDL_GetDisplayForWindow(game) : 0, pick = 0;
    if (test_w) return g ? g : SDL_GetPrimaryDisplay();
    int n = 0;
    SDL_DisplayID *ds = SDL_GetDisplays(&n);
    for (int i = 0; ds && i < n; i++) {
        if (ds[i] == g) continue;
        if (ds[i] == on_display && bw) { pick = ds[i]; break; }
        if (!pick) pick = ds[i];
    }
    SDL_free(ds);
    return pick;
}

static void close_window(void) {
    if (!bw) return;
    g_bottom_hud_on = false;
    bottom_hud_touch(false, 0, 0);
    finger = 0, mouse_down = false;
    if (bt) SDL_DestroyTexture(bt);
    if (br) SDL_DestroyRenderer(br);
    SDL_DestroyWindow(bw);
    bw = NULL, br = NULL, bt = NULL, bw_id = 0, on_display = 0;
    SD_LOG("off: the HUD is all on the main screen\n");
}

static bool open_window(SDL_DisplayID d) {
    SDL_Rect r;
    if (!SDL_GetDisplayBounds(d, &r) || r.w <= 0 || r.h <= 0) return false;
    // (Wayland places no window itself: full screen on that monitor there; elsewhere a borderless window over it, which
    // needs no mode change and stays put when the game's window has the focus)
    bool wayland = SDL_GetCurrentVideoDriver() && !strcmp(SDL_GetCurrentVideoDriver(), "wayland");
    SDL_WindowFlags flags = SDL_WINDOW_NOT_FOCUSABLE | SDL_WINDOW_BORDERLESS | (wayland ? SDL_WINDOW_FULLSCREEN : 0);
    Sint64 x = wayland ? (Sint64)SDL_WINDOWPOS_CENTERED_DISPLAY(d) : r.x, y = wayland ? (Sint64)SDL_WINDOWPOS_CENTERED_DISPLAY(d) : r.y;
    if (test_w) {  // (test: an ordinary window of that size, where the system puts it)
        r.w = test_w, r.h = test_h, flags = SDL_WINDOW_NOT_FOCUSABLE | SDL_WINDOW_RESIZABLE;
        x = y = (Sint64)SDL_WINDOWPOS_UNDEFINED;
    }
    SDL_PropertiesID p = SDL_CreateProperties();
    SDL_SetStringProperty(p, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "MHGU Recomp bottom screen");
    SDL_SetNumberProperty(p, SDL_PROP_WINDOW_CREATE_X_NUMBER, x);
    SDL_SetNumberProperty(p, SDL_PROP_WINDOW_CREATE_Y_NUMBER, y);
    SDL_SetNumberProperty(p, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, r.w);
    SDL_SetNumberProperty(p, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, r.h);
    SDL_SetNumberProperty(p, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, (Sint64)flags);
    bw = SDL_CreateWindowWithProperties(p);
    SDL_DestroyProperties(p);
    if (!bw) { SD_LOG("no window: %s\n", SDL_GetError()); return false; }
    br = SDL_CreateRenderer(bw, NULL);
    if (!br) { SD_LOG("no renderer: %s\n", SDL_GetError()); SDL_DestroyWindow(bw), bw = NULL; return false; }
    SDL_SetRenderVSync(br, 0);  // (never waits for this monitor: the UI thread's loop goes on)
    if (!pic_w) {  // (the first window sets the picture's size for good: bottom_hud.c makes its images once)
        float k = (float)PICTURE_MAX / (float)(r.w > r.h ? r.w : r.h);
        if (k > 1.0f) k = 1.0f;
        pic_w = (uint32_t)((float)r.w * k + 0.5f), pic_h = (uint32_t)((float)r.h * k + 0.5f);
        frame = calloc((size_t)pic_w * pic_h, 4);
        if (!frame) { pic_w = pic_h = 0; close_window(); return false; }
        bottom_hud_set_size(pic_w, pic_h);
    }
    bt = SDL_CreateTexture(br, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, (int)pic_w, (int)pic_h);
    if (!bt) { SD_LOG("no texture: %s\n", SDL_GetError()); close_window(); return false; }
    SDL_SetTextureScaleMode(bt, SDL_SCALEMODE_LINEAR);
    bw_id = SDL_GetWindowID(bw), on_display = d, fresh = true, st.empty_shown = false;
    const char *name = SDL_GetDisplayName(d);
    SD_LOG("window on %s (%dx%d at %d,%d, %s), picture %ux%u: the panels' HUD parts leave the main screen\n",
           name ? name : "?", r.w, r.h, r.x, r.y, SDL_GetRendererName(br), pic_w, pic_h);
    g_bottom_hud_on = true;
    return true;
}

void second_display_tick(void) {
    Uint64 now = SDL_GetTicks();
    static bool env_read;
    if (!env_read) {
        env_read = true;
        const char *e = getenv("MHGU_SECOND_DISPLAY_WINDOW");
        if (e && sscanf(e, "%dx%d", &test_w, &test_h) == 2 && test_w > 0 && test_h > 0) SD_LOG("test: a %dx%d window\n", test_w, test_h);
        else test_w = test_h = 0;
    }
    bool asked = config_second_display_take_request();
    if (asked || now >= next_check) {
        next_check = now + 1000;
        if (asked) dismissed = false;
        SDL_DisplayID d = pick_display();
        if (config_second_display_available() != (d != 0)) SD_LOG("%s\n", d ? "a second monitor is there" : "no second monitor");
        config_set_second_display_available(d != 0);
        bool want = g_config.second_display && d && !dismissed && !getenv("MHGU_BOTTOM_HUD_TEST");
        if (bw && (!want || d != on_display)) close_window();
        if (want && !bw) open_window(d);
    }
    if (!bw || now < next_frame) return;
    next_frame = now + 16;  // (about 60 times a second)
    if (!second_display_compose(frame, pic_w, pic_h, &st) && !fresh) return;
    fresh = false;
    SDL_UpdateTexture(bt, NULL, frame, (int)pic_w * 4);
    SDL_RenderClear(br);
    SDL_RenderTexture(br, bt, NULL, NULL);
    SDL_RenderPresent(br);
}

// A point of the window (its own coordinates) as a picture pixel, for the panels.
static void touch_at(bool down, float x, float y) {
    int w = 0, h = 0;
    SDL_GetWindowSize(bw, &w, &h);
    if (w <= 0 || h <= 0) return;
    float u = x * (float)pic_w / (float)w, v = y * (float)pic_h / (float)h;
    bottom_hud_touch(down, u > 0 ? (uint32_t)u : 0, v > 0 ? (uint32_t)v : 0);
}

bool second_display_event(const union SDL_Event *ev) {
    const SDL_Event *e = (const SDL_Event *)ev;
    if (!bw) return false;
    if (e->type == SDL_EVENT_DISPLAY_ADDED || e->type == SDL_EVENT_DISPLAY_REMOVED) next_check = 0;  // (and the game sees it too)
    SDL_Window *w = SDL_GetWindowFromEvent(e);
    if (!w || SDL_GetWindowID(w) != bw_id) return false;
    switch (e->type) {
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION:
        if (!finger) finger = e->tfinger.fingerID;
        if (e->tfinger.fingerID == finger) {
            int ww = 0, wh = 0;
            SDL_GetWindowSize(bw, &ww, &wh);
            touch_at(true, e->tfinger.x * (float)ww, e->tfinger.y * (float)wh);
        }
        break;
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED:
        if (e->tfinger.fingerID == finger) finger = 0, bottom_hud_touch(false, 0, 0);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        // (a mouse's left button presses as a finger does; the mouse events SDL makes from touches are left out: the
        // finger events above have them)
        if (e->button.which != SDL_TOUCH_MOUSEID && e->button.button == SDL_BUTTON_LEFT) {
            mouse_down = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            touch_at(mouse_down, e->button.x, e->button.y);
        }
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (mouse_down && e->motion.which != SDL_TOUCH_MOUSEID) touch_at(true, e->motion.x, e->motion.y);
        break;
    case SDL_EVENT_WINDOW_EXPOSED:
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_RESTORED:
        fresh = true;
        break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:  // (closed from the taskbar: gone until the game starts again or the settings are
        dismissed = true;                   // saved; the setting itself stays on)
        close_window();
        break;
    default:
        break;
    }
    return true;  // (all of the bottom screen window's events are its own)
}
#endif
