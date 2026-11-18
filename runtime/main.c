// MHGU Recomp entry: claim the guest window, load the modules, set up the kernel, run nn::init::Start on the
// main thread.
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <strings.h>

#include "camera_jitter.h"
#include "gamelib.h"
#include "hle.h"
#include "hle/itembar.h"
#include "hle/mousecam.h"
#include "kernel.h"
#include "mods.h"
#include "engine_module.h"
#include "app_actions.h"
#include "net/discord.h"
#ifndef __ANDROID__
#include "nspimport.h"
#endif
#ifdef __ANDROID__
#include <SDL3/SDL_main.h>  // (main becomes SDL_main, which SDL's Java activity calls in libmain.so)
#endif

#ifndef MHGU_VERSION
#define MHGU_VERSION "unknown"  // (the Makefile gives the build's release: VERSION)
#endif

const char *g_game_dir = "game";  // folder holding exefs/ and romfs/ (the player's own dump)

// where the player puts the game files, for the messages below
#ifdef __ANDROID__
#define DATA_FOLDER "Android/data/io.github.morerokk.mhgurecomp/files/data on this device"
#define DATA_SUB(s) "data/" s
#else
#define DATA_FOLDER "the data folder next to mhgurecomp.exe"
#define DATA_SUB(s) "data\\" s
#endif

// rtld normally runs every module's DT_INIT (which registers it with musl and runs its init_array) before
// nnMain: nn::init::Start calls its third argument right after nnosInitialize (SDK constructors run early) and its
// fourth after nninitStartup has set up the game's heap.
static void module_init(Cpu *c, Module *m) {
    LOG("init %s (DT_INIT %x, %u init_array entries)\n", m->name, m->init, m->init_array_sz / 4);
    if (m->init) guest_call(c, m->init);
    else
        for (uint32_t i = 0; i < m->init_array_sz / 4; i++) {
            uint32_t fn = LD32(m->init_array + 4 * i);
            if (fn) guest_call(c, fn);
        }
}
static void rtld_init_sdk(Cpu *c) { module_init(c, &g_modules[1]); }
static void rtld_init_main(Cpu *c) {  // (then the mods' subsdks, in the order rtld loads them: after main)
    module_init(c, &g_modules[0]);
    for (int i = 2; i < g_nexefs; i++) module_init(c, &g_modules[i]);
}

// Claim the guest window before anything else can map there.
__attribute__((constructor(101))) static void reserve_guest_space(void) {
    if (plat_reserve_guest(GUEST_BASE, GUEST_SPACE)) return;
    fprintf(stderr, "FATAL: the guest address window at %llx is not free\n", GUEST_BASE);
    exit(1);
}

