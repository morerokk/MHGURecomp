// The process's exit codes (docs/EXIT-CODES.md): what a launcher or a mod manager can tell from how the game ended.
// A crash's code says whose code was running when it happened, not who caused it (a mod that wrote over the game's
// memory makes the game crash in its own code). Codes stay clear of 1 (a kill from the Task Manager), 3 (Windows'
// abort()) and 128 and up (Linux signals): an exit code not listed here is a crash the handler never saw.
#pragma once
#include <stdint.h>

enum {
    EXIT_NORMAL = 0,         // closed: the window, Quit Game, the title screen, the game's own exit
    EXIT_CRASH_RECOMP = 20,  // a crash in the port's own code (the runtime, the renderer, a driver it called)
    EXIT_CRASH_GAME = 21,    // a crash in the game's code, or the game gave up (a failed check, abort, svcBreak)
    EXIT_CRASH_FORGE = 22,   // a crash in a Forge plugin's code
    EXIT_CRASH_SUBSDK = 23,  // a crash in a mod's subsdk code, or a mod's subsdk that can't be loaded
    EXIT_CRASH_ENGINE = 24,  // a crash in an engine module's native code (or a library it brought)
    EXIT_CANT_START = 30,    // can't start on this PC: the processor, the GPU driver, the game files (a message box said why)
    EXIT_RESTART = 31,       // closed to restart (the settings menu): a new process takes over
};

// Ends the process with code, logged with what it means; the exit handlers run, as with exit().
void rt_exit(int code) __attribute__((noreturn));
// The code the process ends with: the renderer's exit handler ends it with _exit, past exit()'s own status.
int rt_exit_status(void);
const char *rt_exit_meaning(int code);

// Whose fault a FATAL that follows on this thread is (its exit code); 0: the port's (EXIT_CRASH_RECOMP).
void rt_blame(int code);
int rt_blamed(void);
// The code for a crash in the guest code at addr: the game's (main, sdk), a mod's subsdk, a Forge plugin (or Forge's
// trampolines); fallback for an address in none of them.
int rt_blame_guest(uint32_t addr, int fallback);
// The code for a fault at a host pc (the crash handlers): interpreted guest code, the game's recompiled code, an engine
// module's library, else the port's.
int rt_blame_host(uint64_t pc);
