// The bottom screen (bottom_hud.c) on Android's second display: a dual-screen handheld's other screen (AYN Thor, AYANEO
// Pocket DS...), or any display Android offers for a presentation. DualScreen.java shows an android.app.Presentation
// there with a SurfaceView and hands its Surface over; a thread of this file copies the panel's picture
// (second_display_compose) into it (ANativeWindow_lock: a CPU copy, so nothing here touches the game's Vulkan device or
// waits for it), and the view's touches come back here for the panels.
//   [Dual Screen] SecondDisplay = On (settings menu, Android tab), Panels, Background, Wallpaper: config.c
// With the setting off, or no second display, g_bottom_hud_on stays false: the frame path then costs one branch.
// The picture is made at the first panel surface's size; a later display of another size gets it scaled (Android
// scales a window's buffers to its surface).
#include "../../config.h"
#if defined(MHGU_DUAL_SCREEN) && defined(__ANDROID__)
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <SDL3/SDL.h>

#include "../../rt.h"
#include "bottom_hud.h"
#include "second_display.h"

#define SD_LOG(...) LOG("[second-display] " __VA_ARGS__)

static pthread_mutex_t win_lock = PTHREAD_MUTEX_INITIALIZER;
static ANativeWindow *win;  // the Presentation's surface, NULL none (win_lock)
static bool win_fresh;      // a new surface: the picture goes on it at once, new or not (win_lock)
static _Atomic uint32_t surf_w, surf_h;  // the view's size in pixels: touches come in those
static uint32_t pic_w, pic_h;            // the picture's (bottom_hud_set_size), fixed at the first surface
static uint32_t *frame;                  // the picture as the panel shows it, pic_w x pic_h (panel thread)
static pthread_t thread;
static bool thread_started;

bool second_display_event(const union SDL_Event *e) { (void)e; return false; }
bool second_display_window_open(void) { return false; }

// frame onto the surface (win_lock held). Its buffers are pic_w x pic_h RGBA_8888, the bytes as frame holds them.
static void post(void) {
    ANativeWindow_Buffer b;
    if (ANativeWindow_lock(win, &b, NULL) != 0) return;
    uint32_t w = (uint32_t)b.width < pic_w ? (uint32_t)b.width : pic_w, h = (uint32_t)b.height < pic_h ? (uint32_t)b.height : pic_h;
    for (uint32_t v = 0; v < h; v++) memcpy((uint32_t *)b.bits + (size_t)v * (uint32_t)b.stride, frame + (size_t)v * pic_w, (size_t)w * 4);
    ANativeWindow_unlockAndPost(win);
}

// The panel's thread, 60 times a second: the picture onto the surface when it changed, or when the surface is new.
static void *panel_thread(void *arg) {
    (void)arg;
    SecondDisplayState st = {0};
    struct timespec period = {0, 1000000000L / 60};
    for (;;) {
        nanosleep(&period, NULL);
        if (!g_bottom_hud_on) { st.empty_shown = false; continue; }
        bool drawn = second_display_compose(frame, pic_w, pic_h, &st);
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

// (the UI thread: SDL's own, which Java made)
void second_display_tick(void) {
    if (!config_second_display_take_request()) return;
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
    if (config_second_display_available() != (yes != 0)) SD_LOG("%s\n", yes ? "a second display is there" : "no second display");
    config_set_second_display_available(yes != 0);
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
