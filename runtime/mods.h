// Mods: folders under mods/ (beside mhgurecomp.exe in the standalone build, else in the current folder;
// MHGU_MODS_DIR overrides). Dropping a mod's folder in installs it, removing the folder uninstalls it: everything is
// layered over the game's own files at start, nothing is changed on disk. A mod folder may hold:
//   romfs/   loose files layered over the game's RomFS (same paths as in the game's romfs/ folder; a bare nativeNX/
//            folder counts as romfs/nativeNX/)
//   exefs/   IPSwitch patches (*.pchtxt) and IPS / IPS32 patches (<build id>.ips) for the game's code modules, and
//            code modules of the mod's own (subsdk0..subsdk9 NSOs, as Atmosphere and the emulators load them). The
//            port runs the game's code compiled ahead of time (tools/gen.py), which never reads its instructions:
//            every function a patch changes runs in the interpreter instead (from the patched code in memory, as a
//            cheat's), and so does a subsdk's code (runtime/loader.c places it; mods_patch_code).
// These folders are found up to three levels down, so a mod unpacked as Mod/Mod/romfs or with an emulator or
// Atmosphere layout (atmosphere/contents/0100770008DD8000/romfs) works as it is.
// Mods load in name order; a file in a later mod wins. mods/load_order.txt (one folder name per line, '#'
// comments) picks and orders them instead; a folder whose name starts with '_' is skipped. A list the Mods tab of the
// settings menu wrote also loads a folder it doesn't mention (a mod added since), after the listed ones.
#pragma once
#include <stdint.h>

struct Module;

#include "modorder.h"

void mods_make_dir(void);  // creates an empty mods/ (standalone: shows players where mods go)
int mods_romfs_layers(const char **out, int max);  // host folders to layer over the RomFS, in order
void mods_patch_module(struct Module *m, const uint8_t build_id[32]);  // exefs patches for a loaded module
void mods_patch_code(void);  // then the patches of main's and sdk's code: once the dispatch tables are set up
// The mods' exefs/<name> (a subsdk): its path in path, and the mod's name (the last mod that has one); NULL: none.
// skip: a file to pass over (and its mod's name), else NULL.
const char *mods_exefs_file(const char *name, char *path, size_t n, bool (*skip)(const char *path, const char *mod));
void forge_init(void);  // Forge plugins from the mods' RomFS (runtime/forge.c): before the game starts
void hpscale_init(void);  // large monster HP by the number of hunters (runtime/hle/hpscale.c)
void villagehub_init(void);  // Village quests at the Hub / Pub counter (runtime/hle/villagehub.c)
void mashrate_init(void);  // mashing out of a stun at the Switch's rate at high frame rates (runtime/hle/mashrate.c)
void inflate_init(void);  // zlib's inflate_fast as host code: archives decompress faster (runtime/hle/inflate.c)
void movelog_init(void);  // (debug) MHGU_LOG_MOVES: the player's command table changes (runtime/hle/movelog.c)
void quickhunt_init(void);  // (debug) MHGU_QUICKHUNT: start-up straight into a quest (runtime/hle/quickhunt.c)
void emtrace_init(void);  // (debug) MHGU_EMTRACE: large monster actions and their durations (runtime/hle/emtrace.c)
void kotrace_init(void);  // (debug) MHGU_KOTRACE: large monster KO (stun) events (runtime/hle/kotrace.c)

// Cheat codes (Atmosphere's format) from the .txt files in cheats/, beside mods/ (runtime/cheats.c)
const char *cheats_dir(void);
void cheats_make_dir(void);  // creates an empty cheats/ (standalone: shows players where cheats go)
void cheats_init(void);      // loads them and runs them once: before the game starts
void cheats_frame(void);     // runs them for a frame the game presented (the window loop, each pass)
