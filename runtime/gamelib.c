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

// The library being opened, for the crash handler (platform/linux.c): a SIGBUS in the loader then is a file it couldn't
// read (cut short, or a read error from the drive), told as such instead of as a crash.
const char *volatile g_gamelib_loading;

// The player is told the library is damaged and to unpack the release again; the game ends.
static void damaged(const char *path, const char *detail) {
    const char *name = strrchr(path, '/'), *bs = strrchr(path, '\\');
    if (bs > name) name = bs;
    name = name ? name + 1 : path;
    LOG("[gamelib] %s is damaged: %s\n", path, detail);
    char msg[700];
    snprintf(msg, sizeof msg, "The game's file %s is damaged (%s), so the game can't start. Download the release "
             "again and unpack it over the game folder.", name, detail);
    plat_error_box(msg);
    exit(1);
}

static uint64_t le(const unsigned char *p, int n) {
    uint64_t v = 0;
    for (int i = n - 1; i >= 0; i--) v = v << 8 | p[i];
    return v;
}

// Is the whole library there? Its headers say how far its contents reach (ELF: the program segments and the section
// header table; PE: the sections' raw data); a file shorter than that was cut short (an interrupted unpack or copy, a
// full disk) and would crash the loader (a 0.18.0 Steam Deck report: SIGBUS in ld.so). Only the headers are read.
// A file that isn't there, or isn't ELF / PE, is left to the loader's own error.
static void check_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    enum { HEAD = 65536 };
    unsigned char *h = calloc(1, HEAD);
    long long size = -1;
    size_t got = 0;
    if (h && fseek(f, 0, SEEK_END) == 0) size = ftell(f);
    if (h && size >= 0 && fseek(f, 0, SEEK_SET) == 0) got = fread(h, 1, HEAD, f);
    fclose(f);
    if (!h || size < 0) { free(h); return; }
    uint64_t need = 0;
    bool known = false;
    if (got >= 0x40 && !memcmp(h, "\x7f" "ELF", 4) && h[4] == 2 && h[5] == 1) {  // (ELF64, little-endian)
        known = true;
        uint64_t phoff = le(h + 0x20, 8), shoff = le(h + 0x28, 8);
        unsigned phentsize = (unsigned)le(h + 0x36, 2), phnum = (unsigned)le(h + 0x38, 2);
        unsigned shentsize = (unsigned)le(h + 0x3a, 2), shnum = (unsigned)le(h + 0x3c, 2);
        if (shoff) need = shoff + (uint64_t)shentsize * shnum;
        if (phentsize >= 0x28 && phoff + (uint64_t)phentsize * phnum > got) need = UINT64_MAX;  // (headers cut off)
        else
            for (unsigned i = 0; phentsize >= 0x28 && i < phnum; i++) {
                const unsigned char *ph = h + phoff + (uint64_t)phentsize * i;
                uint64_t end = le(ph + 8, 8) + le(ph + 0x20, 8);  // (p_offset + p_filesz)
                if (le(ph + 0x20, 8) && end > need) need = end;
            }
    } else if (got >= 0x40 && h[0] == 'M' && h[1] == 'Z') {
        known = true;
        uint64_t pe = le(h + 0x3c, 4);
        if (pe + 24 > got || memcmp(h + pe, "PE\0\0", 4)) need = UINT64_MAX;
        else {
            unsigned nsec = (unsigned)le(h + pe + 6, 2), optsize = (unsigned)le(h + pe + 20, 2);
            uint64_t sec = pe + 24 + optsize;
            if (sec + 40ull * nsec > got) need = UINT64_MAX;
            else
                for (unsigned i = 0; i < nsec; i++) {
                    const unsigned char *s = h + sec + 40ull * i;
                    uint64_t end = le(s + 20, 4) + le(s + 16, 4);  // (PointerToRawData + SizeOfRawData)
                    if (le(s + 16, 4) && end > need) need = end;
                }
        }
    } else if (got < 0x40) {
        known = true, need = UINT64_MAX;  // (too short to be a library at all)
    }
    free(h);
    if (!known || (uint64_t)size >= need) return;
    char detail[160];
    if (need == UINT64_MAX) snprintf(detail, sizeof detail, "only %lld bytes, its headers cut off", size);
    else snprintf(detail, sizeof detail, "%lld bytes, %llu expected", size, (unsigned long long)need);
    damaged(path, detail);
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
#ifndef __ANDROID__  // (Android: in the APK, which the system checked when it installed it)
    check_file(path);
#endif
#ifdef _WIN32
    HMODULE h = LoadLibraryA(path);
    if (!h) { snprintf(why, why_size, "can't load %s (error %lu): run `make gen` and `make` first", path, GetLastError()); return false; }
    check_cpu(path, (const uint32_t *)(void *)GetProcAddress(h, GAMELIB_CPU_NEEDS));
    GameLibEntry entry = (GameLibEntry)(void *)GetProcAddress(h, GAMELIB_ENTRY);
#else
    g_gamelib_loading = path;
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    g_gamelib_loading = NULL;
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
