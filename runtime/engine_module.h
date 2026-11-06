// Engine modules (runtime/engine_module.c, runtime/engine_module_api.h, docs/ENGINE-MODULES.md): native code that extends the
// port itself.
#pragma once
#include <stddef.h>

#include "engine_module_api.h"

const char *engine_modules_dir(void);  // engine_modules/ beside mods/ (MHGU_ENGINE_MODULES_DIR overrides)
void engine_modules_make_dir(void);    // creates an empty engine_modules/ (standalone: shows where they go)
// Loads the modules engine_modules/enabled.txt lists: once, after the settings are read. port_version: kept, for the modules.
void engine_modules_init(const char *port_version);
// At a clean exit: from an atexit handler (engine_modules_init registers it) until the renderer's exit handler takes over,
// which ends the process with _exit before that handler would run (nvn/vulkan/device.c). The second call does nothing.
void engine_modules_shutdown(void);
int engine_modules_loaded(const char **names, int max);  // the loaded modules' names; returns how many
void engine_modules_report(void);  // in a crash report: which modules were loaded (none: says nothing)
