// Engine plugins (runtime/plugin.c, runtime/plugin_api.h, docs/ENGINE-PLUGINS.md): native code that extends the port itself.
#pragma once
#include <stddef.h>

#include "plugin_api.h"

const char *plugins_dir(void);  // plugins/ beside mods/ (MHGU_PLUGINS_DIR overrides)
void plugins_make_dir(void);    // creates an empty plugins/ (standalone: shows where they go)
void plugins_init(void);        // loads the plugins plugins/enabled.txt lists: once, after the settings are read
void plugins_shutdown(void);    // at a clean exit (registered by plugins_init)
int plugins_loaded(const char **names, int max);  // the loaded plugins' names; returns how many
