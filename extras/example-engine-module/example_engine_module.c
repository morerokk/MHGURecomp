// Example engine module (docs/ENGINE-MODULES.md): logs what the host tells it and does nothing else.
// Build:  make example-engine-module    (obj/engine-module-test/engine_modules/example/example.dll, or .so)
// Install: copy that folder to engine_modules/example/ and add the line `example` to engine_modules/enabled.txt.
// The same file builds the loader's test modules (make test-engine-modules): -DEXAMPLE_API=<n> claims another API version,
// -DEXAMPLE_NO_API leaves the version out, -DEXAMPLE_DECLINE makes the entry return false.
#include "../../runtime/engine_module_api.h"

#ifndef EXAMPLE_API
#define EXAMPLE_API MHGU_ENGINE_MODULE_API
#endif

#ifndef EXAMPLE_NO_API
MHGU_ENGINE_MODULE_EXPORT const uint32_t mhgu_engine_module_api = EXAMPLE_API;
#endif

static const MhguHost *g_host;

static void bye(void) { g_host->log("[example] shutdown\n"); }

MHGU_ENGINE_MODULE_EXPORT bool mhgu_engine_module_entry(const MhguHost *host, MhguEngineModule *out) {
    g_host = host;
    char value[64];
    host->log("[example] port %s, host API %u, folder %s\n", host->port_version, host->api, host->module_dir);
    if (host->setting("FSR", value, sizeof value)) host->log("[example] FSR = %s\n", value);
#ifdef EXAMPLE_DECLINE
    host->log("[example] declining to start\n");
    return false;
#endif
    out->name = "Example";
    out->shutdown = bye;
    return true;
}
