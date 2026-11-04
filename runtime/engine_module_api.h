// Engine module API (docs/ENGINE-MODULES.md). Self-contained C: a module copies this one header and includes nothing else of
// the port's.
//
// An engine module is native code (engine_modules/<name>/<name>.dll, or .so) that extends the port itself, not the game: it is
// neither a mod nor a Forge plugin. It is loaded only when engine_modules/enabled.txt lists its folder name, because it runs with
// the game's rights. Modules are independent of mods and Forge plugins: a module never enables, disables or changes either.
//
// The port knows nothing of what a module does. There are no hooks in the port for any one feature: a module applies its
// changes itself, in memory, once it is loaded (the way a patch would: by intercepting calls the port makes). That keeps a
// feature the port's maintainers cannot test - FSR 4 is the first - out of the port's code, and with its module.
// Only this header is an API. The port's own symbols can be reached (the executable exports them) but may change in any release:
// a module that relies on them checks port_version or what it finds, and declines to load when it doesn't know them.
//
// A module exports two things:
//     MHGU_ENGINE_MODULE_EXPORT const uint32_t mhgu_engine_module_api = MHGU_ENGINE_MODULE_API;
//     MHGU_ENGINE_MODULE_EXPORT bool mhgu_engine_module_entry(const MhguHost *host, MhguEngineModule *out) { ... }
// The host reads the version first: a module built for a newer API than the host's is refused before any of its code is called,
// so a table the module was built with is never bigger than the host's. Both tables only ever grow at the end; `out` starts
// zeroed, so a field an older module doesn't know of stays zero.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MHGU_ENGINE_MODULE_API 1

#ifdef __cplusplus
#define MHGU_ENGINE_MODULE_LINK extern "C"
#else
#define MHGU_ENGINE_MODULE_LINK
#endif
#ifdef _WIN32
#define MHGU_ENGINE_MODULE_EXPORT MHGU_ENGINE_MODULE_LINK __declspec(dllexport)
#else
#define MHGU_ENGINE_MODULE_EXPORT MHGU_ENGINE_MODULE_LINK __attribute__((visibility("default")))
#endif

typedef struct MhguHost {
    uint32_t api;   // MHGU_ENGINE_MODULE_API of the host: at least the module's
    // Writes a line to the port's log (printf formats; the module adds its own [tag]).
    void (*log)(const char *fmt, ...);
    // A setting by its key in mhgurecomp.ini ("FSR", "Resolution"...), spelled as the file spells it. It is the value the game
    // started with (environment overrides included), not a later Save in the settings menu. false: no such setting.
    bool (*setting)(const char *key, char *out, size_t out_size);
    // The module's own folder (absolute, with no trailing slash): where it keeps its files and the libraries it brings. A path
    // for fopen or LoadLibraryA: on Windows in the system's code page, not UTF-8.
    const char *module_dir;
    // The port's release: "0.14.0" on a release, "0.14.0-17-g3c76f99" on a build after it, "unknown" without git.
    const char *port_version;
} MhguHost;

typedef struct MhguEngineModule {
    const char *name;  // shown in the log (and later the settings menu): "Example", "FSR 4 (AMD SDK)"
    // NULL: none. Called once at a clean exit, in the reverse order of loading: when the renderer is up, after the GPU has finished
    // and with the game's threads held out of it, just before the process ends. Be quick. Not called after a crash.
    void (*shutdown)(void);
} MhguEngineModule;

// false: the module cannot run (it logs why, and has undone anything it changed): the host unloads it and carries on without it.
// It runs once, early: the window exists, the renderer and the game do not yet.
typedef bool (*MhguEngineModuleEntry)(const MhguHost *host, MhguEngineModule *out);
#define MHGU_ENGINE_MODULE_API_NAME "mhgu_engine_module_api"
#define MHGU_ENGINE_MODULE_ENTRY_NAME "mhgu_engine_module_entry"
