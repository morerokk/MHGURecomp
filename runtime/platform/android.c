// Android host bits beside runtime/platform/linux.c (the Makefile's PLATFORM=android; docs/research/android.md).
//
// The app's folder is its external files folder, /sdcard/Android/data/<package>/files: the player (or adb push) puts
// the game's exefs / romfs in its data/ there, and the settings, saves, caches, mods, cheats and log.txt go beside
// them, as in the PC build's game folder. Android gives no app an environment to start with, so the debug variables
// (MHGU_QUICKHUNT, MHGU_NO_BC...) come from env.txt in that folder: one NAME=value per line, # for comments.
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <SDL3/SDL.h>
#include <jni.h>

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

void android_error_box(const char *msg) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "MHGU Recomp", msg, NULL);
}

// ------------------------------------------------------------------ importing the game files or a save
// The system's file picker and the copy are Java's (android/java: ImportActivity.java), which has its own dialogs;
// this waits until it is done. 1: game files (into the data folder), 2: a save (into save/save_0). running: the game
// has started (the app closes after the import). true when the files were copied in.

static const char *absolute(const char *dir, char *buf, size_t size) {
    if (dir[0] == '/') return dir;
    snprintf(buf, size, "%s/%s", android_home(), dir);
    return buf;
}

bool android_import(int what, bool running) {
    char buf[1100], folder[1200];
    if (what == 1) snprintf(folder, sizeof folder, "%s", absolute(g_game_dir, buf, sizeof buf));
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
    int result = 3;
    if ((*env)->ExceptionCheck(env) || !begin || !state) {
        (*env)->ExceptionClear(env);
        LOG("[android] the import isn't in this APK\n");
    } else {
        LOG("[android] importing %s into %s\n", what == 1 ? "the game files" : "a save", folder);
        jstring dir = (*env)->NewStringUTF(env, folder);
        (*env)->CallStaticVoidMethod(env, c, begin, act, (jint)what, dir, (jboolean)running);
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        else while ((result = (*env)->CallStaticIntMethod(env, c, state)) == 0) SDL_Delay(100);
        (*env)->DeleteLocalRef(env, dir);
        LOG("[android] import %s\n", result == 1 ? "done" : result == 2 ? "cancelled" : "failed");
    }
    (*env)->DeleteLocalRef(env, name);
    if (c) (*env)->DeleteLocalRef(env, c);
    (*env)->DeleteLocalRef(env, lc);
    if (loader) (*env)->DeleteLocalRef(env, loader);
    (*env)->DeleteLocalRef(env, ac);
    (*env)->DeleteLocalRef(env, act);
    return result == 1;
}

// At the start, the game files missing or not right (msg): offers to import them. true: imported, check again.
bool android_offer_import(const char *msg) {
    char text[1200];
    snprintf(text, sizeof text, "%s\n\nThe app can copy them in from a folder on this device (for example Download): "
             "pick the folder that holds the exefs and romfs folders.", msg);
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Quit"},
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Import game files"},
    };
    const SDL_MessageBoxData box = {SDL_MESSAGEBOX_INFORMATION, NULL, "MHGU Recomp", text, 2, buttons, NULL};
    int pressed = 0;
    return SDL_ShowMessageBox(&box, &pressed) && pressed == 1 && android_import(1, false);
}
