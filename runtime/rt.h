// Runtime-internal declarations.
#pragma once
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>

#include "cpu.h"
#include "platform.h"

#define LOG(...) fprintf(stderr, __VA_ARGS__)
#define FATAL(...) (fprintf(stderr, "FATAL: " __VA_ARGS__), rt_fatal_trace())

// ---- guest address space (32-bit; host address = GUEST_BASE + guest address)
// Layout of the process as this runtime's kernel presents it (GetInfo / QueryMemory agree with it). The modules
// sit low in the code region as Horizon loads them; the heap, alias and stack regions follow.
#define MAIN_BASE     0x00200000u
#define SDK_BASE      0x02400000u   // (main's image ends at 0x02347000)
#define HLE_BASE      0x03000000u   // fake addresses of host-implemented functions (16 bytes each)
#define HLE_SIZE      0x00100000u
#define TLS_BASE      0x03100000u   // thread local regions, 0x200 bytes per thread
#define TLS_SIZE      0x00100000u
#define PLUGIN_BASE   0x03200000u   // Forge plugins (runtime/forge.c): modules, then Forge's hook trampolines / stubs
#define PLUGIN_END    0x03E00000u
#define FORGE_TRAMP   0x03E00000u   // trampolines to hooked functions' original code (dispatch.c runs them)
#define FORGE_STUBS   0x03F00000u   // ARM stubs Forge writes (interpreted)
#define FORGE_END     0x04000000u
#define HLE_HEAP      0x04000000u   // guest-visible memory owned by host code
#define HLE_HEAP_SIZE 0x04000000u
#define CODE_END      0x08000000u
#define HEAP_BASE     0x08000000u   // svcSetHeapSize
#define HEAP_MAX      0xA0000000u   // 2.5 GB
#define ALIAS_BASE    0xA8000000u   // svcMapMemory targets
#define ALIAS_SIZE    0x28000000u
#define STACK_BASE    0xD0000000u   // stack region (main thread stack, mapped thread stacks)
#define STACK_SIZE    0x20000000u
#define GUEST_SPACE   0x100000000ull

// ---- loader.c
typedef struct Module {
    const char *name;
    uint32_t base, size;
    uint32_t text_off, text_size, ro_off, ro_size, data_off, data_size;
    uint32_t dynamic, init, fini, init_array, init_array_sz, fini_array, fini_array_sz;
    uint32_t symtab, strtab, strsz, nsyms;
    uint32_t tls_off, tls_size;  // a plugin that uses __thread: its block in a thread's (from the thread pointer), and its size
    uint32_t bss_size;  // (the end of data_size)
    // A mod's subsdk: the address the game's layout gives it (after main and the subsdks before it, as on the console
    // and in emulators), when it is loaded elsewhere (base); 0: at its own base. See loader_load.
    uint32_t expected;
    const char *mod;  // the mod it comes from (a subsdk), else NULL
} Module;

#define MAX_MODULES 128  // main + sdk + mods' subsdks + Forge plugins
extern Module g_modules[MAX_MODULES];
extern int g_nmodules;
extern int g_nexefs;  // the code modules of exefs/: main, sdk, then the mods' subsdks (g_modules[0..g_nexefs)); then Forge's
void loader_load(const char *exefs_dir);
// A mod's subsdk not where the game's layout puts it: is addr within slack bytes of the range it expects (an address
// the mod's patches use for it)? *shift: where it is minus where it is expected. false: no such module.
bool loader_moved(uint32_t addr, uint32_t slack, int32_t *shift);
uint32_t loader_find_symbol(const char *name);
// An NRO (a module loaded at run time: Forge plugins) at base, relocated against the loaded modules. NULL if invalid.
Module *loader_load_nro(const char *name, const uint8_t *data, size_t len, uint32_t base);
uint32_t loader_module_symbol(const Module *m, const char *name);  // a symbol this module defines, or 0
// Thread-local data of plugins (-mtp=soft, -ftls-model=initial-exec: R_ARM_TLS_TPOFF32): a thread's block is the 8-byte
// TCB, then each such module's tls_size bytes at its tls_off. The size is 0 when no plugin uses it.
uint32_t loader_tls_block_size(void);
// Thread-local data of plugins (-mtp=soft, -ftls-model=initial-exec: R_ARM_TLS_TPOFF32): every thread's block is the 8-byte
// TCB, then each such module's tls_size bytes at its tls_off. The size is 0 when no plugin uses it.
uint32_t loader_tls_block_size(void);
const char *loader_symbol_at(uint32_t addr, uint32_t *offset);

