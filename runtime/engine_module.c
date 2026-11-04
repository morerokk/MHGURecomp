// Engine modules (engine_module.h): engine_modules/<name>/<name>.dll (.so) for each <name> engine_modules/enabled.txt lists.
#include "engine_module.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define LIB_EXT ".dll"
#else
#include <dlfcn.h>
#define LIB_EXT ".so"
#endif

#include "config.h"
#include "rt.h"

const char *engine_modules_dir(void) {
    const char *e = getenv("MHGU_ENGINE_MODULES_DIR");
    return e && *e ? e : "engine_modules";
}

void engine_modules_make_dir(void) {
    struct stat st;
    if (stat(engine_modules_dir(), &st)) {
#ifdef _WIN32
        _mkdir(engine_modules_dir());
#else
        mkdir(engine_modules_dir(), 0755);
#endif
    }
}

#define MAX_MODULES_LOADED 32
static struct {
    char name[64];  // the folder's
    char own[64];   // the module's own name, once it has told it
    void *lib;
    void (*shutdown)(void);
    MhguHost host;  // (a module keeps the pointer it is given: the table lives here, for the life of the process)
    char dir[1100];
} g_mod[MAX_MODULES_LOADED];
static int g_nmods;
static bool g_registered;
static const char *g_port_version;

static void host_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static bool host_setting(const char *key, char *out, size_t n) {
    const ConfigSetting *s = key && out && n ? config_setting(key) : NULL;
    if (!s) return false;
    config_format(s, &g_config, out, n);
    return true;
}

// A folder name that cannot leave engine_modules/: letters, digits, '-', '_', '.', and not starting with a dot.
static bool name_ok(const char *s) {
    if (!*s || *s == '.' || strlen(s) >= sizeof g_mod[0].name) return false;
    for (; *s; s++)
        if (!isalnum((unsigned char)*s) && *s != '-' && *s != '_' && *s != '.') return false;
    return true;
}

static bool absolute(const char *path, char *out, size_t n) {
#ifdef _WIN32
    char buf[MAX_PATH];
    if (!_fullpath(buf, path, sizeof buf)) return false;
    for (char *c = buf; *c; c++)
        if (*c == '\\') *c = '/';
#else
    char buf[4096];
    if (!realpath(path, buf)) return false;
#endif
    if (strlen(buf) >= n) return false;
    strcpy(out, buf);
    return true;
}

