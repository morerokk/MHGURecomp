// MHGU Recomp entry: claim the guest window, load the modules, set up the kernel, run nn::init::Start on the
// main thread.
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "camera_jitter.h"
#include "gamelib.h"
#include "hle.h"
#include "kernel.h"
#include "mods.h"
#include "app_actions.h"

#ifndef MHGU_VERSION
#define MHGU_VERSION "unknown"  // (the Makefile gives the build's release: VERSION)
#endif

const char *g_game_dir = "game";  // folder holding exefs/ and romfs/ (the player's own dump)

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
    movelog_init();  // (debug, MHGU_LOG_MOVES: runtime/hle/movelog.c)
    quickhunt_init();  // (debug, MHGU_QUICKHUNT: runtime/hle/quickhunt.c)
    camjit_init();  // (FSR: the main camera and its jitter: runtime/hle/camera_jitter.c)
    emtrace_init();  // (debug, MHGU_EMTRACE: runtime/hle/emtrace.c)
    kotrace_init();  // (debug, MHGU_KOTRACE: runtime/hle/kotrace.c)
    cheats_init();  // (cheat codes from cheats/: runtime/cheats.c)

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
    for (int i = 1; i < argc; i++)
        if (argv[i][0] != '-') g_game_dir = argv[i], given = true;
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
        g_game_dir = data;
        mods_make_dir();
        cheats_make_dir();
        static char save[1024];
        if (plat_user_dir(save, sizeof save - 8)) {
            strcat(save, "/save");
            g_save_dir = save;
        } else {
            fprintf(stderr, "[main] no user data folder: saves go to save\\ beside the executable\n");
        }
        char main_path[1200];
        snprintf(main_path, sizeof main_path, "%s/exefs/main", data);
        FILE *f = fopen(main_path, "rb");
        if (!f && strcmp(data, "data")) {  // (or in MHGU_HOME's data\: a macOS release app has no game files inside)
            snprintf(data, sizeof data, "data");
            snprintf(main_path, sizeof main_path, "%s/exefs/main", data);
            f = fopen(main_path, "rb");
        }
        if (!f) {
            const char *msg = "The game files were not found. Put your own dump's exefs and romfs folders in the data "
                              "folder next to mhgurecomp.exe (data\\exefs, data\\romfs).";
#ifdef __APPLE__
            if (plat_in_app_bundle()) {  // (the player's folder is MHGU_HOME: open its data folder for them)
                msg = "The game files were not found. Put your own dump's exefs and romfs folders in the data folder "
                      "that just opened (~/Library/Application Support/MHGURecomp/data).";
                mkdir("data", 0755);
                if (system("open data") != 0) { /* the message says where it is */ }
            } else {
                msg = "The game files were not found. Put your own dump's exefs and romfs folders in the data folder "
                      "next to mhgurecomp (data/exefs, data/romfs).";
            }
#endif
            fprintf(stderr, "%s\n", msg);
            plat_error_box(msg);
            return 1;
        }
        // the recompiled code is v1.4.0's: another version's files don't fit it (NSO header +0x40: build id)
        static const uint8_t want[8] = {0xfb, 0x08, 0xf1, 0xd2, 0x0f, 0xd1, 0x20, 0x4f};
        uint8_t hdr[0x48] = {0};
        bool ok = fread(hdr, 1, sizeof hdr, f) == sizeof hdr && !memcmp(hdr, "NSO0", 4) && !memcmp(hdr + 0x40, want, 8);
        fclose(f);
        if (!ok) {
            const char *msg = "The game files in the data folder are not version 1.4.0 of Monster Hunter Generations "
#ifdef __APPLE__
                              "Ultimate (data/exefs/main, build id FB08F1D20FD1204F). Dump the game with its 1.4.0 "
#else
                              "Ultimate (data\\exefs\\main, build id FB08F1D20FD1204F). Dump the game with its 1.4.0 "
#endif
                              "update applied (exefs from the update, romfs merged).";
            fprintf(stderr, "%s\n", msg);
            plat_error_box(msg);
            return 1;
        }
        // an incomplete romfs starts, then crashes or quits with the game's own error: players' dumps of the base game
        // without the update merged (34149 files) or of the update alone (12 files, no shader package) did. 1.4.0
        // merged has 35531; the margin is for dump tools that leave out a few. MHGU_ROMFS_ANY=1 skips the check.
        char romfs[1200], shaders[1300];
        snprintf(romfs, sizeof romfs, "%s/romfs", data);
        snprintf(shaders, sizeof shaders, "%s/nativeNX/system/app_shader/AppShaderPackage.mfx", romfs);
        struct stat st;
        int nfiles = getenv("MHGU_ROMFS_ANY") ? -1 : plat_count_files(romfs);
        bool have_shaders = stat(shaders, &st) == 0;
        if (nfiles >= 0 && (nfiles < 35000 || !have_shaders)) {
            char msg[700];
#ifdef __APPLE__
            snprintf(msg, sizeof msg, "The game files in data/romfs are incomplete: %d files, where Monster Hunter "
                     "Generations Ultimate 1.4.0 has 35531%s. Extract the romfs again with the 1.4.0 update applied (the "
                     "base game's files with the update's merged over them) and replace data/romfs with it.", nfiles,
                     !have_shaders ? ", and nativeNX/system/app_shader/AppShaderPackage.mfx is missing" : "");
#else
            snprintf(msg, sizeof msg, "The game files in data\\romfs are incomplete: %d files, where Monster Hunter "
                     "Generations Ultimate 1.4.0 has 35531%s. Extract the romfs again with the 1.4.0 update applied (the "
                     "base game's files with the update's merged over them) and replace data\\romfs with it.", nfiles,
                     !have_shaders ? ", and nativeNX\\system\\app_shader\\AppShaderPackage.mfx is missing" : "");
#endif
            fprintf(stderr, "%s\n", msg);
            plat_error_box(msg);
            return 1;
        }
#ifdef __APPLE__
        // a release app carries no game code: it builds it from these files on the player's Mac, once
        if (plat_in_app_bundle() && !plat_gamecode_ready(data)) return 1;
#endif
    }
    LOG("MHGU Recomp %s\n", MHGU_VERSION);
    { void fnprof_init(void); fnprof_init(); }
    crash_handler_install();
    ui_init();
    boot(g_game_dir);
    return ui_run();
}
