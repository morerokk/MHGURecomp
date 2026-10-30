// Host OS layer: everything the runtime needs that differs between Windows and Linux.
// Implementations: runtime/platform/windows.c, runtime/platform/linux.c (the Makefile picks one).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#include "platform/win_compat.h"
#else
// POSIX has no text / binary file modes, and O_BINARY does not exist: the runtime opens everything with O_BINARY
// (on Windows it is what keeps CRLF translation away from game data).
#ifndef O_BINARY
#define O_BINARY 0
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---- guest address space
// Claim the 4 GB guest window at GUEST_BASE (pages are backed lazily, on first touch).
bool plat_reserve_guest(uint64_t base, uint64_t size);
// Make sure host [p, p+size) inside the window is backed before the OS itself writes there (file reads).
void plat_commit(uint64_t host_addr, uint64_t size);
enum { PLAT_PROT_NONE = 0, PLAT_PROT_READ = 1, PLAT_PROT_RW = 3 };
int plat_protect(uint64_t host_addr, uint64_t size, int prot);
#define PLAT_PAGE 0x1000ull
// Pages of host [addr, addr+size) inside the window written since the last call for them (the tracking is reset):
// their host addresses into pages (at most max; a full array means "maybe more": treat all as written), *page = the
// tracking granularity. -1: no write tracking.
// Is host addr in committed, readable memory? *end = where that region (committed or not) ends. (Debug scans.)
bool plat_committed(uint64_t addr, uint64_t *end);int plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max, uint64_t *page);

// ---- time
uint64_t plat_now_ns(void);                // monotonic
void plat_sleep_until_ns(uint64_t t);      // precise wait for plat_now_ns() >= t

// ---- process
void plat_init(void);
bool plat_exe_path(char *buf, size_t size);
unsigned long plat_process_id(void);
bool plat_wait_process(unsigned long pid); // Restart child waits for old host to release saves and logs.
// Started without a game folder: work in the executable's folder; log to log.txt there when there is no terminal
// (plat_fatal_notice then tells the player about a fatal error).
void plat_standalone(void);
#ifdef __APPLE__
bool plat_in_app_bundle(void);  // running from a macOS .app (runtime/platform/darwin.c)
// The game code library for an app: its own (beside the executable), or the one built from the game files in
// game_dir on this Mac, building it first when there is none for this runtime (runtime/platform/darwin_gamecode.m).
// false: not there (the player cancelled, or the build failed: they were told).
bool plat_gamecode_ready(const char *game_dir);
void plat_menu_init(void);      // the Settings menu in the menu bar (runtime/platform/darwin_menu.m)
bool plat_restart_game(void);   // (macOS) the game starts again once this process has ended (runtime/ui.c then quits)
#endif
// The player's own folder for this game, created if missing (save data lives there, away from the game folder so a
// rebuilt or deleted build folder can't take it along): Windows %APPDATA%\MHGURecomp.
// TODO(Linux): $XDG_DATA_HOME/MHGURecomp, else ~/.local/share/MHGURecomp (see docs/STATUS.md "Save data").
bool plat_user_dir(char *buf, size_t size);
// The files in a folder and its subfolders, 0 if it can't be read (main.c: whether the player's romfs is complete;
// the system's own listing, not a stat per file: ~80 ms for the game's 35531 on Windows).
int plat_count_files(const char *dir);
void plat_error_box(const char *msg);
void plat_fatal_notice(void);

// ---- diagnostics
const char *plat_symbol(uint64_t pc, uint64_t *offset);
void plat_print_backtrace(void);
void plat_crash_handler_install(void);
// Called (when set) after a guest page nobody mapped was first touched and got backed with zeros: guest address,
// 1 write / 0 read / -1 unknown. Runs in the fault handler: Forge uses it to name a plugin's bad pointer.
extern void (*plat_touch_hook)(uint64_t guest_addr, int write);
void rt_fault_report(const char *what, uint64_t fault_addr, uint64_t host_pc, uint64_t host_lr) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif
