// The bottom screen's panel as every platform shows it (second_display.h): the game's bottom picture (bottom_hud.c), the
// background before and after a quest, the wallpapers. Putting it on a display is second_display_android.c's and
// second_display_desktop.c's.
#include "../../config.h"
#ifdef MHGU_DUAL_SCREEN
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <SDL3/SDL.h>

#include "../../rt.h"
#include "bottom_hud.h"
#include "second_display.h"

#define SD_LOG(...) LOG("[second-display] " __VA_ARGS__)

// ---- the wallpapers ([Dual Screen] Background = Wallpaper: Wallpaper names a BMP in wallpapers/, empty each in turn),
// loaded once when the setting changes, in the picture's own RGBA layout
#define WALLPAPERS_MAX 16
#define WALLPAPER_SECONDS 30
#define WALLPAPER_FADE_MS 1000  // (each change fades from one to the next over this time)
static uint32_t *wallpapers[WALLPAPERS_MAX];
static int nwallpapers;

// One BMP of wallpapers/ into w x h, scaled to cover and cropped at its centre. NULL: no such file.
static uint32_t *load_wallpaper(const char *file, uint32_t w, uint32_t h) {
    char path[320];
    snprintf(path, sizeof path, "wallpapers/%s", file);
    SDL_Surface *bmp = SDL_LoadBMP(path);
    SDL_Surface *img = bmp ? SDL_ConvertSurface(bmp, SDL_PIXELFORMAT_RGBA32) : NULL;
    if (bmp) SDL_DestroySurface(bmp);
    if (!img) { SD_LOG("no wallpaper (%s): %s\n", path, SDL_GetError()); return NULL; }
    uint32_t *px = malloc((size_t)w * h * 4);
    if (px) {
        float k = fmaxf((float)w / (float)img->w, (float)h / (float)img->h);
        float x0 = ((float)img->w - (float)w / k) * 0.5f, y0 = ((float)img->h - (float)h / k) * 0.5f;
        for (uint32_t v = 0; v < h; v++) {
            const uint32_t *row = (const uint32_t *)((const uint8_t *)img->pixels + (size_t)img->pitch * (uint32_t)(y0 + (float)v / k));
            for (uint32_t u = 0; u < w; u++) px[v * w + u] = row[(uint32_t)(x0 + (float)u / k)] | 0xff000000u;
        }
        SD_LOG("wallpaper %s %dx%d\n", path, img->w, img->h);
    }
    SDL_DestroySurface(img);
    return px;
}

static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

