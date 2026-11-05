// MHGU Recomp updater: started in place of the game (a shortcut, or Steam's non-Steam game entry), it asks GitHub for
// the latest release, installs it over the game folder it sits in when it is newer than this build, and starts the game
// (with the arguments it was given). The game itself never updates: it starts as ever without this.
//
// - Only GitHub's "latest" release: never a draft, a pre-release or an older version than this one (MHGU_VERSION, the
//   build's tag; a build that is not exactly a tag, e.g. 0.10.3-4-gabc or -dirty, is left alone: --update-force).
// - The archive must match its asset's size and SHA-256 (the digest GitHub keeps for every upload).
// - Never written: data/, mods/, engine_modules/, cheats/, bonus/, cache/, shots/, save/, mhgurecomp.ini and log.txt (the player's), only
//   the files the release carries besides those. Each file replaced is kept in update/backup/ until the next start, and
//   put back if the install stops halfway.
// - No connection, GitHub's rate limit (60 checks an hour per address; an unchanged release is answered "304" from its
//   ETag, without the JSON again), a cancelled or failed update: the game starts as it is.
// - [Application] Updates in mhgurecomp.ini: Ask (the default), Auto (no question), Off (start the game at once).
//
//   --update-force       update a development build too        --update-no-launch   don't start the game afterwards
//   MHGU_UPDATE_API=url  another "latest release" address (tests)
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "updater.h"

#ifdef _WIN32
#include <windows.h>
#define GAME_EXE "mhgurecomp.exe"
#define GAME_LIB "mhgugame.dll"
#define ASSET_PREFIXES "Windows-"
#else
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#define GAME_EXE "mhgurecomp"
#define GAME_LIB "mhgugame.so"
#define ASSET_PREFIXES "Linux-,ArchLinux-"  // (ArchLinux-: up to 0.11.4)
#endif
#define LATEST_URL "https://api.github.com/repos/morerokk/MHGURecomp/releases/latest"

// what the archive must never write (first path component, any case): the player's
static const char *const KEEP[] = {"data", "mods", "engine_modules", "cheats", "bonus", "cache", "shots", "save", "update",
                                   "mhgurecomp.ini", "log.txt"};

static char root[1024];  // the game folder (this executable's), with the separator at its end
static SDL_IOStream *logf;
static SDL_Mutex *lock;

void ulog(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    SDL_LockMutex(lock);
    fputs(buf, stderr);
    if (logf) SDL_WriteIO(logf, buf, strlen(buf)), SDL_FlushIO(logf);
    SDL_UnlockMutex(lock);
}

static void path(char *out, size_t size, const char *rel) { snprintf(out, size, "%s%s", root, rel); }

// ---- versions: "0.10.3" (or v0.10.3) as numbers; clean: nothing after them
static bool version(const char *s, int v[4], bool *clean) {
    memset(v, 0, 4 * sizeof *v);
    if (*s == 'v' || *s == 'V') s++;
    int n = 0;
    while (n < 4 && *s >= '0' && *s <= '9') {
        v[n++] = (int)strtol(s, (char **)&s, 10);
        if (*s != '.') break;
        s++;
    }
    if (clean) *clean = n > 0 && !*s;
    return n > 0;
}
static int version_cmp(const int a[4], const int b[4]) {
    for (int i = 0; i < 4; i++) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

// ---- files
static SDL_EnumerationResult SDLCALL rm_entry(void *arg, const char *dir, const char *name) {
    char p[2048];
    snprintf(p, sizeof p, "%s%s", dir, name);
    SDL_PathInfo info;
    if (SDL_GetPathInfo(p, &info) && info.type == SDL_PATHTYPE_DIRECTORY) SDL_EnumerateDirectory(p, rm_entry, arg);
    if (!SDL_RemovePath(p)) *(bool *)arg = false;
    return SDL_ENUM_CONTINUE;
}
static bool rm_tree(const char *p) {  // (true: gone)
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(p, &info)) return true;
    bool ok = true;
    if (info.type == SDL_PATHTYPE_DIRECTORY) SDL_EnumerateDirectory(p, rm_entry, &ok);
    return SDL_RemovePath(p) && ok;
}
static bool exists(const char *p) { return SDL_GetPathInfo(p, NULL); }
static bool make_parent(const char *p) {
    char d[2048];
    snprintf(d, sizeof d, "%s", p);
    char *s = strrchr(d, '/'), *b = strrchr(d, '\\');
    if (b > s) s = b;
    if (!s) return true;
    *s = 0;
    return SDL_CreateDirectory(d);
}

