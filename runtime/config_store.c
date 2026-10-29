// Atomic INI persistence and live settings. Cold boot state is never mutated;
// each consumer reads hot values atomically on its own thread.
#include "config.h"
#include <SDL3/SDL.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

static Config saved, startup;
static bool have_saved, runtime_ready, runtime_fsr;
static SDL_AtomicInt volume = {100}, confirm_exit = {1}, language = {0}, mute_in_background, window_focused = {1};
static SDL_AtomicInt live_fps, live_fov, live_mouse, live_draw = {0x3f800000}, live_aa = {1}, live_msaa = {1},
                     live_vsync = {1}, live_face, live_menu_confirm, live_background = {1}, live_combo,
                     live_smaa = {SMAA_ULTRA}, live_cmaa2 = {CMAA2_HIGH}, live_cmaa2_sharp;
static void set_float(SDL_AtomicInt *slot, float value) {
    int bits; _Static_assert(sizeof bits == sizeof value, "32-bit float settings");
    memcpy(&bits, &value, sizeof bits); SDL_SetAtomicInt(slot, bits);
}
static float get_float(SDL_AtomicInt *slot) {
    int bits = SDL_GetAtomicInt(slot); float value; memcpy(&value, &bits, sizeof value); return value;
}
static SDL_SpinLock live_ldn_lock;  // [Multiplayer] Server / Key: read by the network threads (runtime/net/ldn.c)
static char live_ldn_server[sizeof saved.ldn_server], live_ldn_key[sizeof saved.ldn_key];
// A save updates changed hot fields only. Saving the volume must not silently
// remove an unchanged FOV/FPS override supplied by the development environment.
static void publish_live(const Config *c, bool all) {
    if (all || c->fps != saved.fps) set_float(&live_fps, (float)c->fps);
    if (all || c->fov != saved.fov) set_float(&live_fov, c->fov);
    if (all || c->mouse_sensitivity != saved.mouse_sensitivity) set_float(&live_mouse, c->mouse_sensitivity);
    if (all || c->draw_distance != saved.draw_distance) set_float(&live_draw, c->draw_distance);
    if (all || c->aa != saved.aa) SDL_SetAtomicInt(&live_aa, c->aa);
    if (all || c->msaa != saved.msaa) SDL_SetAtomicInt(&live_msaa, c->msaa);
    if (all || c->vsync != saved.vsync) SDL_SetAtomicInt(&live_vsync, c->vsync);
    if (all || c->face_buttons != saved.face_buttons) SDL_SetAtomicInt(&live_face, c->face_buttons);
    if (all || c->menu_confirm != saved.menu_confirm) SDL_SetAtomicInt(&live_menu_confirm, c->menu_confirm);
    if (all || c->background_input != saved.background_input) SDL_SetAtomicInt(&live_background, c->background_input);
    if (all || c->combo_window != saved.combo_window) SDL_SetAtomicInt(&live_combo, c->combo_window);
    if (all || c->smaa_quality != saved.smaa_quality) SDL_SetAtomicInt(&live_smaa, c->smaa_quality);
    if (all || c->cmaa2_quality != saved.cmaa2_quality) SDL_SetAtomicInt(&live_cmaa2, c->cmaa2_quality);
    if (all || c->cmaa2_extra_sharpness != saved.cmaa2_extra_sharpness)
        SDL_SetAtomicInt(&live_cmaa2_sharp, c->cmaa2_extra_sharpness);
    if (all || strcmp(c->ldn_server, saved.ldn_server) || strcmp(c->ldn_key, saved.ldn_key)) {
        SDL_LockSpinlock(&live_ldn_lock);
        if (all || strcmp(c->ldn_server, saved.ldn_server)) memcpy(live_ldn_server, c->ldn_server, sizeof live_ldn_server);
        if (all || strcmp(c->ldn_key, saved.ldn_key)) memcpy(live_ldn_key, c->ldn_key, sizeof live_ldn_key);
        SDL_UnlockSpinlock(&live_ldn_lock);
    }
}
// Quick apply (settings_panel.cpp): the menu's unsaved values of the hot settings are live while it is open. A value
// back at the file's gets what was live before the menu changed anything (an environment override stays one), and
// config_preview_end puts those back. A save keeps the previewed values: they are the file's from then on.
static SDL_AtomicInt *const hot[] = {&live_fps, &live_fov, &live_mouse, &live_draw, &live_aa, &live_msaa, &live_vsync,
                                     &live_face, &live_menu_confirm, &live_background, &live_combo, &volume,
                                     &mute_in_background, &live_smaa, &live_cmaa2, &live_cmaa2_sharp};
