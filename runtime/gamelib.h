// The recompiled game code: a library built from the player's own game files (`make gen` + `make`) and loaded at
// startup (runtime/gamelib.c). Nothing from the game ships with the app.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cpu.h"

#if defined(_WIN32)
#define GAMELIB_NAME "mhgugame.dll"
#define GAMELIB_EXPORT __declspec(dllexport)
#elif defined(__ANDROID__)
#define GAMELIB_NAME "libmhgugame.so"  // (in the APK's lib/<abi>/, which only takes lib*.so)
#define GAMELIB_EXPORT __attribute__((visibility("default")))
#else
#define GAMELIB_NAME "mhgugame.so"
#define GAMELIB_EXPORT __attribute__((visibility("default")))
#endif

#ifndef GAMELIB_ID
#define GAMELIB_ID "dev"
#endif
#define GAMELIB_ABI 1

typedef struct {
    uint32_t abi;
    const char *id;
    const uint32_t *main_addrs, *sdk_addrs;  // sorted guest addresses
    const GuestFn *main_fns, *sdk_fns;       // the functions at them
    const uint32_t *main_count, *sdk_count;
} GameLibTables;

typedef const GameLibTables *(*GameLibEntry)(void);
#define GAMELIB_ENTRY "mhgugame_tables"

// The x86 features the game code was compiled to use (GENARCH: the release's x86-64-v3 is AVX2 + FMA), as data the
// library exports (a uint32_t of these bits), so the app can check the CPU before it runs any of that code: on an
// older CPU (Ivy Bridge: AVX, no AVX2) its first instruction was an illegal-instruction crash.
#define GAMELIB_CPU_NEEDS "mhgugame_cpu_needs"
#define GAMELIB_CPU_SSE42  (1u << 0)
#define GAMELIB_CPU_POPCNT (1u << 1)
#define GAMELIB_CPU_AVX    (1u << 2)
#define GAMELIB_CPU_AVX2   (1u << 3)
#define GAMELIB_CPU_FMA    (1u << 4)
#define GAMELIB_CPU_BMI    (1u << 5)
#define GAMELIB_CPU_BMI2   (1u << 6)
#define GAMELIB_CPU_F16C   (1u << 7)
#define GAMELIB_CPU_AVX512 (1u << 8)  // (AVX-512 F)

#ifdef __cplusplus
extern "C" {
#endif
// Loads the game code library (next to the executable, or MHGU_GAME_LIB). false: why says what's wrong.
bool gamelib_load(char *why, size_t why_size);
const GameLibTables *gamelib_tables(void);
#ifdef __cplusplus
}
#endif
