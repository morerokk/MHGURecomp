// Engine plugins (plugin.h): plugins/<name>/<name>.dll (.so) for each <name> plugins/enabled.txt lists.
#include "plugin.h"

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

const char *plugins_dir(void) {
    const char *e = getenv("MHGU_PLUGINS_DIR");
    return e && *e ? e : "plugins";
}

void plugins_make_dir(void) {
    struct stat st;
    if (stat(plugins_dir(), &st)) {
#ifdef _WIN32
        _mkdir(plugins_dir());
#else
        mkdir(plugins_dir(), 0755);
#endif
    }
}

#define MAX_PLUGINS 32
static struct {
    char name[64];  // the folder's, and the plugin's own name once it has told it
    char own[64];
    void *lib;
    void (*shutdown)(void);
    MhguHost host;  // (a plugin keeps the pointer it is given: the table lives here, for the life of the process)
    char dir[1100];
} g_plugin[MAX_PLUGINS];
static int g_nplugins;
static bool g_registered;

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

// A folder name that cannot leave plugins/: letters, digits, '-', '_', '.', and not starting with a dot.
static bool name_ok(const char *s) {
    if (!*s || *s == '.' || strlen(s) >= sizeof g_plugin[0].name) return false;
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
    // (the folder's own libraries - the ones the plugin brings along - are found beside it)
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
    if (g_nplugins >= MAX_PLUGINS) { LOG("[plugins] %s: too many plugins (%d): not loaded\n", name, MAX_PLUGINS); return; }
    for (int i = 0; i < g_nplugins; i++)
        if (!strcmp(g_plugin[i].name, name)) return;  // (listed twice)
    char rel[1200], dir[1100], lib[1200];
    snprintf(rel, sizeof rel, "%s/%s", plugins_dir(), name);
    struct stat st;
    if (!absolute(rel, dir, sizeof dir) || stat(dir, &st) || !S_ISDIR(st.st_mode)) {
        LOG("[plugins] %s: no such folder in %s: not loaded\n", name, plugins_dir());
        return;
    }
    snprintf(lib, sizeof lib, "%s/%s" LIB_EXT, dir, name);
    if (stat(lib, &st)) { LOG("[plugins] %s: %s/%s" LIB_EXT " is missing: not loaded\n", name, dir, name); return; }
    void *h = open_lib(lib);
    if (!h) {
#ifdef _WIN32
        LOG("[plugins] %s: can't load %s (error %lu): not loaded\n", name, lib, GetLastError());
#else
        LOG("[plugins] %s: can't load %s (%s): not loaded\n", name, lib, dlerror());
#endif
        return;
    }
    MhguPluginEntry entry = (MhguPluginEntry)lib_sym(h, MHGU_PLUGIN_ENTRY_NAME);
    if (!entry) {
        LOG("[plugins] %s: no %s in %s: not a plugin, not loaded\n", name, MHGU_PLUGIN_ENTRY_NAME, lib);
        close_lib(h);
        return;
    }
    int slot = g_nplugins;
    snprintf(g_plugin[slot].dir, sizeof g_plugin[slot].dir, "%s", dir);
    g_plugin[slot].host = (MhguHost){.size = sizeof(MhguHost), .api = MHGU_PLUGIN_API, .log = host_log, .setting = host_setting,
                                     .plugin_dir = g_plugin[slot].dir};
    MhguPlugin pl = {.size = sizeof pl};
    if (!entry(&g_plugin[slot].host, &pl)) { LOG("[plugins] %s: the plugin declined to start (it logged why)\n", name); close_lib(h); return; }
    if (pl.size < offsetof(MhguPlugin, name) + sizeof pl.name || pl.api < 1 || pl.api > MHGU_PLUGIN_API) {
        LOG("[plugins] %s: built for plugin API %u (table %u bytes), this port has %u: not loaded\n", name, pl.api, pl.size,
            MHGU_PLUGIN_API);
        close_lib(h);
        return;
    }
    int i = g_nplugins++;
    snprintf(g_plugin[i].name, sizeof g_plugin[i].name, "%s", name);
    snprintf(g_plugin[i].own, sizeof g_plugin[i].own, "%s", pl.name && *pl.name ? pl.name : name);
    g_plugin[i].lib = h;
    // (a field is read only when the plugin's table is big enough to hold it)
    g_plugin[i].shutdown = pl.size >= offsetof(MhguPlugin, shutdown) + sizeof pl.shutdown ? pl.shutdown : NULL;    LOG("[plugins] %s: loaded (%s, API %u)\n", name, g_plugin[i].own, pl.api);
}

void plugins_init(void) {
    if (g_registered) return;
    g_registered = true;
    char path[1200], line[256];
    snprintf(path, sizeof path, "%s/enabled.txt", plugins_dir());
    FILE *f = fopen(path, "r");
    if (!f) return;  // none enabled: nothing is loaded, whatever plugins/ holds
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        while (isspace((unsigned char)*s)) s++;
        size_t n = strlen(s);
        while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
        if (!*s || *s == '#') continue;
        if (!name_ok(s)) { LOG("[plugins] enabled.txt: \"%s\" is not a folder name: skipped\n", s); continue; }
        load_one(s);
    }
    fclose(f);
    atexit(plugins_shutdown);
}

void plugins_shutdown(void) {
    while (g_nplugins > 0) {
        int i = --g_nplugins;
        if (g_plugin[i].shutdown) g_plugin[i].shutdown();
        // (the library stays loaded: the process is ending, and a plugin's threads may still be unwinding)
    }
}

int plugins_loaded(const char **names, int max) {
    int n = g_nplugins < max ? g_nplugins : max;
    for (int i = 0; i < n; i++) names[i] = g_plugin[i].own;
    return g_nplugins;
}
