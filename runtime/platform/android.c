// Android host bits beside runtime/platform/linux.c (the Makefile's PLATFORM=android; docs/research/android.md).
//
// The app's folder is its external files folder, /sdcard/Android/data/<package>/files: the player (or adb push) puts
// the game's exefs / romfs in its data/ there, and the settings, saves, caches, mods, cheats and log.txt go beside
// them, as in the PC build's game folder. Android gives no app an environment to start with, so the debug variables
// (MHGU_QUICKHUNT, MHGU_NO_BC...) come from env.txt in that folder: one NAME=value per line, # for comments.
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <SDL3/SDL.h>
#include <jni.h>

#include "config.h"
#include "modorder.h"
#include "rt.h"

const char *android_home(void) {
    static char dir[1024];
    if (!dir[0]) {
        const char *p = SDL_GetAndroidExternalStoragePath();  // (SDL's Java activity has set up JNI before SDL_main)
        snprintf(dir, sizeof dir, "%s", p ? p : "/sdcard/MHGURecomp");
        mkdir(dir, 0777);
    }
    return dir;
}

// <the app's folder>/<rel> (an absolute path: SDL's file functions read relative ones from the APK's assets)
static const char *android_path(const char *rel) {
    static char path[1100];
    snprintf(path, sizeof path, "%s/%s", android_home(), rel);
    return path;
}

void android_load_env(void) {
    char path[1100];
    snprintf(path, sizeof path, "%s/env.txt", android_home());
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq || eq == line) continue;
        *eq = 0;
        setenv(line, eq + 1, 1);
    }
    fclose(f);
}

// The files a PC release has beside the exe come in the APK's assets (android/apk.mk): copied into the app's folder
// (the working directory by now) at every start, pipelines.list whenever it differs (a newer release's), bonus/system
// only when missing (the player may have put their own there). data/ made too, so the player sees where the game
// files go.
static void unpack(const char *name, bool replace) {
    size_t n;
    void *asset = SDL_LoadFile(name, &n);  // (a relative path: from the APK's assets)
    if (!asset) return;
    size_t m = 0;
    void *have = SDL_LoadFile_IO(SDL_IOFromFile(android_path(name), "rb"), &m, true);
    if (!have || (replace && (m != n || memcmp(have, asset, n)))) {
        char tmp[1100];
        snprintf(tmp, sizeof tmp, "%s.part", android_path(name));
        FILE *f = fopen(tmp, "wb");
        if (f) {
            bool ok = fwrite(asset, 1, n, f) == n;
            if (fclose(f) == 0 && ok) rename(tmp, android_path(name));
            else remove(tmp);
        }
    }
    SDL_free(have);
    SDL_free(asset);
}

void android_unpack_assets(void) {
    mkdir(android_path("data"), 0777);
    mkdir(android_path("bonus"), 0777);
    unpack("pipelines.list", true);
    unpack("bonus/system", false);
}

// The game stopped on an error (plat_fatal_notice): at the next start the app offers to share this run's log
// (android/java: MHGUActivity's offerCrashLog). Only open/close: this can run in a fault handler.
void android_mark_crash(void) {
    int fd = open(android_path("last-run-crashed"), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) close(fd);
}

void android_error_box(const char *msg) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "MHGU Recomp", msg, NULL);
}