// ---- dispatch.c
typedef void (*HostFn)(Cpu *c);
void dispatch_init(void);
GuestFn dispatch_lookup(uint32_t addr);
// The entry of the recompiled function whose code holds addr: the nearest entry at or below it (0: none), and the
// next entry above it in *next (0: none, it is the module's last).
uint32_t dispatch_function_at(uint32_t addr, uint32_t *next);
// From now on run the function at entry (its code: [entry, end)) in the interpreter, which reads the code in guest
// memory: for code changed while the game runs (cheats, runtime/cheats.c), which the recompiled function would not
// see. Calls that are already running finish in the recompiled code. 1: done, 0: it already was, -1: no function
// there, or too many.
int dispatch_interpret(uint32_t entry, uint32_t end);
uint32_t hle_register(const char *name, HostFn fn);  // returns a fake guest address
HostFn hle_lookup_impl(const char *name);
void hle_override(uint32_t guest_addr, HostFn fn, const char *name);
// Function hooks: calls of the recompiled function at addr run detour (any guest address: recompiled, interpreted or
// host code) instead, with the function's arguments and return address (detour 0 removes the hook). The check sits
// in every function's entry (FN_ENTER, cpu.h). fnhook_call_original runs the function itself; a trampoline address
// (fnhook_add_trampoline) does the same when guest code calls it (original), or runs addr (a hook's predecessor).
bool fnhook_set(uint32_t addr, uint32_t detour);
uint32_t fnhook_get(uint32_t addr);
void fnhook_call_original(Cpu *c, uint32_t addr);
bool fnhook_add_trampoline(uint32_t at, uint32_t addr, bool original);  // false: the table is full
bool fnhook_room(uint32_t addr, bool tramp);  // a hook at addr (with a trampoline) fits in the tables (1024 each)
// A hook that writes over a function's first two words in guest memory (Forge's `ldr pc, [pc, #-4]; .word detour`)
// keeps the words it replaced here (the first time: the originals), so the interpreter runs the function's own code
// when it is interpreted (dispatch_interpret) and the hook's original calls it. fnhook_entry_word: the kept word at
// pc, if pc is one of them.
void fnhook_keep_entry(uint32_t addr, uint32_t w0, uint32_t w1);
bool fnhook_entry_word(uint32_t pc, uint32_t *w);
void rt_fatal_trace(void) __attribute__((noreturn));
void crash_handler_install(void);

// ---- kernel.c
typedef struct KThread KThread;
void kernel_init(void);
KThread *kernel_create_main_thread(uint32_t entry, uint32_t stack_size, int prio);
void kernel_run_main(KThread *t, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3);
uint32_t kernel_current_thread_handle(void);
Cpu *kernel_current_cpu(void);
void kernel_map_region(uint32_t addr, uint32_t size, uint32_t type, uint32_t perm);  // (QueryMemory shows it)
bool kernel_mapped(uint64_t addr, uint32_t size);  // [addr, addr + size) is inside one mapped region

// ---- ipc.c
typedef struct Service Service;
int ipc_handle_request(Cpu *c, uint32_t handle);
uint32_t ipc_connect_named_port(const char *name);

// ---- ui (window loop, runtime/ui.c; host controls, runtime/host_input.c)
struct SDL_Window;
void ui_init(void);                      // SDL and the game window, before the guest starts (the renderer uses it)
int ui_run(void);                        // the host main thread's event loop
struct SDL_Window *ui_window(void);
void ui_toggle_fullscreen(void);         // Alt+Enter: windowed <-> full screen
void ui_setting_changed(const char *key);  // a live setting was changed (config_set): apply it
extern volatile uint64_t g_present_count;  // frames the game presented (the renderer counts them)
union SDL_Event;
void host_input_event(const union SDL_Event *e);
void host_input_update(void);            // recompute the controller state the game reads (each loop pass)
bool host_input_press(const char *button, unsigned frames);  // scripted press from the next frame (UI thread)
void quickhunt_poll(void);               // (debug) MHGU_QUICKHUNT's presses (runtime/hle/quickhunt.c; UI thread)

// ---- helpers
void guest_call(Cpu *c, uint32_t addr);   // call a guest function (saves / restores lr)
uint32_t guest_sym(const char *name);     // cached loader_find_symbol, fatal if missing
uint32_t hle_alloc(uint32_t size);        // zeroed guest memory owned by the host (never freed): guest address
extern const char *g_game_dir;            // folder holding exefs/ and romfs/
extern const char *g_save_dir;            // host folder for save data (services/fs.c; MHGU_SAVE_DIR overrides)
void rt_unwind_point(void);
extern volatile int g_rt_unwind;

// guest strings / pointers
static inline const char *gstr(uint32_t a) { return (const char *)G2H(a); }
