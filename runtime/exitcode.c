// The process's exit codes (exitcode.h, docs/EXIT-CODES.md), and whose code a crash happened in.
#include "exitcode.h"

#include <stdlib.h>
#include <string.h>

#include "engine_module.h"
#include "gamelib.h"
#include "interp.h"
#include "rt.h"

static volatile int g_status;
static __thread int t_blame;

const char *rt_exit_meaning(int code) {
    switch (code) {
    case EXIT_NORMAL: return "closed normally";
    case EXIT_CRASH_RECOMP: return "a crash in the port's own code";
    case EXIT_CRASH_GAME: return "a crash in the game's code";
    case EXIT_CRASH_FORGE: return "a crash in a Forge plugin";
    case EXIT_CRASH_SUBSDK: return "a crash in a mod's subsdk";
    case EXIT_CRASH_ENGINE: return "a crash in an engine module";
    case EXIT_CANT_START: return "can't start on this PC";
    case EXIT_RESTART: return "closed to restart";
    default: return "?";
    }
}

void rt_exit(int code) {
    g_status = code;
    LOG("[exit] code %d (%s)\n", code, rt_exit_meaning(code));
    exit(code);
}

int rt_exit_status(void) { return g_status; }

void rt_blame(int code) { t_blame = code; }
int rt_blamed(void) { return t_blame; }

int rt_blame_guest(uint32_t a, int fallback) {
    for (int i = 0; i < g_nmodules; i++) {
        const Module *m = &g_modules[i];
        if (a < m->base || a - m->base >= m->size) continue;
        return i < 2 ? EXIT_CRASH_GAME : i < g_nexefs ? EXIT_CRASH_SUBSDK : EXIT_CRASH_FORGE;
    }
    // (Forge's own: the plugins' room, its trampolines and stubs, the plugins that didn't fit there)
    if ((a >= PLUGIN_BASE && a < FORGE_END) || (a >= PLUGIN2_BASE && a < PLUGIN2_END)) return EXIT_CRASH_FORGE;
    return fallback;
}

uint32_t rt_host_fn_addr(uint64_t pc);  // runtime/dispatch.c

int rt_blame_host(uint64_t pc) {
    // Interpreting guest code (a mod's subsdk, a Forge plugin, a game function a patch or a hook changed): its owner.
    // (0 once the interpreter calls out to recompiled code or the host)
    if (g_ip_pc) return rt_blame_guest(g_ip_pc, EXIT_CRASH_RECOMP);
    uint32_t g = rt_host_fn_addr(pc);  // (a recompiled guest function: the game's)
    if (g) return rt_blame_guest(g, EXIT_CRASH_GAME);
    if (engine_module_at(pc)) return EXIT_CRASH_ENGINE;
    const char *lib = plat_symbol(pc, NULL);
    if (lib && !strcmp(lib, GAMELIB_NAME)) return EXIT_CRASH_GAME;  // (the game's library, outside its functions)
    return EXIT_CRASH_RECOMP;
}