static void boot(const char *root) {
    char exefs[1024], npdm_path[1024];
    snprintf(exefs, sizeof exefs, "%s/exefs", root);
    snprintf(npdm_path, sizeof npdm_path, "%s/main.npdm", exefs);

    char why[512];
    if (!gamelib_load(why, sizeof why)) FATAL("%s\n", why);
    loader_load(exefs);
    kernel_init();
    dispatch_init();
    mods_patch_code();  // (the mods' patches of the game's code: runtime/mods.c)
    forge_init();  // (Forge plugins in the mods: runtime/forge.c)
    hpscale_init();  // (large monster HP by the number of hunters: runtime/hle/hpscale.c)
    villagehub_init();  // (Village quests at the Hub / Pub counter: runtime/hle/villagehub.c)
    rewardbox_init();  // (quest rewards: Send to the Item Box in one go: runtime/hle/rewardbox.c)
    urgentcredit_init();  // (Urgent quests count for all who clear them: runtime/hle/urgentcredit.c)
    mashrate_init();  // (mashing out of a stun at high frame rates: runtime/hle/mashrate.c)
    hatestep_init();  // (monsters' attention scores at high frame rates: runtime/hle/hatestep.c)
    inflate_init();  // (zlib's inflate_fast as host code, for loading: runtime/hle/inflate.c)
    fastload_init();  // (shorter transitions when not in a network session: runtime/hle/fastload.c)
    movelog_init();  // (debug, MHGU_LOG_MOVES: runtime/hle/movelog.c)
    quickhunt_init();  // (debug, MHGU_QUICKHUNT: runtime/hle/quickhunt.c)
    camjit_init();  // (FSR: the main camera and its jitter: runtime/hle/camera_jitter.c)
    mousecam_init();  // (mouse look on the hunt camera's angles: runtime/hle/mousecam.c)
    itembar_init();  // (the mouse wheel on the item bar: runtime/hle/itembar.c)
    { void menuptr_init(void); menuptr_init(); }  // (the mouse in the pause menu: runtime/hle/menuptr.c)
    { void prompts_init(void); prompts_init(); }  // (keyboard and mouse button prompts: runtime/hle/prompts.c)
    { void input_view_init(void); input_view_init(); }  // (game-action rebinding, with [Actions] Enable=1: runtime/hle/input_view.c)
    { void title_exit_init(void); title_exit_init(); }  // (B on the title screen's Main Menu asks to quit: runtime/hle/title_exit.c)
    { void textlayout_init(void); textlayout_init(); }  // (UI text spacing and kerning: runtime/hle/textlayout.c)
    { void chat_init(void); chat_init(); }  // (typed chat: a message's rows under one name: runtime/hle/chat.c)
    emtrace_init();  // (debug, MHGU_EMTRACE: runtime/hle/emtrace.c)
    { void camtrace_init(void); camtrace_init(); }  // (debug, MHGU_CAMTRACE: hunt camera takeovers: runtime/hle/camtrace.c)
    kotrace_init();  // (debug, MHGU_KOTRACE: runtime/hle/kotrace.c)
    dummy_init();  // (debug, MHGU_DUMMY: runtime/hle/dummy.c)
    hitsync_init();  // (multiplayer: hits as their hunter saw them: runtime/net/hitsync.c)
    partsync_init();  // (multiplayer: part breaks reach the other games at once: runtime/net/partsync.c)
    netrate_init();  // (debug, MHGU_NETRATE: runtime/hle/netrate.c)
    cheats_init();  // (cheat codes from cheats/: runtime/cheats.c)
    discord_init();  // (Discord Rich Presence: runtime/net/discord.c, runtime/hle/presence.c)
    { void sampler_init(void); sampler_init(); }  // (debug, Windows, MHGU_SAMPLE: runtime/sampler.c)

    uint32_t stack_size = 0x100000;
    int prio = 44;
    FILE *f = fopen(npdm_path, "rb");
    if (f) {
        uint8_t meta[0x80];
        if (fread(meta, 1, sizeof meta, f) == sizeof meta && !memcmp(meta, "META", 4)) {
            prio = meta[0xE];
            stack_size = *(uint32_t *)(meta + 0x1C);
        }
        fclose(f);
    }
    LOG("main thread: prio %d, stack 0x%x\n", prio, stack_size);

    uint32_t start = loader_find_symbol("_ZN2nn4init5StartEjjPFvvES2_");
    if (!start) FATAL("nn::init::Start not found\n");
    uint32_t init_fn = hle_register("<rtld_init_sdk>", rtld_init_sdk);
    uint32_t main_init_fn = hle_register("<rtld_init_main>", rtld_init_main);
    KThread *t = kernel_create_main_thread(start, stack_size, prio);
    kernel_run_main(t, t->handle, 0, init_fn, main_init_fn);
}

