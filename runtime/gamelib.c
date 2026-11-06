// Loads the recompiled game code library (runtime/gamelib.h).
#include "gamelib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "rt.h"

static const GameLibTables *g_tables;

// Can this CPU run the game code? (needs: the library's GAMELIB_CPU_NEEDS bits.) If not, the player is told so, not
// sent to a crash log: the first of its instructions this CPU lacks would be an illegal-instruction crash.
static void check_cpu(const char *path, const uint32_t *needs) {
#if defined(__x86_64__) || defined(_M_X64)
    if (!needs || !*needs) return;  // (a library from before the check, or built for the baseline)
    static const struct { uint32_t bit; const char *name; } feat[] = {
        {GAMELIB_CPU_SSE42, "SSE4.2"}, {GAMELIB_CPU_POPCNT, "POPCNT"}, {GAMELIB_CPU_AVX, "AVX"},
        {GAMELIB_CPU_AVX2, "AVX2"},    {GAMELIB_CPU_FMA, "FMA"},       {GAMELIB_CPU_BMI, "BMI1"},
        {GAMELIB_CPU_BMI2, "BMI2"},    {GAMELIB_CPU_F16C, "F16C"},     {GAMELIB_CPU_AVX512, "AVX-512"},
    };
    __builtin_cpu_init();
    uint32_t have = (__builtin_cpu_supports("sse4.2") ? GAMELIB_CPU_SSE42 : 0) |
                    (__builtin_cpu_supports("popcnt") ? GAMELIB_CPU_POPCNT : 0) |
                    (__builtin_cpu_supports("avx") ? GAMELIB_CPU_AVX : 0) |
                    (__builtin_cpu_supports("avx2") ? GAMELIB_CPU_AVX2 : 0) |
                    (__builtin_cpu_supports("fma") ? GAMELIB_CPU_FMA : 0) |
                    (__builtin_cpu_supports("bmi") ? GAMELIB_CPU_BMI : 0) |
                    (__builtin_cpu_supports("bmi2") ? GAMELIB_CPU_BMI2 : 0) |
                    (__builtin_cpu_supports("f16c") ? GAMELIB_CPU_F16C : 0) |
                    (__builtin_cpu_supports("avx512f") ? GAMELIB_CPU_AVX512 : 0);
    uint32_t missing = *needs & ~have;
    if (!missing) return;
    char list[128] = "";
    for (size_t i = 0; i < sizeof feat / sizeof *feat; i++)
        if (missing & feat[i].bit)
            snprintf(list + strlen(list), sizeof list - strlen(list), "%s%s", list[0] ? ", " : "", feat[i].name);
    LOG("[gamelib] %s needs CPU features this CPU lacks: %s\n", path, list);
    char msg[600];
    if (missing & GAMELIB_CPU_AVX2)
        snprintf(msg, sizeof msg, "This PC's processor does not support AVX2, which the game needs (missing: %s). "
                 "Processors from about 2014 on have it: Intel 4th generation Core (Haswell) or newer, AMD Ryzen "
                 "or newer. The game can't run on this processor.", list);
    else
        snprintf(msg, sizeof msg, "This PC's processor lacks features the game was built for (missing: %s). The game "
                 "can't run on this processor.", list);
    plat_error_box(msg);
    exit(1);
#else
    (void)path, (void)needs;
#endif
}

bool gamelib_load(char *why, size_t why_size) {
    if (g_tables) return true;
    char path[1100];
    const char *env = getenv("MHGU_GAME_LIB");
    if (env && *env) {
        snprintf(path, sizeof path, "%s", env);
    } else {
#ifdef __ANDROID__
        snprintf(path, sizeof path, "%s", GAMELIB_NAME);  // (by name: the app's own libraries are on its linker's path)
#else
        char exe[1024];
        if (!plat_exe_path(exe, sizeof exe)) exe[0] = 0;
        char *slash = strrchr(exe, '/'), *bs = strrchr(exe, '\\');
        if (bs > slash) slash = bs;
        if (slash) slash[1] = 0; else exe[0] = 0;
        snprintf(path, sizeof path, "%s%s", exe, GAMELIB_NAME);
#endif
    }
#ifdef _WIN32
    HMODULE h = LoadLibraryA(path);
    if (!h) { snprintf(why, why_size, "can't load %s (error %lu): run `make gen` and `make` first", path, GetLastError()); return false; }
    check_cpu(path, (const uint32_t *)(void *)GetProcAddress(h, GAMELIB_CPU_NEEDS));
    GameLibEntry entry = (GameLibEntry)(void *)GetProcAddress(h, GAMELIB_ENTRY);
#else
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { snprintf(why, why_size, "%s", dlerror()); return false; }
    check_cpu(path, (const uint32_t *)dlsym(h, GAMELIB_CPU_NEEDS));
    GameLibEntry entry = (GameLibEntry)dlsym(h, GAMELIB_ENTRY);
#endif
    if (!entry) { snprintf(why, why_size, "%s: not game code", path); return false; }
    const GameLibTables *t = entry();
    if (!t || t->abi != GAMELIB_ABI || !t->id || strcmp(t->id, GAMELIB_ID)) {
        snprintf(why, why_size, "%s was built for another version of the runtime: rebuild it (make)", path);
        return false;
    }
    g_tables = t;
    LOG("[gamelib] %s (%u + %u functions)\n", path, *t->main_count, *t->sdk_count);
    return true;
}

const GameLibTables *gamelib_tables(void) { return g_tables; }
