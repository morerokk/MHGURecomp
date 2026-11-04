// Engine plugin API (docs/ENGINE-PLUGINS.md). Self-contained C: a plugin copies this one header and includes nothing else of
// the port's.
//
// An engine plugin is native code (plugins/<name>/<name>.dll, or .so) that extends the port itself, not the game: it is not a
// mod. It is loaded only when plugins/enabled.txt lists its folder name, because it runs with the game's rights. Plugins and
// mods are independent: a plugin never enables, disables or changes a mod, and the mods load the same with or without plugins.
//
// The port knows nothing of what a plugin does. There are no hooks in the port for any one feature: a plugin applies its
// changes itself, in memory, once it is loaded (the way a patch would: by intercepting calls the port makes). That keeps a
// feature the port's maintainers cannot test - FSR 4 is the first - out of the port's code, and with its plugin.
//
// A plugin exports one function, mhgu_plugin_entry. The host passes its table; the plugin fills in its own and returns true.
// Compatibility: both structs start with their own size and the API version, and only ever grow at the end. The host never
// reads past what the plugin's `size` says, and a plugin must not read past what the host's `size` says.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MHGU_PLUGIN_API 1

#ifdef _WIN32
#define MHGU_PLUGIN_EXPORT __declspec(dllexport)
#else
#define MHGU_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

typedef struct MhguHost {
    uint32_t size;  // sizeof the host's table
    uint32_t api;   // MHGU_PLUGIN_API of the host
    // Writes a line to the port's log (printf formats; the plugin adds its own [tag]).
    void (*log)(const char *fmt, ...);
    // The value of a setting of mhgurecomp.ini as the file spells it, by key ("FSR", "Resolution"...). false: no such setting.
    bool (*setting)(const char *key, char *out, size_t out_size);
    // The plugin's own folder (UTF-8, absolute, with no trailing slash): where it keeps its files and the libraries it brings.
    const char *plugin_dir;
} MhguHost;

typedef struct MhguPlugin {
    uint32_t size;  // sizeof this table, set by the plugin
    uint32_t api;   // MHGU_PLUGIN_API the plugin was built against, set by the plugin
    const char *name;  // shown in the log (and later the settings menu): "Example", "FSR 4 (AMD SDK)"
    void (*shutdown)(void);  // NULL: none. Called once at a clean exit, in the reverse order of loading.
} MhguPlugin;

// The only symbol a plugin exports. false: the plugin cannot run (it logs why); the host unloads it and carries on without it.
// It runs once, early: before the window, the renderer and the game exist.
typedef bool (*MhguPluginEntry)(const MhguHost *host, MhguPlugin *out);
#define MHGU_PLUGIN_ENTRY_NAME "mhgu_plugin_entry"