// base/rel as it is spelled on disk, into found (rel's own spelling where it matches): 0 rel is there with those exact
// capitals, 1 it is there with other capitals (found: how; Windows' stat finds it either way, the game would not), -1
// not there. first_any: the first part in any case (romfs.c uses the top folder whatever its spelling).
static int spelled_on_disk(const char *base, const char *rel, bool first_any, char *found, size_t size) {
    char dir[1600], part[256];
    snprintf(dir, sizeof dir, "%s", base);
    found[0] = 0;
    int result = 0;
    for (const char *p = rel; *p;) {
        size_t n = strcspn(p, "/");
        snprintf(part, sizeof part, "%.*s", (int)n, p);
        DIR *d = opendir(dir);
        if (!d) return -1;
        char hit[256] = "";
        for (struct dirent *e; (e = readdir(d));) {
            if (!strcmp(e->d_name, part)) { snprintf(hit, sizeof hit, "%s", e->d_name); break; }
            if (!hit[0] && !strcasecmp(e->d_name, part)) snprintf(hit, sizeof hit, "%s", e->d_name);
        }
        closedir(d);
        if (!hit[0]) return -1;
        if (strcmp(hit, part) && !(first_any && p == rel)) result = 1;
        size_t len = strlen(found);
        snprintf(found + len, size - len, "%s%s", len ? "/" : "", hit);
        len = strlen(dir);
        snprintf(dir + len, sizeof dir - len, "/%s", hit);
        p += n;
        if (*p == '/') p++;
    }
    return result;
}

// What is wrong with the game files in data (into msg), or false when they are fine.
static bool game_files_problem(const char *data, char *msg, size_t size) {
    char main_path[1200];
    snprintf(main_path, sizeof main_path, "%s/exefs/main", data);
    FILE *f = fopen(main_path, "rb");
    if (!f) {
        snprintf(msg, size, "The game files were not found. Put your own dump's exefs and romfs folders in "
                 DATA_FOLDER " (" DATA_SUB("exefs") ", " DATA_SUB("romfs") ")"
#ifndef __ANDROID__
                 ", or drag the base game's NSP and the 1.4.0 update's NSP onto mhgurecomp (or put both in the import folder "
                 "next to it) and they are extracted for you (needs your prod.keys)"
#endif
                 ".");
        return true;
    }
    // the recompiled code is v1.4.0's: another version's files don't fit it (NSO header +0x40: build id)
    static const uint8_t want[8] = {0xfb, 0x08, 0xf1, 0xd2, 0x0f, 0xd1, 0x20, 0x4f};
    uint8_t hdr[0x48] = {0};
    bool ok = fread(hdr, 1, sizeof hdr, f) == sizeof hdr && !memcmp(hdr, "NSO0", 4) && !memcmp(hdr + 0x40, want, 8);
    fclose(f);
    if (!ok) {
        snprintf(msg, size, "The game files in " DATA_FOLDER " are not version 1.4.0 of Monster Hunter Generations "
                 "Ultimate (exefs/main, build id FB08F1D20FD1204F). Dump the game with its 1.4.0 "
                 "update applied (exefs from the update, romfs merged).");
        return true;
    }
    // an incomplete romfs starts, then crashes or quits with the game's own error: players' dumps of the base game
    // without the update merged (34149 files) or of the update alone (12 files, no shader package) did. 1.4.0
    // merged has 35531; the margin is for dump tools that leave out a few. MHGU_ROMFS_ANY=1 skips the check.
    char romfs[1200];
    snprintf(romfs, sizeof romfs, "%s/romfs", data);
    int nfiles = getenv("MHGU_ROMFS_ANY") ? -1 : plat_count_files(romfs);
    // (spelled out, not stat: Linux's would miss a NativeNX folder, which romfs.c takes as nativeNX)
    char spelled[600];
    int case_check = spelled_on_disk(romfs, "nativeNX/system/app_shader/AppShaderPackage.mfx", true, spelled, sizeof spelled);
    bool have_shaders = case_check >= 0;
    // Names with other capitals than the game's (a dump tool that writes them all in small letters): on Windows they
    // pass the count, then the game can't find its first file and quits with "Shader compile failed" (#136)
    if (case_check == 1) {
        const char sep = DATA_SUB("")[4];  // (the folder separator the messages use: \\ or /)
        for (char *p = spelled; *p; p++) if (*p == '/') *p = sep;
        snprintf(msg, size, "The game files in " DATA_SUB("romfs") " have names with the wrong capital letters, so the "
                 "game can't find them: for example %s, which should be nativeNX%csystem%capp_shader%cAppShaderPackage.mfx. "
                 "Extract the romfs again with a tool that keeps the names as they are, and replace " DATA_SUB("romfs")
                 " with it.", spelled, sep, sep, sep);
        return true;
    }
    if (nfiles >= 0 && (nfiles < 35000 || !have_shaders)) {
        snprintf(msg, size, "The game files in " DATA_SUB("romfs") " are incomplete: %d files, where Monster Hunter "
                 "Generations Ultimate 1.4.0 has 35531%s. Extract the romfs again with the 1.4.0 update applied (the "
                 "base game's files with the update's merged over them) and replace " DATA_SUB("romfs") " with it.", nfiles,
                 !have_shaders ? ", and nativeNX\\system\\app_shader\\AppShaderPackage.mfx is missing" : "");
        return true;
    }
    return false;
}