#define NHOT (sizeof hot / sizeof hot[0])
static int hot_before[NHOT];
static bool previewing;
static int float_bits(float f) { int b; memcpy(&b, &f, sizeof b); return b; }
static void hot_values(const Config *c, int v[NHOT]) {
    const int all[NHOT] = {float_bits((float)c->fps), float_bits(c->fov), float_bits(c->mouse_sensitivity),
                           float_bits(c->draw_distance), c->aa, c->msaa, c->vsync, c->face_buttons, c->menu_confirm,
                           c->background_input, c->combo_window, c->volume, c->mute_in_background, c->smaa_quality,
                           c->cmaa2_quality, c->cmaa2_extra_sharpness};
    memcpy(v, all, sizeof all);
}
void config_preview(const Config *draft) {
    if (!previewing)
        for (size_t i = 0; i < NHOT; i++) hot_before[i] = SDL_GetAtomicInt(hot[i]);
    previewing = true;
    int now[NHOT], file[NHOT];
    hot_values(draft, now);
    hot_values(&saved, file);
    for (size_t i = 0; i < NHOT; i++) SDL_SetAtomicInt(hot[i], now[i] != file[i] ? now[i] : hot_before[i]);
}
void config_preview_end(void) {
    if (previewing)
        for (size_t i = 0; i < NHOT; i++) SDL_SetAtomicInt(hot[i], hot_before[i]);
    previewing = false;
}

void config_store_loaded(const Config *c) {
    previewing = false;
    publish_live(c, !have_saved);
    saved = *c; have_saved = true;
    SDL_SetAtomicInt(&language, c->language);
    SDL_SetAtomicInt(&volume, c->volume);
    SDL_SetAtomicInt(&mute_in_background, c->mute_in_background);
    SDL_SetAtomicInt(&confirm_exit, c->confirm_exit);
}
void config_runtime_init(const Config *effective) {
    startup = saved; runtime_ready = true; runtime_fsr = effective->fsr != 0;
    publish_live(effective, true);
    const char *override = getenv("MHGU_FRAMERATE");
    if (override && *override) {
        float f = strtof(override, NULL);
        if (!(f >= 30)) f = 30;
        if (f > 240) f = 240;
        set_float(&live_fps, f);
    }
}
float config_live_fps(void) { return get_float(&live_fps); }
float config_live_fov(void) { return get_float(&live_fov); }
float config_live_mouse_sensitivity(void) { return get_float(&live_mouse); }
float config_live_draw_distance(void) { return get_float(&live_draw); }
int config_live_aa(void) { return runtime_fsr ? 0 : SDL_GetAtomicInt(&live_aa); }
int config_live_smaa_quality(void) { return SDL_GetAtomicInt(&live_smaa); }
int config_live_cmaa2_quality(void) { return SDL_GetAtomicInt(&live_cmaa2); }
int config_live_cmaa2_extra_sharpness(void) { return SDL_GetAtomicInt(&live_cmaa2_sharp); }
int config_live_msaa(void) {  // (SMAA 4x is S2x: MSAA 2x whatever MSAA says)
    return runtime_fsr ? 1 : SDL_GetAtomicInt(&live_aa) == AA_SMAA_4X ? 2 : SDL_GetAtomicInt(&live_msaa);
}
int config_live_vsync(void) { return SDL_GetAtomicInt(&live_vsync); }
int config_live_menu_confirm(void) { return SDL_GetAtomicInt(&live_menu_confirm); }
int config_live_face_buttons(void) { return SDL_GetAtomicInt(&live_face); }
int config_live_background_input(void) { return SDL_GetAtomicInt(&live_background); }
int config_live_combo_window(void) { return SDL_GetAtomicInt(&live_combo); }
void config_live_ldn(char *server, size_t server_size, char *key, size_t key_size) {
    SDL_LockSpinlock(&live_ldn_lock);
    if (server) SDL_strlcpy(server, live_ldn_server, server_size);
    if (key) SDL_strlcpy(key, live_ldn_key, key_size);
    SDL_UnlockSpinlock(&live_ldn_lock);
}
int config_language(void) { return SDL_GetAtomicInt(&language); }
Config config_editable(void) { return saved; }
float config_audio_gain(void) {
    if (SDL_GetAtomicInt(&mute_in_background) && !SDL_GetAtomicInt(&window_focused)) return 0.0f;
    return SDL_GetAtomicInt(&volume) / 100.0f;
}
void config_window_focus(bool focused) { SDL_SetAtomicInt(&window_focused, focused); }
bool config_confirm_exit(void) { return SDL_GetAtomicInt(&confirm_exit) != 0; }
bool config_restart_required(const Config *c) {
    if (!runtime_ready) return false;
    if (c->hp.quests < 0 || c->hp.quests > HP_QUESTS_MAX) return true;
    for (int i = 0; i < g_nsettings; i++) {
        const ConfigSetting *s = &g_settings[i];
        if (s->restart && (!config_valid(s, c) || !config_equal(s, c, &startup))) return true;
    }
    return c->hp.quests != startup.hp.quests ||
        memcmp(c->hp.quest, startup.hp.quest, sizeof c->hp.quest[0] * (size_t)c->hp.quests);
}