// The setting ([Application] Updates): 0 ask, 1 auto, 2 off
enum { MODE_ASK, MODE_AUTO, MODE_OFF };
static void squash(char *s) {
    char *d = s;
    for (; *s; s++) if (*s != ' ' && *s != '\t' && *s != '_' && *s != '-' && *s != '\r') *d++ = (char)SDL_tolower(*s);
    *d = 0;
}
static int read_mode(void) {
    char ini[1100];
    const char *home = SDL_getenv("MHGU_HOME");  // (run-game.bat: the settings are there)
    if (home && *home) snprintf(ini, sizeof ini, "%s/mhgurecomp.ini", home);
    else path(ini, sizeof ini, "mhgurecomp.ini");
    char *text = SDL_LoadFile(ini, NULL);
    int mode = MODE_ASK;
    bool app = false;
    for (char *line = text ? strtok(text, "\n") : NULL; line; line = strtok(NULL, "\n")) {
        line += strspn(line, " \t");
        if (*line == '[') { app = !strncasecmp(line, "[Application]", 13); continue; }
        char *eq = strchr(line, '=');
        if (!app || *line == ';' || *line == '#' || !eq) continue;
        *eq = 0;
        char key[64], value[64];
        snprintf(key, sizeof key, "%s", line), snprintf(value, sizeof value, "%s", eq + 1);
        squash(key), squash(value);
        if (strcmp(key, "updates")) continue;
        if (!strcmp(value, "off") || !strcmp(value, "never") || !strcmp(value, "0")) mode = MODE_OFF;
        else if (!strcmp(value, "auto") || !strcmp(value, "automatic")) mode = MODE_AUTO;
        else mode = MODE_ASK;
    }
    SDL_free(text);
    return mode;
}

// Is the game running from this folder? (Its files can't be replaced under it: Windows refuses, and on Linux it would
// go on with the old ones.)
static bool game_running(void) {
    char exe[1100];
    path(exe, sizeof exe, GAME_EXE);
#ifdef _WIN32
    const char *names[] = {GAME_EXE, GAME_LIB};
    for (int i = 0; i < 2; i++) {
        wchar_t w[1100];
        char p[1100];
        path(p, sizeof p, names[i]);
        if (!MultiByteToWideChar(CP_UTF8, 0, p, -1, w, 1100)) continue;
        HANDLE h = CreateFileW(w, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                               OPEN_EXISTING, 0, NULL);  // (a running image can't be opened for writing)
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        else if (GetLastError() == ERROR_SHARING_VIOLATION) return true;
    }
    return false;
#else
    char want[4096], got[4096];
    if (!realpath(exe, want)) return false;
    DIR *d = opendir("/proc");
    bool found = false;
    for (struct dirent *e; d && !found && (e = readdir(d));) {
        if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
        char link[300];
        snprintf(link, sizeof link, "/proc/%s/exe", e->d_name);
        ssize_t n = readlink(link, got, sizeof got - 1);
        if (n > 0) got[n] = 0, found = !strcmp(got, want);
    }
    if (d) closedir(d);
    return found;
#endif
}

// ---- what the window shows, set by the worker
enum { ST_CHECKING, ST_ASK, ST_DOWNLOADING, ST_INSTALLING, ST_DONE, ST_FAILED, ST_LAUNCH };
static SDL_AtomicInt state, answer, cancel, kb_done, kb_total;  // answer: 1 update, 2 not now
static char latest[64], failure[300];
static bool no_launch;  // (the game is running: don't start another)
static int mode;
static bool force;