#ifndef __ANDROID__
// ------------------------------------------------------------------ game files from NSP files (runtime/nspimport.c)
static bool is_nsp(const char *path) {
    size_t n = strlen(path);
    return n > 4 && !strcasecmp(path + n - 4, ".nsp");
}

// the update's exefs/main must be v1.4.0's (the same build id game_files_problem wants): checked before the long RomFS
static bool nsp_verify_exefs(const char *exefs_dir, char *err, size_t size) {
    char path[1200];
    snprintf(path, sizeof path, "%s/main", exefs_dir);
    static const uint8_t want[8] = {0xfb, 0x08, 0xf1, 0xd2, 0x0f, 0xd1, 0x20, 0x4f};
    uint8_t hdr[0x48] = {0};
    FILE *f = fopen(path, "rb");
    bool ok = f && fread(hdr, 1, sizeof hdr, f) == sizeof hdr && !memcmp(hdr, "NSO0", 4) && !memcmp(hdr + 0x40, want, 8);
    if (f) fclose(f);
    if (!ok) snprintf(err, size, "The update's NSP is not version 1.4.0 of Monster Hunter Generations Ultimate "
                      "(exefs/main build id FB08F1D20FD1204F). Use the 1.4.0 update's NSP, together with the base game's.");
    return ok;
}

static void nsp_progress(void *user, const char *what, uint64_t done, uint64_t total) {
    (void)user;
    if (!total) { fprintf(stderr, "[import] %s...\n", what); return; }
    fprintf(stderr, "\r[import] %s: %llu / %llu MB   ", what, (unsigned long long)(done >> 20), (unsigned long long)(total >> 20));
    if (done >= total) fprintf(stderr, "\n");
}

// The .nsp files in folder (up to max, full paths into list). Returns how many.
static int find_nsps(const char *folder, char list[][1100], int max) {
    DIR *d = opendir(folder);
    if (!d) return 0;
    int n = 0;
    for (struct dirent *e; (e = readdir(d)) && n < max;)
        if (is_nsp(e->d_name)) snprintf(list[n++], 1100, "%s/%s", folder, e->d_name);
    closedir(d);
    return n;
}

// Imports the NSP files into data (both the base game's and the update's). false: it said why (a message box too).
static bool import_nsps(const char *data, char *const *paths, int count) {
    NspImport job = {.nsp_paths = (const char *const *)paths, .nsp_count = count, .out_dir = data,
                     .progress = nsp_progress, .verify_exefs = nsp_verify_exefs};
    fprintf(stderr, "[import] Extracting the game from %d NSP files into %s (this takes a few minutes, and about 13 GB)\n", count, data);
    char err[1000];
    if (nsp_import(&job, err, sizeof err)) {
        fprintf(stderr, "[import] Done: the game files are in %s\n", data);
        return true;
    }
    char msg[1100];
    snprintf(msg, sizeof msg, "Importing the NSP files failed:\n\n%s", err);
    fprintf(stderr, "%s\n", msg);
    plat_error_box(msg);
    return false;
}
#endif

#ifdef __ANDROID__
bool android_offer_import(const char *msg, char *data, size_t size);  // (runtime/platform/android.c)
void android_game_folder(char *out, size_t size);
#endif

int main(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        if (!strncmp(argv[i], "--recomp-wait=", 14)) {
            char *end; unsigned long pid = strtoul(argv[i] + 14, &end, 10);
            if (!pid || *end || !plat_wait_process(pid)) return 1;
        }
    }
    ui_capture_launch_args(argc, argv);
    plat_init();
    setvbuf(stderr, NULL, _IONBF, 0);
    bool given = false;
