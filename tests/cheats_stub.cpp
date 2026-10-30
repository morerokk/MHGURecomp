// A stand-in for cheats.c so test-settings-panel links without the rest of the runtime (as modorder.c does for the
// Mods tab). Three cheats: one on, one off, one a {master code}.
#include "cheats_ui.h"
static bool on[3] = {true, false, true};
extern "C" {
int cheats_count(void) { return 3; }
const char *cheats_name(int i) { static const char *n[] = {"Infinite stamina", "Max sharpness", "Always on"}; return n[i]; }
const char *cheats_file(int i) { static const char *f[] = {"stamina.txt", "_sharpness.txt", "master.txt"}; return f[i]; }
bool cheats_master(int i) { return i == 2; }
bool cheats_enabled(int i) { return on[i]; }
bool cheats_is_patch(int) { return false; }
bool cheats_set_enabled(int i, bool v) { on[i] = v; return v; }
void cheats_disable_all(void) { for (bool &b : on) b = false; }
}