// ------------------------------------------------------------------ [Android] GameFolder
// The game files can stay in a folder of the player's (a USB drive, an SD card: main.c). main.c needs it before the
// settings can load (config_load wants the window system up), so this reads that one line of the ini.
void android_game_folder(char *out, size_t size) {
    char path[1100];
    config_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[1400];
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (strncasecmp(s, "GameFolder", 10)) continue;
        s += 10;
        while (*s == ' ' || *s == '\t') s++;
        if (*s++ != '=') continue;
        while (*s == ' ' || *s == '\t') s++;
        s[strcspn(s, ";#\r\n")] = 0;
        for (char *e = s + strlen(s); e > s && (e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0;
        if (*s) snprintf(out, size, "%s", s);
        break;
    }
    fclose(f);
}

// A new value from outside the settings menu (chosen at the first start, or undone by an import): saved at once, or when
// the settings have loaded (ui.c).
static char pending_folder[sizeof g_config.game_dir];
static bool pending, config_ready;

static void store_game_folder(void) {
    Config c = config_editable();
    char error[200];
    snprintf(c.game_dir, sizeof c.game_dir, "%s", pending_folder);
    if (!config_save(&c, error, sizeof error)) LOG("[android] game files folder not saved: %s\n", error);
    pending = false;
}

static void set_game_folder(const char *folder) {
    snprintf(pending_folder, sizeof pending_folder, "%s", folder);
    pending = true;
    if (config_ready) store_game_folder();
}

void android_config_loaded(void) {
    config_ready = true;
    if (pending) store_game_folder();
}

// ------------------------------------------------------------------ importing the game files or a save
// The system's file picker and the copy are Java's (android/java: ImportActivity.java), which has its own dialogs;
// this waits until it is done. 1: game files (into the data folder), 2: a save (into save/save_0), 3: the save out to a
// folder, 5 / 6: a mod (zip / folder) into the mods folder, 4 / 7: choose the folder to keep the save in / the folder
// holding the game files (its path: android_take_picked). running: the game has started (the app closes after 1 and 2).
// true when it was done.

static const char *absolute(const char *dir, char *buf, size_t size) {
    if (dir[0] == '/') return dir;
    snprintf(buf, size, "%s/%s", android_home(), dir);
    return buf;
}

static char picked_path[1100];
static SDL_AtomicInt picked_ready;  // the kind of import (4 or 7) that chose picked_path

// The folder chosen by the last import of kind 4 or 7, once: which kind it was (0: none).
int android_take_picked(char *out, size_t size) {
    int what = SDL_SetAtomicInt(&picked_ready, 0);
    if (what) snprintf(out, size, "%s", picked_path);
    return what;
}

bool android_import(int what, bool running) {
    char buf[1100], folder[1200];
    if (what == 1) snprintf(folder, sizeof folder, "%s", android_path("data"));  // (always the app's own: see below)
    else if (what == 5 || what == 6) snprintf(folder, sizeof folder, "%s", absolute(mods_dir(), buf, sizeof buf));
    else snprintf(folder, sizeof folder, "%s/save_0", absolute(getenv("MHGU_SAVE_DIR") ? getenv("MHGU_SAVE_DIR") : g_save_dir, buf, sizeof buf));
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = (jobject)SDL_GetAndroidActivity();
    if (!env || !act) return false;
    // (through the activity's class loader: a thread the runtime made only sees the system's classes with FindClass)
    jclass ac = (*env)->GetObjectClass(env, act);
    jobject loader = (*env)->CallObjectMethod(env, act, (*env)->GetMethodID(env, ac, "getClassLoader", "()Ljava/lang/ClassLoader;"));
    jclass lc = (*env)->FindClass(env, "java/lang/ClassLoader");
    jstring name = (*env)->NewStringUTF(env, "io.github.morerokk.mhgurecomp.ImportActivity");
    jclass c = (jclass)(*env)->CallObjectMethod(env, loader, (*env)->GetMethodID(env, lc, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;"), name);
    jmethodID begin = c ? (*env)->GetStaticMethodID(env, c, "begin", "(Landroid/app/Activity;ILjava/lang/String;Z)V") : NULL;
    jmethodID state = c ? (*env)->GetStaticMethodID(env, c, "state", "()I") : NULL;
    jmethodID pickedm = c ? (*env)->GetStaticMethodID(env, c, "picked", "()Ljava/lang/String;") : NULL;
    int result = 3;
    if ((*env)->ExceptionCheck(env) || !begin || !state || !pickedm) {
        (*env)->ExceptionClear(env);
        LOG("[android] the import isn't in this APK\n");
    } else {
        LOG("[android] %s: %s\n", what == 1 ? "importing the game files into" : what == 2 ? "importing a save into" :
            what == 3 ? "exporting the save from" : what == 7 ? "choosing the game files' folder, now" : "choosing the save folder, now", folder);
        jstring dir = (*env)->NewStringUTF(env, folder);
        (*env)->CallStaticVoidMethod(env, c, begin, act, (jint)what, dir, (jboolean)running);
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        else while ((result = (*env)->CallStaticIntMethod(env, c, state)) == 0) SDL_Delay(100);
        (*env)->DeleteLocalRef(env, dir);
        if ((what == 4 || what == 7) && result == 1) {
            jstring str = (jstring)(*env)->CallStaticObjectMethod(env, c, pickedm);
            const char *chars = str ? (*env)->GetStringUTFChars(env, str, NULL) : NULL;
            if (chars && *chars) {
                snprintf(picked_path, sizeof picked_path, "%s", chars);
                SDL_SetAtomicInt(&picked_ready, what);
            }
            if (chars) (*env)->ReleaseStringUTFChars(env, str, chars);
            if (str) (*env)->DeleteLocalRef(env, str);
        }
        LOG("[android] import %s\n", result == 1 ? "done" : result == 2 ? "cancelled" : "failed");
        if (what == 1 && result == 1) set_game_folder("");  // (the player just put the game files in the app's folder)
    }
    (*env)->DeleteLocalRef(env, name);
    if (c) (*env)->DeleteLocalRef(env, c);
    (*env)->DeleteLocalRef(env, lc);
    if (loader) (*env)->DeleteLocalRef(env, loader);
    (*env)->DeleteLocalRef(env, ac);
    (*env)->DeleteLocalRef(env, act);
    return result == 1;
}

// At the start, the game files missing or not right in data (msg): offers to import them or to use them where they are,
// and data then says where they are. true: done, check again.
bool android_offer_import(const char *msg, char *data, size_t size) {
    char text[2600], where[1300] = "";
    if (strcmp(data, "data")) snprintf(where, sizeof where, "\n\nThe app looked in %s. If it's on a drive, is the drive connected?", data);
    snprintf(text, sizeof text, "%s%s\n\nPick the folder that holds the exefs and romfs folders. The app can copy them into its own "
             "folder (13 GB of free space, 26 GB while copying), or use them where they are, for example on a USB drive or SD card.",
             msg, where);
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Quit"},
        {0, 2, "Use where they are"},
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Import game files"},
    };
    const SDL_MessageBoxData box = {SDL_MESSAGEBOX_INFORMATION, NULL, "MHGU Recomp", text, 3, buttons, NULL};
    int pressed = 0;
    if (!SDL_ShowMessageBox(&box, &pressed)) return false;
    if (pressed == 1 && android_import(1, false)) {
        snprintf(data, size, "data");
        return true;
    }
    if (pressed == 2 && android_import(7, false)) {
        char folder[sizeof pending_folder];
        if (!android_take_picked(folder, sizeof folder)) return false;
        snprintf(data, size, "%s", folder);
        set_game_folder(folder);
        return true;
    }
    // (cancelled in the picker: ask again rather than leave)
    return pressed == 1 || pressed == 2;
}
