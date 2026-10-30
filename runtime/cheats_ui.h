// What the settings menu (settings_panel.cpp, the Cheats tab) uses of cheats.c.
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif
int cheats_count(void);
const char *cheats_name(int i);          // the name between [brackets]
const char *cheats_file(int i);          // the file it comes from (a name starting with '_' is off at the start)
bool cheats_master(int i);               // a {master code}
bool cheats_enabled(int i);
bool cheats_is_patch(int i);             // only writes fixed values (often the game's code: it runs in the interpreter)
bool cheats_set_enabled(int i, bool on); // at once; undoes its changes in memory when switched off. Returns the state.
void cheats_disable_all(void);
#ifdef __cplusplus
}
#endif