#ifndef __ANDROID__
    char *nsp_args[8];  // (NSP files dropped on the program, or on its shortcut: argv has their paths)
    int n_nsp_args = 0;
#endif
    for (int i = 1; i < argc; i++) {
#ifndef __ANDROID__
        if (argv[i][0] != '-' && is_nsp(argv[i])) {
            if (n_nsp_args < 8) nsp_args[n_nsp_args++] = argv[i];
            continue;
        }
#endif
        if (argv[i][0] != '-') g_game_dir = argv[i], given = true;
    }
    if (getenv("MHGU_GAME_DIR")) g_game_dir = getenv("MHGU_GAME_DIR"), given = true;
    if (!given) {  // standalone (the build folder): data\ holds the game files; caches, mods and log.txt beside, saves
        plat_standalone();  // in the player's folder (%APPDATA%\MHGURecomp\save)
        static char data[1100] = "data";
        char exe[1024];
        if (getenv("MHGU_HOME") && plat_exe_path(exe, sizeof exe)) {  // (running elsewhere: the files beside the exe)
            char *s = strrchr(exe, '\\'), *s2 = strrchr(exe, '/');
            if (s2 > s) s = s2;
            if (s) *s = 0, snprintf(data, sizeof data, "%s/data", exe);
        }
#ifdef __ANDROID__
        android_game_folder(data, sizeof data);  // ([Android] GameFolder: the game files on a USB drive, an SD card...)
#endif
        // (MHGU_DATA_DIR: the game files elsewhere, also on a PC; on Android from env.txt)
        if (getenv("MHGU_DATA_DIR")) snprintf(data, sizeof data, "%s", getenv("MHGU_DATA_DIR"));
        g_game_dir = data;
        mods_make_dir();
        cheats_make_dir();
#ifndef __ANDROID__
        engine_modules_make_dir();
#endif
        static char save[1024];
        if (plat_user_dir(save, sizeof save - 8)) {
            strcat(save, "/save");
            g_save_dir = save;
        } else {
            fprintf(stderr, "[main] no user data folder: saves go to save\\ beside the executable\n");
        }
#ifndef __ANDROID__
        if (n_nsp_args) {  // NSP files given: they are the game files (the ones in data move to data/previous-N)
            if (!import_nsps(data, nsp_args, n_nsp_args)) return 1;
        } else {
            char first[900];
            static char found[4][1100];
            char *list[4];
            int n_found = 0;
            if (game_files_problem(data, first, sizeof first)) {  // (no game files yet: the NSP files in import\ will do)
                n_found = find_nsps("import", found, 4);
                for (int i = 0; i < n_found; i++) list[i] = found[i];
                if (n_found >= 2 && !import_nsps(data, list, n_found)) return 1;
            }
        }
#endif
        for (char msg[900]; game_files_problem(data, msg, sizeof msg);) {
            fprintf(stderr, "%s\n", msg);
#ifdef __ANDROID__
            if (android_offer_import(msg, data, sizeof data)) continue;  // (from a folder the player picked: check again)
#endif
            plat_error_box(msg);
            return 1;
        }
    }
    LOG("MHGU Recomp %s\n", MHGU_VERSION);
    { void fnprof_init(void); fnprof_init(); }
    crash_handler_install();
    ui_init();
#ifndef __ANDROID__
    engine_modules_init(MHGU_VERSION);  // (the ones engine_modules/enabled.txt lists: runtime/engine_module.c)
#endif
    {   // The renderer now rather than at the game's nvnDeviceInitialize, so the loading screen shows while the game
        // starts (Android: still there, this thread mustn't wait for a window the app may not have yet)
        void backend_init(void), backend_loading_screen(void), overlay_init_early(void);
        overlay_init_early();  // (its indicator: ImGui's font until Forge's config is read in boot)
#if !defined(__ANDROID__) && !defined(MHGU_NULL_BACKEND)
        backend_init();
#endif
        backend_loading_screen();
    }
    boot(g_game_dir);
    return ui_run();
}
