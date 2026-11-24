// The bottom screen (bottom_hud.c) on Android's second display: a dual-screen handheld's other screen (AYN Thor, AYANEO
// Pocket DS...), or any display Android offers for a presentation. DualScreen.java shows an android.app.Presentation
// there with a SurfaceView and hands its Surface over; a thread of this file copies the game's newest bottom picture
// into it (ANativeWindow_lock: a CPU copy, so nothing here touches the game's Vulkan device or waits for it), and the
// view's touches come back here for the panels.
//   [Dual Screen] SecondDisplay = On (settings menu, Android tab), Panels, Background, Wallpaper: config.c
// With the setting off, or no second display, g_bottom_hud_on stays false: the frame path then costs one branch.
// The picture is made at the first panel surface's size; a later display of another size gets it scaled (Android
// scales a window's buffers to its surface).
#include "../../config.h"
#ifdef MHGU_DUAL_SCREEN
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <dirent.h>
#include <errno.h>
#include <jni.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
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

static atomic_bool available;  // (DualScreen.java: a display to put it on)
static pthread_mutex_t win_lock = PTHREAD_MUTEX_INITIALIZER;
static ANativeWindow *win;  // the Presentation's surface, NULL none (win_lock)
static bool win_fresh;      // a new surface: the picture goes on it at once, new or not (win_lock)
static _Atomic uint32_t surf_w, surf_h;  // the view's size in pixels: touches come in those
static uint32_t pic_w, pic_h;            // the picture's (bottom_hud_set_size), fixed at the first surface
static uint32_t *frame;                  // the picture as the panel shows it, pic_w x pic_h (panel thread)
static pthread_t thread;
static bool thread_started;

bool second_display_available(void) { return atomic_load(&available); }

// ---- the wallpapers ([Dual Screen] Background = Wallpaper: Wallpaper names a BMP in wallpapers/, empty each in turn),
// loaded once when the setting changes, in the picture's own RGBA layout
#define WALLPAPERS_MAX 16
#define WALLPAPER_SECONDS 30
#define WALLPAPER_FADE_MS 1000  // (each change fades from one to the next over this time)
static uint32_t *wallpapers[WALLPAPERS_MAX];
static const uint32_t *wallpaper;  // (the one hud_copy puts round the panels)
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

static uint64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
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
    uint64_t now = now_ms();
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
// The game's bottom picture (RGBA8, w x h: bottom_hud.c) into frame, opaque, with the wallpaper round the panels.
static void hud_copy(const uint32_t *rgba, uint32_t w, uint32_t h, void *ctx) {
    bool quest = ctx == NULL;  // (a quest picture: its panels kept, the wallpaper round them; else all wallpaper)
    if (w != pic_w || h != pic_h) return;
    for (uint32_t v = 0; v < h; v++)
        for (uint32_t u = 0; u < w; u++) {
            uint32_t p = rgba[v * w + u];
            if (wallpaper && (!quest || !bottom_hud_covers(u, v, w, h))) p = wallpaper[v * w + u];
            frame[v * w + u] = 0xff000000u | p;
        }
}

// frame onto the surface (win_lock held). Its buffers are pic_w x pic_h RGBA_8888, the bytes as frame holds them.
static void post(void) {
    ANativeWindow_Buffer b;
    if (ANativeWindow_lock(win, &b, NULL) != 0) return;
    uint32_t w = (uint32_t)b.width < pic_w ? (uint32_t)b.width : pic_w, h = (uint32_t)b.height < pic_h ? (uint32_t)b.height : pic_h;
    for (uint32_t v = 0; v < h; v++) memcpy((uint32_t *)b.bits + (size_t)v * (uint32_t)b.stride, frame + (size_t)v * pic_w, (size_t)w * 4);
    ANativeWindow_unlockAndPost(win);
}

// The panel's thread, 60 times a second: the game's newest bottom picture when there is one, the background alone
// before the first and a while after the last (out of a quest), each onto the surface when it changed.
static void *panel_thread(void *arg) {
    (void)arg;
    uint64_t hud_seq = 0;  // (the game's last bottom picture shown, 0 none yet)
    bool empty_shown = false;
    struct timespec period = {0, 1000000000L / 60};
    for (;;) {
        nanosleep(&period, NULL);
        if (!g_bottom_hud_on) { empty_shown = false; continue; }
        bool had_hud = hud_seq != 0, changed, drawn = false;
        wallpaper = second_display_wallpaper(pic_w, pic_h, &changed);
        if (changed) empty_shown = false;
        if (bottom_hud_take(hud_copy, NULL, &hud_seq)) {
            empty_shown = false, drawn = true;
            if (!had_hud) SD_LOG("showing the game's bottom picture\n");
        } else if (!empty_shown && (!had_hud || bottom_hud_stale())) {
            uint32_t *empty = malloc((size_t)pic_w * pic_h * 4);
            if (empty) {
                bottom_hud_empty_picture(empty, pic_w, pic_h, false);  // (out of a quest: the background alone)
                hud_copy(empty, pic_w, pic_h, empty);
                free(empty);
                empty_shown = drawn = true;
            }
        }
        pthread_mutex_lock(&win_lock);
        if (win && (drawn || win_fresh)) post(), win_fresh = false;
        pthread_mutex_unlock(&win_lock);
    }
    return NULL;
}