static void set_state(int s) { SDL_SetAtomicInt(&state, s); }
static int failed(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int failed(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    SDL_LockMutex(lock);
    vsnprintf(failure, sizeof failure, fmt, ap);
    SDL_UnlockMutex(lock);
    va_end(ap);
    ulog("[update] failed: %s\n", failure);
    set_state(ST_FAILED);
    return 0;
}
static int quietly(const char *why) {  // nothing to do (or nothing worth bothering the player with): play
    ulog("[update] %s\n", why);
    set_state(ST_LAUNCH);
    return 0;
}

typedef struct { char *data; size_t size, cap; } Buf;
static bool to_buf(void *ctx, const void *data, size_t n) {
    Buf *b = ctx;
    if (b->size + n > (4u << 20)) return false;
    if (b->size + n + 1 > b->cap) {
        size_t cap = (b->size + n + 1) * 2;
        char *p = realloc(b->data, cap);
        if (!p) return false;
        b->data = p, b->cap = cap;
    }
    memcpy(b->data + b->size, data, n);
    b->size += n;
    b->data[b->size] = 0;
    return true;
}

typedef struct { SDL_IOStream *out; Sha256 sha; int64_t got; } Download;
static bool to_file(void *ctx, const void *data, size_t n) {
    Download *d = ctx;
    if (SDL_GetAtomicInt(&cancel)) return false;
    if (SDL_WriteIO(d->out, data, n) != n) return false;
    sha256_update(&d->sha, data, n);
    d->got += (int64_t)n;
    SDL_SetAtomicInt(&kb_done, (int)(d->got >> 10));
    return true;
}

// ---- extracting into update/new/: the files to install, their paths relative to the game folder
typedef struct { char rel[512]; unsigned mode; } Item;
typedef struct {
    Item *items;
    int n, cap;
    char top[256];  // the archive's one top folder (Windows-MHGURecomp/), left out
    bool bad;
    char why[300];
} Extract;

static bool safe_rel(const char *rel) {
    if (!*rel || *rel == '/' || strchr(rel, ':') || strchr(rel, '\\') || strlen(rel) >= 500) return false;
    for (const char *p = rel; *p;) {
        size_t n = strcspn(p, "/");
        if (!n || (n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.')) return false;
        p += n + (p[n] == '/');
    }
    return true;
}
static bool kept(const char *rel) {
    size_t n = strcspn(rel, "/");
    for (size_t i = 0; i < sizeof KEEP / sizeof *KEEP; i++)
        if (strlen(KEEP[i]) == n && !strncasecmp(rel, KEEP[i], n)) return true;
    return false;
}

static SDL_IOStream *on_entry(void *ctx, const char *name, bool dir, unsigned mode) {
    Extract *x = ctx;
    if (x->bad) return NULL;
    size_t tn = strcspn(name, "/");
    if (!name[tn]) {  // (a file at the top: not the layout of a release)
        if (dir) return NULL;
        snprintf(x->why, sizeof x->why, "unexpected archive layout (%s)", name);
        x->bad = true;
        return NULL;
    }
    if (!x->top[0]) snprintf(x->top, sizeof x->top, "%.*s", (int)tn, name);
    else if (strlen(x->top) != tn || strncmp(x->top, name, tn)) {
        snprintf(x->why, sizeof x->why, "unexpected archive layout (%s)", name);
        x->bad = true;
        return NULL;
    }
    char rel[1024];
    snprintf(rel, sizeof rel, "%s", name + tn + 1);
    size_t len = strlen(rel);
    while (len && rel[len - 1] == '/') rel[--len] = 0;
    if (!len) return NULL;
    if (!safe_rel(rel)) {
        snprintf(x->why, sizeof x->why, "unsafe path in the archive (%s)", name);
        x->bad = true;
        return NULL;
    }
    if (kept(rel)) return NULL;
    char p[2048];
    snprintf(p, sizeof p, "%supdate/new/%s", root, rel);
    if (dir) {
        SDL_CreateDirectory(p);
        return NULL;
    }
    if (x->n == x->cap) {
        Item *items = realloc(x->items, (x->cap = x->cap ? x->cap * 2 : 64) * sizeof *items);
        if (!items) { x->bad = true; snprintf(x->why, sizeof x->why, "out of memory"); return NULL; }
        x->items = items;
    }
    make_parent(p);
    SDL_IOStream *out = SDL_IOFromFile(p, "wb");
    if (!out) {
        snprintf(x->why, sizeof x->why, "can't write %s: %s", p, SDL_GetError());
        x->bad = true;
        return NULL;
    }
    Item *it = &x->items[x->n++];
    snprintf(it->rel, sizeof it->rel, "%s", rel);
    it->mode = mode;
    return out;
}

// Put the extracted files in place, each one it replaces kept in update/backup/; on a failure, all put back.
static bool install(const Extract *x, char *why, size_t why_size) {
    char src[2048], dst[2048], bak[2048];
    bool *moved = calloc(x->n ? x->n : 1, sizeof *moved), *placed = calloc(x->n ? x->n : 1, sizeof *placed);
    bool ok = moved && placed;
    for (int i = 0; ok && i < x->n; i++) {
        const char *rel = x->items[i].rel;
        snprintf(src, sizeof src, "%supdate/new/%s", root, rel);
        snprintf(dst, sizeof dst, "%s%s", root, rel);
        snprintf(bak, sizeof bak, "%supdate/backup/%s", root, rel);
#ifndef _WIN32
        // (a zip made on Windows has no permissions: the programs and libraries by name)
        const char *base = strrchr(rel, '/') ? strrchr(rel, '/') + 1 : rel;
        bool program = !strcmp(base, GAME_EXE) || !strcmp(base, "mhgurecomp-updater") || strstr(base, ".so");
        unsigned m = x->items[i].mode & 0777;
        if (!m || program) m = program ? 0755 : 0644;
        chmod(src, m);
#endif
        if (exists(dst)) {
            if (!make_parent(bak) || !SDL_RenamePath(dst, bak)) {
                snprintf(why, why_size, "can't move %s aside: %s", dst, SDL_GetError());
                ok = false;
                break;
            }
            moved[i] = true;
        }
        if (!make_parent(dst) || !SDL_RenamePath(src, dst)) {
            snprintf(why, why_size, "can't write %s: %s", dst, SDL_GetError());
            ok = false;
            break;
        }
        placed[i] = true;
    }
    if (!ok && moved && placed) {
        ulog("[update] putting the old files back\n");
        for (int i = x->n; i-- > 0;) {
            snprintf(dst, sizeof dst, "%s%s", root, x->items[i].rel);
            snprintf(bak, sizeof bak, "%supdate/backup/%s", root, x->items[i].rel);
            if (placed[i]) SDL_RemovePath(dst);
            if (moved[i] && !SDL_RenamePath(bak, dst)) ulog("[update] can't put back %s: %s\n", dst, SDL_GetError());
        }
    }
    free(moved), free(placed);
    return ok;
}

static const Asset *pick_asset(const Release *r) {
    const Asset *best = NULL;
    for (int i = 0; i < r->nassets; i++) {
        const Asset *a = &r->assets[i];
        bool mine = false;
        for (const char *p = ASSET_PREFIXES; *p;) {
            size_t n = strcspn(p, ",");
            mine |= !strncmp(a->name, p, n);
            p += n + (p[n] == ',');
        }
        size_t len = strlen(a->name);
        bool tgz = len > 7 && !strcmp(a->name + len - 7, ".tar.gz"), zip = len > 4 && !strcmp(a->name + len - 4, ".zip");
        if (!mine || !(tgz || zip)) continue;
        if (!best || tgz) best = a;  // (.tar.gz first: it keeps the files' permissions)
    }
    return best;
}

static int SDLCALL worker(void *arg) {
    (void)arg;
    char p[1100], etag_path[1100];
    path(p, sizeof p, "update/latest.json");
    path(etag_path, sizeof etag_path, "update/latest.etag");
    char *etag = SDL_LoadFile(etag_path, NULL);
    bool cached = etag && *etag && exists(p);
    const char *url = SDL_getenv("MHGU_UPDATE_API");
    if (!url || !*url) url = LATEST_URL;
    Buf body = {0};
    HttpResult r;
    bool ok = http_get(url, "application/vnd.github+json", cached ? etag : NULL, 5, to_buf, &body, &r);
    SDL_free(etag);
    if (!ok) return quietly(r.error[0] ? r.error : "no answer");
    ulog("[update] %s: %d%s (rate limit: %ld left)\n", url, r.status, r.status == 304 ? " unchanged" : "",
         r.rate_remaining);
    if (r.status == 304) {  // (unchanged since the last check: the release saved then)
        size_t n;
        free(body.data);
        body.data = SDL_LoadFile(p, &n), body.size = n;
        if (!body.data) return quietly("the saved release is gone");
    } else if (r.status == 200) {
        SDL_SaveFile(p, body.data, body.size);
        SDL_SaveFile(etag_path, r.etag, strlen(r.etag));
    } else if (r.status == 403 || r.status == 429) {
        return quietly("GitHub's rate limit: next time");
    } else {
        char why[64];
        snprintf(why, sizeof why, "GitHub answered %d", r.status);
        return quietly(why);
    }
    Release rel;
    ok = json_release(body.data, body.size, &rel);
    if (r.status == 304) SDL_free(body.data); else free(body.data);
    if (!ok) return quietly("can't read the release");
    if (rel.draft || rel.prerelease) return quietly("the latest release is a draft or pre-release");
    int have[4], want[4];
    bool clean;
    if (!version(rel.tag, want, &clean) || !clean) return quietly("the latest release's tag is not a version");
    version(MHGU_VERSION, have, NULL);
    ulog("[update] this build %s, latest release %s\n", MHGU_VERSION, rel.tag);
    if (version_cmp(want, have) <= 0) return quietly("up to date");
    const Asset *a = pick_asset(&rel);
    if (!a) return quietly("no download for this system in the release");
    if (strncmp(a->digest, "sha256:", 7) || strlen(a->digest) != 7 + 64)
        return quietly("the release's download has no SHA-256 digest: not installed");
    if (a->size < (1 << 20) || a->size > ((int64_t)2 << 30)) return quietly("the release's download has an odd size");
    snprintf(latest, sizeof latest, "%s", rel.tag);

    if (SDL_GetAtomicInt(&cancel)) return quietly("skipped");
    if (mode == MODE_ASK) {
        set_state(ST_ASK);
        while (!SDL_GetAtomicInt(&answer)) SDL_Delay(20);
        if (SDL_GetAtomicInt(&answer) != 1) return quietly("not now");
    }

    // download (update/<asset>.part), checked
    set_state(ST_DOWNLOADING);
    SDL_SetAtomicInt(&kb_total, (int)(a->size >> 10));
    char part[1100];
    snprintf(part, sizeof part, "%supdate/%s.part", root, a->name);
    Download d = {SDL_IOFromFile(part, "wb")};
    if (!d.out) return failed("can't write %s: %s", part, SDL_GetError());
    sha256_init(&d.sha);
    ulog("[update] downloading %s (%lld bytes)\n", a->url, (long long)a->size);
    ok = http_get(a->url, "application/octet-stream", NULL, 15, to_file, &d, &r);
    if (!SDL_CloseIO(d.out)) ok = false;
    if (SDL_GetAtomicInt(&cancel)) { SDL_RemovePath(part); return quietly("download cancelled"); }
    if (!ok || r.status != 200) {
        SDL_RemovePath(part);
        return r.status && r.status != 200 ? failed("download: GitHub answered %d", r.status)
                                           : failed("download: %s", r.error[0] ? r.error : "no answer");
    }
    char hex[65];
    sha256_hex(&d.sha, hex);
    if (d.got != a->size || strcmp(hex, a->digest + 7)) {
        SDL_RemovePath(part);
        return failed("the download is damaged (%lld of %lld bytes, SHA-256 %.12s..., expected %.12s...)",
                      (long long)d.got, (long long)a->size, hex, a->digest + 7);
    }
    ulog("[update] download checked: %lld bytes, sha256 %s\n", (long long)d.got, hex);

    // install
    set_state(ST_INSTALLING);
    if (game_running()) {
        no_launch = true;
        SDL_RemovePath(part);
        return failed("MHGU Recomp is running: close it, then start the updater again.");
    }
    char newdir[1100];
    path(newdir, sizeof newdir, "update/new");
    rm_tree(newdir);
    SDL_CreateDirectory(newdir);
    Extract x = {0};
    char why[300] = "";
    ok = archive_extract(part, on_entry, &x, why, sizeof why);
    if (x.bad) ok = false, snprintf(why, sizeof why, "%s", x.why);
    bool complete = false, has_lib = false;
    for (int i = 0; i < x.n; i++) complete |= !strcmp(x.items[i].rel, GAME_EXE), has_lib |= !strcmp(x.items[i].rel, GAME_LIB);
    if (ok && !(complete && has_lib)) ok = false, snprintf(why, sizeof why, "the release has no %s or %s", GAME_EXE, GAME_LIB);
    if (ok) {
        ulog("[update] installing %d files\n", x.n);
        ok = install(&x, why, sizeof why);
    }
    free(x.items);
    rm_tree(newdir);
    SDL_RemovePath(part);
    if (!ok) return failed("%s", why);
    char bak[1100];
    path(bak, sizeof bak, "update/backup");
    rm_tree(bak);  // (Windows keeps the running updater's own files: they go at the next start)
    ulog("[update] updated to %s\n", latest);
    set_state(ST_DONE);
    return 0;
}

// ---- the window: 8x8 text (SDL's own), keyboard, mouse and controller
#define W 800
#define H 300
typedef struct { const char *label; int action; } Button;  // action: 1 update, 2 not now / cancel / skip, 3 continue
static int sel;

static void text(SDL_Renderer *rd, float x, float y, const char *s, int scale) {
    SDL_SetRenderScale(rd, (float)scale, (float)scale);
    SDL_RenderDebugText(rd, x / scale, y / scale, s);
    SDL_SetRenderScale(rd, 1, 1);
}
static float wrap(SDL_Renderer *rd, float x, float y, const char *s, int cols) {  // (2x: 16 px a character)
    while (*s) {
        int n = (int)strlen(s);
        if (n > cols) {
            n = cols;
            while (n > 0 && s[n] != ' ') n--;
            if (!n) n = cols;
        }
        char line[256];
        snprintf(line, sizeof line, "%.*s", n, s);
        text(rd, x, y, line, 2);
        y += 24;
        s += n;
        while (*s == ' ') s++;
    }
    return y;
}

static SDL_FRect button_rect(int i, int n) {
    float w = 200, gap = 24, x0 = (W - (n * w + (n - 1) * gap)) / 2;
    return (SDL_FRect){x0 + i * (w + gap), H - 70, w, 44};
}

static int draw(SDL_Renderer *rd, int st, const Button *b, int nb, Uint64 shown_at) {
    SDL_SetRenderDrawColor(rd, 24, 26, 32, 255);
    SDL_RenderClear(rd);
    SDL_SetRenderDrawColor(rd, 240, 180, 60, 255);
    text(rd, 32, 28, "MHGU Recomp updater", 3);
    SDL_SetRenderDrawColor(rd, 230, 230, 235, 255);
    char msg[512];
    SDL_LockMutex(lock);
    switch (st) {
    case ST_CHECKING: snprintf(msg, sizeof msg, "Checking for updates..."); break;
    case ST_ASK:
        snprintf(msg, sizeof msg, "Version %s is available (this is %s). Update now? Your settings, saves, mods and "
                 "game files are kept.", latest, MHGU_VERSION);
        break;
    case ST_DOWNLOADING:
        snprintf(msg, sizeof msg, "Downloading %s...   %.1f / %.1f MB", latest, SDL_GetAtomicInt(&kb_done) / 1024.0,
                 SDL_GetAtomicInt(&kb_total) / 1024.0);
        break;
    case ST_INSTALLING: snprintf(msg, sizeof msg, "Installing %s...", latest); break;
    case ST_DONE: snprintf(msg, sizeof msg, "Updated to %s. Starting the game...", latest); break;
    case ST_FAILED:
        if (no_launch) snprintf(msg, sizeof msg, "The update was not installed. %s", failure);
        else snprintf(msg, sizeof msg, "The update was not installed: %s   The game starts as it is in %d s.", failure,
                      (int)(10 - (SDL_GetTicks() - shown_at) / 1000));
        break;
    default: msg[0] = 0;
    }
    SDL_UnlockMutex(lock);
    wrap(rd, 32, 90, msg, (W - 64) / 16);
    if (st == ST_DOWNLOADING) {
        int total = SDL_GetAtomicInt(&kb_total);
        float f = total ? (float)SDL_GetAtomicInt(&kb_done) / total : 0;
        SDL_FRect bar = {32, 170, W - 64, 20}, fill = {32, 170, (W - 64) * (f > 1 ? 1 : f), 20};
        SDL_SetRenderDrawColor(rd, 60, 64, 76, 255);
        SDL_RenderFillRect(rd, &bar);
        SDL_SetRenderDrawColor(rd, 240, 180, 60, 255);
        SDL_RenderFillRect(rd, &fill);
    }
    for (int i = 0; i < nb; i++) {
        SDL_FRect r = button_rect(i, nb);
        if (i == sel) SDL_SetRenderDrawColor(rd, 240, 180, 60, 255);
        else SDL_SetRenderDrawColor(rd, 60, 64, 76, 255);
        SDL_RenderFillRect(rd, &r);
        if (i == sel) SDL_SetRenderDrawColor(rd, 24, 26, 32, 255);
        else SDL_SetRenderDrawColor(rd, 230, 230, 235, 255);
        text(rd, r.x + (r.w - 16 * strlen(b[i].label)) / 2, r.y + 14, b[i].label, 2);
    }
    SDL_RenderPresent(rd);
    return 0;
}

static int buttons(int st, Button *b) {
    switch (st) {
    case ST_CHECKING: b[0] = (Button){"Skip", 2}; return 1;
    case ST_ASK: b[0] = (Button){"Update", 1}, b[1] = (Button){"Not now", 2}; return 2;
    case ST_DOWNLOADING: b[0] = (Button){"Cancel", 2}; return 1;
    case ST_FAILED: b[0] = no_launch ? (Button){"Close", 3} : (Button){"Start game", 3}; return 1;
    default: return 0;
    }
}

// a button's action: 0 go on, 1 leave the window
static int act(int st, int action) {
    if (st == ST_ASK) { SDL_SetAtomicInt(&answer, action); return 0; }
    if (st == ST_CHECKING && action == 2) return 1;  // (the check goes on unheard: its answer no longer matters)
    if (st == ST_DOWNLOADING && action == 2) { SDL_SetAtomicInt(&cancel, 1); return 0; }
    return st == ST_FAILED;
}

// The window's icon: a Palico, the game's is a greatsword. Windows takes it from the exe (extras/icon/mhgurecomp-updater.rc),
// Linux from extras/icon/mhgurecomp-updater-128.png, built in here (the path is relative to the repo, where make runs).
#ifndef _WIN32
__asm__(".pushsection .rodata\n.globl mhgu_updater_icon_png\nmhgu_updater_icon_png:\n"
        ".incbin \"extras/icon/mhgurecomp-updater-128.png\"\n"
        ".globl mhgu_updater_icon_png_end\nmhgu_updater_icon_png_end:\n.popsection");
extern const unsigned char mhgu_updater_icon_png[], mhgu_updater_icon_png_end[];

static void set_window_icon(SDL_Window *win) {
    size_t n = (size_t)(mhgu_updater_icon_png_end - mhgu_updater_icon_png);
    SDL_Surface *s = SDL_LoadPNG_IO(SDL_IOFromConstMem(mhgu_updater_icon_png, n), true);
    if (s) SDL_SetWindowIcon(win, s);
    SDL_DestroySurface(s);
}
#else
static void set_window_icon(SDL_Window *win) { (void)win; }
#endif

// The window, while the worker runs: false when the worker is left behind (skipped while checking)
static bool run_window(SDL_Thread *th) {
    SDL_Window *win = NULL;
    SDL_Renderer *rd = NULL;
#ifndef _WIN32
    // (the game's app id, so the window goes with the game's app menu launcher: runtime/ui.c, install_launcher)
    SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_IDENTIFIER_STRING, "mhgurecomp");
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD) &&
        SDL_CreateWindowAndRenderer("MHGU Recomp updater", W, H, SDL_WINDOW_HIDDEN, &win, &rd)) {
        SDL_SetRenderVSync(rd, 1);
        set_window_icon(win);
    }
    else
        ulog("[update] no window (%s)%s\n", SDL_GetError(), mode == MODE_ASK ? ": not asking" : "");
    if (!rd && mode == MODE_ASK) SDL_SetAtomicInt(&answer, 2);
    Uint64 start = SDL_GetTicks(), state_at = start;
    int last = -1;
    bool shown = false, left = false;
    for (;;) {
        int st = SDL_GetAtomicInt(&state);
        if (st != last) last = st, state_at = SDL_GetTicks(), sel = 0;
        if (st == ST_LAUNCH) break;
        if (st == ST_DONE && SDL_GetTicks() - state_at > 1500) break;
        if (st == ST_FAILED && !no_launch && SDL_GetTicks() - state_at > 10000) break;
        if (!rd) {
            if (st == ST_FAILED || st == ST_DONE) break;
            SDL_Delay(50);
            continue;
        }
        // shown when there is something to say: not for a quick "up to date"
        if (!shown && (st != ST_CHECKING || SDL_GetTicks() - start > 700)) SDL_ShowWindow(win), shown = true;
        Button b[2];
        int nb = buttons(st, b), action = 0;
        if (sel >= nb) sel = 0;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            int key = e.type == SDL_EVENT_KEY_DOWN ? (int)e.key.key : 0;
            int pad = e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ? e.gbutton.button : -1;
            if (e.type == SDL_EVENT_GAMEPAD_ADDED) SDL_OpenGamepad(e.gdevice.which);
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED || key == SDLK_ESCAPE ||
                pad == SDL_GAMEPAD_BUTTON_EAST || pad == SDL_GAMEPAD_BUTTON_BACK) {
                action = nb ? b[nb - 1].action : 0;
            } else if (key == SDLK_LEFT || key == SDLK_RIGHT || key == SDLK_TAB || pad == SDL_GAMEPAD_BUTTON_DPAD_LEFT ||
                       pad == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) {
                if (nb) sel = (sel + 1) % nb;
            } else if (key == SDLK_RETURN || key == SDLK_SPACE || key == SDLK_KP_ENTER || pad == SDL_GAMEPAD_BUTTON_SOUTH ||
                       pad == SDL_GAMEPAD_BUTTON_START) {
                if (nb) action = b[sel].action;
            } else if (e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                SDL_FPoint m = {e.type == SDL_EVENT_MOUSE_MOTION ? e.motion.x : e.button.x,
                                e.type == SDL_EVENT_MOUSE_MOTION ? e.motion.y : e.button.y};
                for (int i = 0; i < nb; i++) {
                    SDL_FRect r = button_rect(i, nb);
                    if (SDL_PointInRectFloat(&m, &r)) {
                        sel = i;
                        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) action = b[i].action;
                    }
                }
            }
        }
        if (action && act(st, action)) {
            left = st == ST_CHECKING;
            break;
        }
        if (shown) draw(rd, st, b, nb, state_at);
        else SDL_Delay(16);
    }
    if (rd) SDL_DestroyRenderer(rd);
    if (win) SDL_DestroyWindow(win);
    SDL_Quit();
    if (left) {  // (it stops before downloading anything)
        SDL_SetAtomicInt(&cancel, 1), SDL_SetAtomicInt(&answer, 2);
        SDL_DetachThread(th);
        return false;
    }
    if (SDL_GetAtomicInt(&state) == ST_ASK) SDL_SetAtomicInt(&answer, 2);
    SDL_WaitThread(th, NULL);
    return true;
}

