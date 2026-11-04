// The cheats loaded from cheats/ (runtime/cheats.c), for the settings' Cheats tab: read at start-up and again by
// cheats_rescan when the settings menu opens (on its thread; while the menu is open it only grows, by cheats_add).
// cheats_set turns them on / off while the game runs (any thread: it takes effect the next frame) and keeps that in
// cheats/_off.txt. Master codes are always on. cheats_add (the settings' thread) checks a pasted cheat, adds it to
// cheats/added.txt and runs it from the next frame.
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
bool cheats_add(const char *text, char *err, size_t errsz);
void cheats_rescan(void);  // the cheats folder read again: new, changed and removed cheat files (the menu, as it opens)
#ifdef __cplusplus
}
#endif