// The [Dual Screen] Wallpaper, or when it is empty each BMP of wallpapers/ (by name), at most max, into out. Their count:
// 0 when Background is not Wallpaper or no file loads.
static int load_wallpapers(uint32_t w, uint32_t h, uint32_t **out, int max) {
    if (g_config.bottom_background != BOTTOM_BG_WALLPAPER || max < 1) return 0;
    if (g_config.bottom_wallpaper[0]) return (*out = load_wallpaper(g_config.bottom_wallpaper, w, h)) != NULL;
    char *names[WALLPAPERS_MAX];
    int n = 0;
    DIR *d = opendir("wallpapers");
    if (!d) { SD_LOG("no wallpapers folder: %s\n", strerror(errno)); return 0; }
    for (struct dirent *e; (e = readdir(d)) && n < WALLPAPERS_MAX;) {
        size_t len = strlen(e->d_name);
        if (len > 4 && !strcasecmp(e->d_name + len - 4, ".bmp")) names[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof names[0], by_name);
    int got = 0;
    for (int i = 0; i < n; i++) {
        if (got < max && names[i] && (out[got] = load_wallpaper(names[i], w, h))) got++;
        free(names[i]);
    }
    return got;
}

// out = a faded towards b by t / 256 (opaque RGBA8), two channels per multiply.
static void blend_wallpapers(uint32_t *out, const uint32_t *a, const uint32_t *b, size_t n, uint32_t t) {
    for (size_t i = 0; i < n; i++) {
        uint32_t x = a[i], y = b[i];
        uint32_t rb = (((x & 0xff00ffu) * (256 - t) + (y & 0xff00ffu) * t) >> 8) & 0xff00ffu;
        uint32_t g = (((x & 0xff00u) * (256 - t) + (y & 0xff00u) * t) >> 8) & 0xff00u;
        out[i] = 0xff000000u | rb | g;
    }
}

const uint32_t *second_display_wallpaper(uint32_t w, uint32_t h, bool *changed) {
    static int background = -1, shown, from = -1;  // (from: the one fading out, -1 none)
    static char name[sizeof g_config.bottom_wallpaper];
    static uint64_t shown_at;
    static uint32_t *faded;  // (the picture between the two while they fade)
    uint64_t now = SDL_GetTicks();
    *changed = false;
    if (background != g_config.bottom_background || strcmp(name, g_config.bottom_wallpaper)) {
        background = g_config.bottom_background;
        snprintf(name, sizeof name, "%s", g_config.bottom_wallpaper);
        for (int i = 0; i < nwallpapers; i++) free(wallpapers[i]);
        nwallpapers = load_wallpapers(w, h, wallpapers, WALLPAPERS_MAX);
        free(faded);
        faded = nwallpapers > 1 ? malloc((size_t)w * h * 4) : NULL;
        shown = 0, from = -1, shown_at = now, *changed = true;  // (a setting change shows the new one at once)
    } else if (nwallpapers > 1 && faded && now - shown_at >= WALLPAPER_SECONDS * 1000u) {
        from = shown, shown = (shown + 1) % nwallpapers, shown_at = now;
    }
    if (from >= 0) {  // (fading: a new picture every call until it is done)
        *changed = true;
        uint64_t t = now - shown_at;
        if (t < WALLPAPER_FADE_MS) {
            blend_wallpapers(faded, wallpapers[from], wallpapers[shown], (size_t)w * h, (uint32_t)(t * 256 / WALLPAPER_FADE_MS));
            return faded;
        }
        from = -1;
    }
    return nwallpapers ? wallpapers[shown] : NULL;
}

// ---- the picture
typedef struct {
    uint32_t *frame, w, h;
    const uint32_t *wallpaper;
    bool quest;  // (a quest picture: its panels kept, the wallpaper round them; else all wallpaper)
} Copy;

// The game's bottom picture (RGBA8, w x h: bottom_hud.c) into the frame, opaque, with the wallpaper round the panels.
static void hud_copy(const uint32_t *rgba, uint32_t w, uint32_t h, void *ctx) {
    const Copy *c = ctx;
    if (w != c->w || h != c->h) return;
    for (uint32_t v = 0; v < h; v++)
        for (uint32_t u = 0; u < w; u++) {
            uint32_t p = rgba[v * w + u];
            if (c->wallpaper && (!c->quest || !bottom_hud_covers(u, v, w, h))) p = c->wallpaper[v * w + u];
            c->frame[v * w + u] = 0xff000000u | p;
        }
}

bool second_display_compose(uint32_t *frame, uint32_t w, uint32_t h, SecondDisplayState *st) {
    bool had_hud = st->hud_seq != 0, changed;
    Copy c = {frame, w, h, second_display_wallpaper(w, h, &changed), true};
    if (changed) st->empty_shown = false;
    if (bottom_hud_take(hud_copy, &c, &st->hud_seq)) {
        if (!had_hud) SD_LOG("showing the game's bottom picture\n");
        st->empty_shown = false;
        return true;
    }
    // (nothing new from the game: the panel keeps its picture, until no panel piece has been drawn for a while)
    if (st->empty_shown || (had_hud && !bottom_hud_stale())) return false;
    uint32_t *empty = malloc((size_t)w * h * 4);
    if (!empty) return false;
    bottom_hud_empty_picture(empty, w, h, false);  // (out of a quest: the background alone)
    c.quest = false;
    hud_copy(empty, w, h, &c);
    free(empty);
    st->empty_shown = true;
    return true;
}

void second_display_apply(void) { config_second_display_request(); }
#endif