// ---- starting the game, with the arguments given here (the updater's own left out)
static int launch(int argc, char **argv) {
    char exe[1100];
    path(exe, sizeof exe, GAME_EXE);
    ulog("[update] starting %s\n", exe);
    SDL_LockMutex(lock);
    if (logf) SDL_CloseIO(logf), logf = NULL;
    SDL_UnlockMutex(lock);
#ifdef _WIN32
    // (the command line, quoted as the C runtime reads it back)
    size_t cap = 4096, n = 0;
    char *cmd = malloc(cap);
    for (int i = 0; i <= argc && cmd; i++) {
        const char *a = i == 0 ? exe : argv[i - 1];
        if (cap - n < strlen(a) * 2 + 8) cmd = realloc(cmd, cap = cap * 2 + strlen(a) * 2 + 8);
        if (!cmd) break;
        if (i) cmd[n++] = ' ';
        bool quote = !*a || strpbrk(a, " \t\"");
        if (quote) cmd[n++] = '"';
        for (const char *p = a;; p++) {
            size_t bs = 0;
            while (*p == '\\') bs++, p++;
            if (!*p) { for (size_t k = 0; k < (quote ? bs * 2 : bs); k++) cmd[n++] = '\\'; break; }
            if (*p == '"') { for (size_t k = 0; k < bs * 2 + 1; k++) cmd[n++] = '\\'; }
            else for (size_t k = 0; k < bs; k++) cmd[n++] = '\\';
            cmd[n++] = *p;
        }
        if (quote) cmd[n++] = '"';
    }
    if (!cmd) return 1;
    cmd[n] = 0;
    int wn = MultiByteToWideChar(CP_UTF8, 0, cmd, -1, NULL, 0);
    wchar_t *wcmd = calloc(wn, sizeof *wcmd), wexe[1100];
    MultiByteToWideChar(CP_UTF8, 0, cmd, -1, wcmd, wn);
    MultiByteToWideChar(CP_UTF8, 0, exe, -1, wexe, 1100);
    free(cmd);
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    // (no console window: the game writes log.txt then, as when started from Explorer)
    if (!CreateProcessW(wexe, wcmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        char msg[1300];
        snprintf(msg, sizeof msg, "Can't start %s (error %lu).", exe, GetLastError());
        MessageBoxA(NULL, msg, "MHGU Recomp updater", MB_ICONERROR);
        return 1;
    }
    free(wcmd);
    // (waiting for it: Steam and other launchers see the game run as long as it does)
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess), CloseHandle(pi.hThread);
    return (int)code;
#else
    char **args = calloc((size_t)argc + 2, sizeof *args);
    if (!args) return 1;
    args[0] = exe;
    for (int i = 0; i < argc; i++) args[i + 1] = argv[i];
    execv(exe, args);  // (the same process: Steam sees the game as what it started)
    fprintf(stderr, "can't start %s: %s\n", exe, strerror(errno));
    return 1;
#endif
}

