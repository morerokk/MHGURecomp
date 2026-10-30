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
#elif defined(__APPLE__)
#define GAMELIB_NAME "mhgugame.dylib"
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

#ifdef __cplusplus
extern "C" {
#endif
// Loads the game code library (next to the executable, or MHGU_GAME_LIB). false: why says what's wrong.
bool gamelib_load(char *why, size_t why_size);
const GameLibTables *gamelib_tables(void);
const char *gamelib_id(void);  // the id a game code library must have for this runtime
#ifdef __cplusplus
}
#endif