static bool fail(char *error, size_t size, const char *message) {
    if (size) snprintf(error, size, "%s", message);
    return false;
}
static bool factor_valid(float f) { return isfinite(f) && f >= HP_SCALE_MIN && f <= HP_SCALE_MAX; }
bool config_validate(const Config *c, char *error, size_t size) {
    for (int i = 0; i < g_nsettings; i++)
        if (!config_valid(&g_settings[i], c)) return fail(error, size, g_settings[i].error);
    if (c->hp.quests < 0 || c->hp.quests > HP_QUESTS_MAX)
        return fail(error, size, "Invalid village HP or quest override count.");
    for (int i = 0; i < c->hp.quests; i++) {
        if (!c->hp.quest[i].quest) return fail(error, size, "Quest IDs must be greater than zero.");
        for (int j = 0; j < i; j++)
            if (c->hp.quest[j].quest == c->hp.quest[i].quest) return fail(error, size, "Duplicate quest ID.");
        for (int j = 0; j < 4; j++)
            if (!factor_valid(c->hp.quest[i].scale[j])) return fail(error, size, "Quest HP factors must be 0.05-20.");
    }
    if (size) *error = 0;
    return true;
}

// What config_save writes: a setting of the table (setting) or a Quest<number> entry (setting NULL)
typedef struct { const ConfigSetting *setting; const char *section; char key[64], value[256]; bool seen; } Entry;
#define ENTRY_MAX (HP_QUESTS_MAX + 64)
static void normalize(const char *s, char *out, size_t size) {
    size_t n = 0;
    for (; *s && n + 1 < size; s++)
        if (!isspace((unsigned char)*s) && *s != '_' && *s != '-') out[n++] = (char)tolower((unsigned char)*s);
    out[n] = 0;
}
static void flush_section(FILE *out, Entry *entries, int n, const char *section, const char *newline) {
    for (int i = 0; i < n; i++)
        if (!entries[i].seen && !strcasecmp(entries[i].section, section)) {
            fprintf(out, "%s = %s%s", entries[i].key, entries[i].value, newline);
            entries[i].seen = true;
        }
}
// The entry a "key = value" line is for (NULL: none of them)
static Entry *entry_of(Entry *entries, int n, const char *key) {
    const ConfigSetting *s = config_setting(key);
    char normalized[128], other[128];
    normalize(key, normalized, sizeof normalized);
    for (int i = 0; i < n; i++) {
        if (s ? entries[i].setting == s : !entries[i].setting && (normalize(entries[i].key, other, sizeof other),
                                                                   !strcmp(normalized, other)))
            return &entries[i];
    }
    return NULL;
}
// A line that already says what c has (in its own spelling: "21:9", "1.0", "Borderless Window") stays as it is.
static bool line_says(const Entry *e, const char *eq, const Config *c) {
    if (!e->setting) return false;
    char value[256];
    snprintf(value, sizeof value, "%.*s", (int)strcspn(eq + 1, ";#\r\n"), eq + 1);
    char *v = value, *end = value + strlen(value);
    while (isspace((unsigned char)*v)) v++;
    while (end > v && isspace((unsigned char)end[-1])) *--end = 0;
    Config read = *c;
    return config_parse(e->setting, v, &read) && config_equal(e->setting, &read, c);
}
bool config_save(const Config *c, char *error, size_t error_size) {
    if (!config_validate(c, error, error_size)) return false;
    static Entry entries[ENTRY_MAX];  // (big: off the stack)
    int n = 0;
    for (int i = 0; i < g_nsettings; i++) {
        Entry *e = &entries[n++];
        e->setting = &g_settings[i], e->section = g_settings[i].section, e->seen = false;
        snprintf(e->key, sizeof e->key, "%.*s", (int)strcspn(g_settings[i].key, "|"), g_settings[i].key);
        config_format(&g_settings[i], c, e->value, sizeof e->value);
    }
    for (int i = 0; i < c->hp.quests; i++) {
        Entry *e = &entries[n++];
        const float *v = c->hp.quest[i].scale;
        e->setting = NULL, e->section = "Monster HP", e->seen = false;
        snprintf(e->key, sizeof e->key, "Quest%u", c->hp.quest[i].quest);
        snprintf(e->value, sizeof e->value, "%g, %g, %g, %g", (double)v[0], (double)v[1], (double)v[2], (double)v[3]);
    }
    char path[1100];
    config_path(path, sizeof path);
    char temp[1200];
    if (snprintf(temp, sizeof temp, "%s.settings-%llu.tmp", path, (unsigned long long)SDL_GetTicksNS()) >= (int)sizeof temp)
        return fail(error, error_size, "Settings path is too long.");
    FILE *in = fopen(path, "rb");
    if (!in && errno != ENOENT) return fail(error, error_size, "Cannot read the existing settings file.");
    FILE *out = fopen(temp, "wbx");
    if (!out) { if (in) fclose(in); return fail(error, error_size, "Cannot create a temporary settings file."); }
    char line[4096], section[128] = "";
    const char *newline = "\n";
    bool ok = true, ended_line = true;
    while (in && fgets(line, sizeof line, in)) {
        size_t len = strlen(line);
        if (!strchr(line, '\n') && !feof(in)) { ok = false; break; } // never truncate an unknown line
        if (strstr(line, "\r\n")) newline = "\r\n";
        char *s = line; while (isspace((unsigned char)*s)) s++;
        if (*s == '[' && strchr(s, ']')) {
            flush_section(out, entries, n, section, newline);
            snprintf(section, sizeof section, "%.*s", (int)(strchr(s, ']') - s - 1), s + 1);
        }
        char *eq = (*s && *s != ';' && *s != '#' && *s != '[') ? strchr(s, '=') : NULL;
        bool handled = false;
        if (eq) {
            char key[128];
            snprintf(key, sizeof key, "%.*s", (int)(eq - s), s);
            char *k = key + strlen(key);
            while (k > key && isspace((unsigned char)k[-1])) *--k = 0;
            Entry *e = entry_of(entries, n, key);
            if (e) {
                const char *comment = strpbrk(eq + 1, ";#");
                if (line_says(e, eq, c)) fputs(line, out);
                else if (comment) fprintf(out, "%.*s %s %s", (int)(eq - line + 1), line, e->value, comment);
                else fprintf(out, "%.*s %s%s", (int)(eq - line + 1), line, e->value, newline);
                e->seen = true; handled = true; ended_line = true;
            } else {
                // Removing an override must remove its old value, including duplicate entries.
                char normalized[128];
                normalize(key, normalized, sizeof normalized);
                if (!strncmp(normalized, "quest", 5) && isdigit((unsigned char)normalized[5])) handled = true;
            }
        }
        if (!handled) { fputs(line, out); ended_line = len && line[len - 1] == '\n'; }
    }
    if (in) { if (ferror(in)) ok = false; fclose(in); }
    if (!ended_line) fputs(newline, out);
    flush_section(out, entries, n, section, newline);
    for (int i = 0; i < n; i++) if (!entries[i].seen) {
        fprintf(out, "%s[%s]%s", newline, entries[i].section, newline);
        flush_section(out, entries, n, entries[i].section, newline);
    }
    if (fflush(out) || ferror(out)) ok = false;
#ifdef _WIN32
    if (_commit(_fileno(out))) ok = false;
#else
    if (fsync(fileno(out))) ok = false;
#endif
    if (fclose(out)) ok = false;
    if (ok) {
#ifdef _WIN32
        ok = MoveFileExA(temp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
        ok = rename(temp, path) == 0;
#endif
    }
    if (!ok) { remove(temp); return fail(error, error_size, "Could not save settings; the previous file was kept."); }
    config_store_loaded(c);
    return true;
}