int main(int argc, char **argv) {
    char **pass = calloc((size_t)argc + 1, sizeof *pass);
    int npass = 0;
    bool launch_game = true;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--update-force")) force = true;
        else if (!strcmp(argv[i], "--update-no-launch")) launch_game = false;
        else pass[npass++] = argv[i];
    }
    lock = SDL_CreateMutex();
    const char *base = SDL_GetBasePath();
    if (!base) return 1;
    snprintf(root, sizeof root, "%s", base);
    char p[1100];
    path(p, sizeof p, "update");
    SDL_CreateDirectory(p);
    path(p, sizeof p, "update/updater.log");
    logf = SDL_IOFromFile(p, "w");
    ulog("[update] MHGU Recomp updater %s in %s\n", MHGU_VERSION, root);
    // leftovers of the last update (Windows can't remove a running program's files)
    path(p, sizeof p, "update/backup");
    rm_tree(p);
    path(p, sizeof p, "update/new");
    rm_tree(p);

    path(p, sizeof p, GAME_EXE);
    if (!exists(p)) {
        ulog("[update] %s is not here\n", p);
        char msg[1300];
        snprintf(msg, sizeof msg, "%s was not found: the updater belongs in the game folder, next to it.", p);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "MHGU Recomp updater", msg, NULL);
        return 1;
    }
    mode = read_mode();
    int have[4];
    bool clean;
    if (mode == MODE_OFF) {
        ulog("[update] off in mhgurecomp.ini (Updates)\n");
    } else if ((!version(MHGU_VERSION, have, &clean) || !clean) && !force) {
        ulog("[update] %s is a development build: not updated (--update-force)\n", MHGU_VERSION);
    } else {
        SDL_Thread *th = SDL_CreateThread(worker, "update", NULL);
        if (th) run_window(th);
    }
    return launch_game && !no_launch ? launch(npass, pass) : 0;
}