static void *open_lib(const char *path) {
#ifdef _WIN32
    // (the folder's own libraries - the ones the module brings along - are found beside it)
    char win[MAX_PATH];
    snprintf(win, sizeof win, "%s", path);
    for (char *c = win; *c; c++)
        if (*c == '/') *c = '\\';
    return (void *)LoadLibraryExA(win, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
#else
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}

static void *lib_sym(void *lib, const char *name) {
#ifdef _WIN32
    return (void *)GetProcAddress((HMODULE)lib, name);
#else
    return dlsym(lib, name);
#endif
}

static void close_lib(void *lib) {
#ifdef _WIN32
    FreeLibrary((HMODULE)lib);
#else
    dlclose(lib);
#endif
}

static void load_one(const char *name) {
    if (g_nmods >= MAX_MODULES_LOADED) {
        LOG("[engine modules] %s: too many modules (%d): not loaded\n", name, MAX_MODULES_LOADED);
        return;
    }
    for (int i = 0; i < g_nmods; i++)
        if (!strcmp(g_mod[i].name, name)) return;  // (listed twice)
    char rel[1200], dir[1100], lib[1200];
    snprintf(rel, sizeof rel, "%s/%s", engine_modules_dir(), name);
    struct stat st;
    if (!absolute(rel, dir, sizeof dir) || stat(dir, &st) || !S_ISDIR(st.st_mode)) {
        LOG("[engine modules] %s: no such folder in %s: not loaded\n", name, engine_modules_dir());
        return;
    }
    snprintf(lib, sizeof lib, "%s/%s" LIB_EXT, dir, name);
    if (stat(lib, &st)) { LOG("[engine modules] %s: %s/%s" LIB_EXT " is missing: not loaded\n", name, dir, name); return; }
    void *h = open_lib(lib);
    if (!h) {
#ifdef _WIN32
        LOG("[engine modules] %s: can't load %s (error %lu): not loaded\n", name, lib, GetLastError());
#else
        LOG("[engine modules] %s: can't load %s (%s): not loaded\n", name, lib, dlerror());
#endif
        return;
    }
    const uint32_t *api = lib_sym(h, MHGU_ENGINE_MODULE_API_NAME);
    MhguEngineModuleEntry entry = (MhguEngineModuleEntry)lib_sym(h, MHGU_ENGINE_MODULE_ENTRY_NAME);
    if (!api || !entry) {
        LOG("[engine modules] %s: no %s in %s: not an engine module, not loaded\n", name,
            api ? MHGU_ENGINE_MODULE_ENTRY_NAME : MHGU_ENGINE_MODULE_API_NAME, lib);
        close_lib(h);
        return;
    }
    // Before any of its code is called: a newer module's tables may be bigger than ours, and unloading one that has already
    // changed the port's code would leave those changes jumping into nothing.
    if (*api < 1 || *api > MHGU_ENGINE_MODULE_API) {
        LOG("[engine modules] %s: built for engine module API %u, this port has %u: not loaded\n", name, *api, MHGU_ENGINE_MODULE_API);
        close_lib(h);
        return;
    }
    int i = g_nmods;
    snprintf(g_mod[i].dir, sizeof g_mod[i].dir, "%s", dir);
    g_mod[i].host = (MhguHost){.api = MHGU_ENGINE_MODULE_API, .log = host_log, .setting = host_setting,
                               .module_dir = g_mod[i].dir, .port_version = g_port_version};
    LOG("[engine modules] %s: starting %s (API %u)\n", name, lib, *api);  // (a crash in its entry: this names it)
    MhguEngineModule m = {0};
    if (!entry(&g_mod[i].host, &m)) {
        LOG("[engine modules] %s: the module declined to start (it logged why)\n", name);
        close_lib(h);
        return;
    }
    g_nmods++;
    snprintf(g_mod[i].name, sizeof g_mod[i].name, "%s", name);
    snprintf(g_mod[i].own, sizeof g_mod[i].own, "%s", m.name && *m.name ? m.name : name);
    g_mod[i].lib = h;
    g_mod[i].shutdown = m.shutdown;
    LOG("[engine modules] %s: loaded (%s)\n", name, g_mod[i].own);
}

void engine_modules_init(const char *port_version) {
    if (g_registered) return;
    g_registered = true;
    g_port_version = port_version;
    char path[1200], line[256];
    snprintf(path, sizeof path, "%s/enabled.txt", engine_modules_dir());
    FILE *f = fopen(path, "r");
    if (!f) return;  // none enabled: nothing is loaded, whatever engine_modules/ holds
    for (bool first = true; fgets(line, sizeof line, f); first = false) {
        char *s = line;
        if (first && !strncmp(s, "\xEF\xBB\xBF", 3)) s += 3;  // (a UTF-8 mark some editors put at the start of a file)
        while (isspace((unsigned char)*s)) s++;
        size_t n = strlen(s);
        while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
        if (!*s || *s == '#') continue;
        if (!name_ok(s)) { LOG("[engine modules] enabled.txt: \"%s\" is not a folder name: skipped\n", s); continue; }
        load_one(s);
    }
    fclose(f);
    atexit(engine_modules_shutdown);
}

void engine_modules_shutdown(void) {
    while (g_nmods > 0) {
        int i = --g_nmods;
        if (g_mod[i].shutdown) g_mod[i].shutdown();
        // (the library stays loaded: the process is ending, and a module's threads may still be unwinding)
    }
}

int engine_modules_loaded(const char **names, int max) {
    int n = g_nmods < max ? g_nmods : max;
    for (int i = 0; i < n; i++) names[i] = g_mod[i].own;
    return g_nmods;
}

void engine_modules_report(void) {
    if (!g_nmods) return;
    LOG("    engine modules loaded (native code, so a possible cause):");
    for (int i = 0; i < g_nmods; i++) LOG(" %s", g_mod[i].own);
    LOG("\n");
}
