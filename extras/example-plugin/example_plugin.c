// Example engine plugin (docs/ENGINE-PLUGINS.md): logs what the host tells it and does nothing else.
// Build:  make example-plugin    (build/obj/plugin-test/plugins/example/example.dll, or .so)
// Install: copy that folder to plugins/example/ and add the line `example` to plugins/enabled.txt.
// The same file builds the loader's test plugins (tests/test_plugin.c): -DEXAMPLE_API=<n> claims another API version,
// -DEXAMPLE_DECLINE makes the entry return false.
#include "../../runtime/plugin_api.h"

#ifndef EXAMPLE_API
#define EXAMPLE_API MHGU_PLUGIN_API
#endif

static const MhguHost *g_host;

static void bye(void) { g_host->log("[example] shutdown\n"); }

MHGU_PLUGIN_EXPORT bool mhgu_plugin_entry(const MhguHost *host, MhguPlugin *out) {
    g_host = host;
    char value[64];
    host->log("[example] host API %u, folder %s\n", host->api, host->plugin_dir);
    if (host->setting("FSR", value, sizeof value)) host->log("[example] FSR = %s\n", value);
#ifdef EXAMPLE_DECLINE
    host->log("[example] declining to start\n");
    return false;
#endif
    out->api = EXAMPLE_API;
    out->name = "Example";
    out->shutdown = bye;
    return true;
}