// The bottom screen is on while the setting is and a surface is there (win_lock held).
static void update_on(void) {
    if (getenv("MHGU_BOTTOM_HUD_TEST")) return;  // (test mode: on without a panel, bottom_hud.c)
    bool on = g_config.second_display && win != NULL;
    if (on != g_bottom_hud_on) SD_LOG("%s\n", on ? "on: the panels' HUD parts leave the main screen" : "off: the HUD is all on the main screen");
    if (!on) bottom_hud_touch(false, 0, 0);  // (nothing held from a finger that was on it)
    g_bottom_hud_on = on;
}

// ---- Java: DualScreen.java
// The class through the activity's class loader (a thread the runtime made only sees the system's classes with FindClass),
// once.
static jclass dual_class(JNIEnv *env) {
    static jclass cls;
    if (cls) return cls;
    jobject act = (jobject)SDL_GetAndroidActivity();
    if (!act) return NULL;
    jclass ac = (*env)->GetObjectClass(env, act);
    jobject loader = (*env)->CallObjectMethod(env, act, (*env)->GetMethodID(env, ac, "getClassLoader", "()Ljava/lang/ClassLoader;"));
    jclass lc = (*env)->FindClass(env, "java/lang/ClassLoader");
    jstring name = (*env)->NewStringUTF(env, "io.github.morerokk.mhgurecomp.DualScreen");
    jclass c = (jclass)(*env)->CallObjectMethod(env, loader, (*env)->GetMethodID(env, lc, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;"), name);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env), c = NULL;
    if (c) cls = (jclass)(*env)->NewGlobalRef(env, c);
    (*env)->DeleteLocalRef(env, name);
    (*env)->DeleteLocalRef(env, lc);
    (*env)->DeleteLocalRef(env, loader);
    (*env)->DeleteLocalRef(env, ac);
    if (c) (*env)->DeleteLocalRef(env, c);
    (*env)->DeleteLocalRef(env, act);
    return cls;
}

void second_display_apply(void) {
    pthread_mutex_lock(&win_lock);
    update_on();
    pthread_mutex_unlock(&win_lock);
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jclass c = env ? dual_class(env) : NULL;
    jmethodID m = c ? (*env)->GetStaticMethodID(env, c, "want", "(Z)V") : NULL;
    if (!m) {
        if (env && (*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        SD_LOG("DualScreen isn't in this APK\n");
        return;
    }
    (*env)->CallStaticVoidMethod(env, c, m, (jboolean)(g_config.second_display != 0));
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
}

JNIEXPORT void JNICALL Java_io_github_morerokk_mhgurecomp_DualScreen_nativeAvailable(JNIEnv *env, jclass cls, jboolean yes) {
    (void)env, (void)cls;
    if (atomic_exchange(&available, yes != 0) != (yes != 0)) SD_LOG("%s\n", yes ? "a second display is there" : "no second display");
}

// The Presentation's surface (UI thread): a new one or its new size, or NULL when it goes (dismissed, display gone).
JNIEXPORT void JNICALL Java_io_github_morerokk_mhgurecomp_DualScreen_nativeSurface(JNIEnv *env, jclass cls, jobject surface, jint w, jint h) {
    (void)cls;
    pthread_mutex_lock(&win_lock);
    if (win) ANativeWindow_release(win), win = NULL;
    if (surface && w > 0 && h > 0) {
        win = ANativeWindow_fromSurface(env, surface);
        atomic_store(&surf_w, (uint32_t)w), atomic_store(&surf_h, (uint32_t)h);
        if (win && !pic_w) {  // (the first surface sets the picture's size for good: bottom_hud.c makes its images once)
            pic_w = (uint32_t)w, pic_h = (uint32_t)h;
            frame = calloc((size_t)pic_w * pic_h, 4);
            if (!frame) ANativeWindow_release(win), win = NULL, pic_w = pic_h = 0;
            else bottom_hud_set_size(pic_w, pic_h);
        }
        if (win) {
            ANativeWindow_setBuffersGeometry(win, (int32_t)pic_w, (int32_t)pic_h, WINDOW_FORMAT_RGBA_8888);
            win_fresh = true;
            if (!thread_started && pthread_create(&thread, NULL, panel_thread, NULL) == 0) pthread_detach(thread), thread_started = true;
            SD_LOG("surface %dx%d (picture %ux%u)\n", w, h, pic_w, pic_h);
        }
    }
    update_on();
    pthread_mutex_unlock(&win_lock);
}

// A finger on the Presentation's view (UI thread), in its pixels, or lifted.
JNIEXPORT void JNICALL Java_io_github_morerokk_mhgurecomp_DualScreen_nativeTouch(JNIEnv *env, jclass cls, jboolean down, jfloat x, jfloat y) {
    (void)env, (void)cls;
    uint32_t sw = atomic_load(&surf_w), sh = atomic_load(&surf_h), pw, ph;
    bottom_hud_size(&pw, &ph);
    if (!g_bottom_hud_on || !sw || !sh || !pw || !ph) return;
    float u = x * (float)pw / (float)sw, v = y * (float)ph / (float)sh;
    if (u < 0) u = 0;
    if (v < 0) v = 0;
    bottom_hud_touch(down != 0, (uint32_t)u, (uint32_t)v);
}
#endif
