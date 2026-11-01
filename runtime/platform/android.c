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
