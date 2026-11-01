// The cheats loaded from cheats/ (runtime/cheats.c), for the settings' Cheats tab: fixed once cheats_init ran.
// cheats_set turns them on / off while the game runs (any thread: it takes effect the next frame) and keeps that in
// cheats/_off.txt. Master codes are always on.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif
int cheats_count(void);
const char *cheats_name(int i);
const char *cheats_file(int i);
bool cheats_master(int i);
bool cheats_on(int i);
bool cheats_set(const bool *on, int n, char *err, size_t errsz);
#ifdef __cplusplus
}
#endif
