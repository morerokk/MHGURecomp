// Mod folders (mods.h).
#include "mods.h"
#include "modorder.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "rt.h"

static bool is_dir(const char *p) {
    struct stat st;
    return !stat(p, &st) && S_ISDIR(st.st_mode);
}

void mods_make_dir(void) {
    if (!is_dir(mods_dir())) mkdir(mods_dir(), 0755);
}

#define MAX_MODS 256
static char *g_names[MAX_MODS];
static int g_nmods = -1;

static void list_mods(void) {
    if (g_nmods >= 0) return;
    g_nmods = 0;
    ModEntry all[MODS_MAX];
    int n = mods_installed(all, MODS_MAX);
    for (int i = 0; i < n; i++)
        if (all[i].on) g_names[g_nmods++] = strdup(all[i].name);
    char path[1100];
    snprintf(path, sizeof path, "%s/load_order.txt", mods_dir());
    struct stat st;
    if (!stat(path, &st)) LOG("[mods] %d mod(s) loading, in the order of %s\n", g_nmods, path);
}

int mods_romfs_layers(const char **out, int max) {
    list_mods();
    int k = 0;
    for (int i = 0; i < g_nmods && k < max; i++) {
        char mod[1100], path[1100];
        snprintf(mod, sizeof mod, "%s/%s", mods_dir(), g_names[i]);
        if (!is_dir(mod)) { LOG("[mods] %s: no such folder in %s\n", g_names[i], mods_dir()); continue; }
        if (!mods_romfs_of(g_names[i], path, sizeof path)) continue;
        out[k++] = strdup(path);
        LOG("[mods] %s: romfs %s\n", g_names[i], path);
    }
    return k;
}

// ------------------------------------------------------------------ exefs modules
const char *mods_exefs_file(const char *name, char *path, size_t n, bool (*skip)(const char *path, const char *mod)) {
    list_mods();
    const char *found = NULL;
    for (int i = 0; i < g_nmods; i++) {
        char mod[1100], ex[1100], p[1300];
        struct stat st;
        snprintf(mod, sizeof mod, "%s/%s", mods_dir(), g_names[i]);
        if (!mods_find_dir(mod, "exefs", 3, ex, sizeof ex)) continue;
        snprintf(p, sizeof p, "%s/%s", ex, name);
        if (stat(p, &st) || !S_ISREG(st.st_mode) || (skip && skip(p, g_names[i]))) continue;
        if (found) LOG("[mods] %s and %s both have exefs/%s: %s's is used\n", found, g_names[i], name, g_names[i]);
        found = g_names[i];
        snprintf(path, n, "%s", p);
    }
    return found;
}

// ------------------------------------------------------------------ exefs patches
typedef struct {
    Module *m;
    const char *mod, *file;
    int data, code, bad;
} Apply;

// Patches of main's or sdk's code, for mods_patch_code
typedef struct { Module *m; uint32_t off, len; uint8_t *bytes; const char *mod, *file; } Pending;
static Pending *pending;
static int npending, pending_cap;

// One patch: `len` bytes at offset `off` of the module's memory image.
static void apply(Apply *a, uint32_t off, const uint8_t *bytes, uint32_t len) {
    Module *m = a->m;
    if (!len) return;
    if ((uint64_t)off + len > m->size) { a->bad++; return; }
    bool compiled = m == &g_modules[0] || m == &g_modules[1];  // (a subsdk's code is interpreted: it just changes)
    if (compiled && off < m->text_off + m->text_size && off + len > m->text_off) {  // code: see mods_patch_code
        if (npending == pending_cap) {
            pending_cap = pending_cap ? 2 * pending_cap : 256;
            pending = realloc(pending, pending_cap * sizeof *pending);
        }
        Pending *p = &pending[npending++];
        p->m = m, p->off = off, p->len = len, p->bytes = malloc(len), p->mod = a->mod, p->file = a->file;
        memcpy(p->bytes, bytes, len);
        a->code++;
        return;
    }
    memcpy(G2H(m->base + off), bytes, len);
    a->data++;
}

static uint32_t ror32(uint32_t v, unsigned r) { return r ? v >> r | v << (32 - r) : v; }
// An ARM data-processing immediate (8 bits rotated right by an even amount) for v
static bool arm_imm(uint32_t v, uint32_t *enc) {
    for (unsigned rot = 0; rot < 16; rot++) {
        uint32_t x = rot ? v << (2 * rot) | v >> (32 - 2 * rot) : v;
        if (x < 256) { *enc = rot << 8 | x; return true; }
    }
    return false;
}

// A patched word at a that leads into a mod's subsdk the loader placed elsewhere than the game's layout (loader_moved):
// a B / BL, an `add` / `sub pc, pc, #imm` (MH4U Port's hooks) or a pc-relative offset (a literal the code adds to
// pc, from up to 4 KiB before it) moved by the same difference. 1: moved, 0: not one of those, -1: can't be encoded.
static int retarget(uint32_t a, uint32_t *w) {
    uint32_t v = *w, enc;
    int32_t shift;
    if (v >> 28 != 0xF && (v & 0x0E000000) == 0x0A000000) {
        uint32_t t = a + 8 + (uint32_t)((int32_t)(v << 8) >> 6);
        if (!loader_moved(t, 0, &shift)) return 0;
        int32_t d = (int32_t)(t + (uint32_t)shift - (a + 8));
        if (d < -0x2000000 || d >= 0x2000000) return -1;
        *w = (v & 0xFF000000) | ((uint32_t)d >> 2 & 0xFFFFFF);
        return 1;
    }
    if ((v & 0x0FFFF000) == 0x028FF000 || (v & 0x0FFFF000) == 0x024FF000) {
        uint32_t imm = ror32(v & 0xFF, 2 * (v >> 8 & 15));
        uint32_t t = a + 8 + ((v & 0x0FFFF000) == 0x028FF000 ? imm : 0u - imm);
        if (!loader_moved(t, 0, &shift)) return 0;
        int32_t d = (int32_t)(t + (uint32_t)shift - (a + 8));
        if (!arm_imm(d < 0 ? 0u - (uint32_t)d : (uint32_t)d, &enc)) return -1;
        *w = (v & 0xF0000000) | (d < 0 ? 0x024FF000 : 0x028FF000) | enc;
        return 1;
    }
    if (!loader_moved(a + v, 0x1000, &shift)) return 0;
    *w = v + (uint32_t)shift;
    return 1;
}

// The patches of main's and sdk's code: the bytes go into memory now (jumps and offsets into a subsdk retargeted),
// and every function they touch runs in the interpreter from now on (dispatch_interpret, as a cheat's changes do),
// which reads them there.
void mods_patch_code(void) {
    int words = 0, fns = 0, moved = 0, failed = 0;
    for (int i = 0; i < npending; i++) {
        Pending *p = &pending[i];
        const Module *m = p->m;
        uint32_t a = m->base + p->off;
        for (uint32_t k = (4 - (a & 3)) & 3; k + 4 <= p->len; k += 4) {
            uint32_t w;
            memcpy(&w, p->bytes + k, 4);
            int r = retarget(a + k, &w);
            if (r < 0) LOG("[mods] %s: %s: the jump at %s+%x into a moved subsdk can't be encoded there (left as it is)\n",
                           p->mod, p->file, m->name, a + k - m->base);
            moved += r > 0, failed += r < 0;
            memcpy(p->bytes + k, &w, 4);
        }
        memcpy(G2H(a), p->bytes, p->len);
        uint32_t last = 0;
        for (uint32_t w = a & ~3u; w < a + p->len; w += 4, words++) {
            uint32_t next, entry = dispatch_function_at(w, &next);
            if (!entry || entry == last) continue;
            last = entry;
            int r = dispatch_interpret(entry, next ? next : m->base + m->text_off + m->text_size);
            fns += r > 0;
            if (r < 0) LOG("[mods] %s: %s: the function at %s+%x can't run in the interpreter: the patch at %s+%x has no effect\n",
                           p->mod, p->file, m->name, entry - m->base, m->name, a - m->base);
        }
        free(p->bytes);
        bool last_of_file = i + 1 == npending || pending[i + 1].file != p->file;
        if (!last_of_file) continue;
        LOG("[mods] %s: %s: %d word(s) of code patched, %d function(s) run in the interpreter now", p->mod, p->file,
            words, fns);
        if (moved || failed) LOG(", %d jump(s) / offset(s) into a moved subsdk retargeted%s", moved, failed ? " (some failed)" : "");
        LOG("\n");
        words = fns = moved = failed = 0;
    }
    free(pending);
    pending = NULL, npending = pending_cap = 0;
}

static int hexval(int c) { return isdigit(c) ? c - '0' : isxdigit(c) ? (tolower(c) - 'a' + 10) : -1; }

// Leading hex digits of `s` as a build id prefix: does it match `id`? (Patches name or tag a shortened id.)
static bool id_matches(const char *s, const uint8_t id[32]) {
    int n = 0;
    for (; hexval(s[n]) >= 0 && n < 64; n++)
        if (hexval(s[n]) != ((id[n / 2] >> (n & 1 ? 0 : 4)) & 15)) return false;
    return n >= 16;  // (at least 8 bytes of the id)
}

// IPSwitch text patches: "@nsobid-<id>", "@flag offset_shift <n>", "@little-endian" / "@big-endian",
// "@enabled" / "@disabled" blocks of "<hex offset> <hex bytes | "string">" lines, "@stop", // comments.
// Offsets + offset_shift are IPS offsets (the NSO file's 0x100-byte header included).
static void pchtxt(Apply *a, const char *path, const uint8_t id[32]) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[4096];
    bool ours = false, on = false, le = false;
    long shift = 0;
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        while (isspace((unsigned char)*s)) s++;
        char *cm = strstr(s, "//");
        if (cm) *cm = 0;
        s[strcspn(s, "\r\n")] = 0;
        if (!*s) continue;
        if (!strncasecmp(s, "@nsobid-", 8)) { ours = id_matches(s + 8, id); continue; }
        if (!strncasecmp(s, "@flag", 5)) {
            char key[64];
            long v;
            if (sscanf(s + 5, "%63s %li", key, &v) == 2 && !strcasecmp(key, "offset_shift")) shift = v;
            continue;
        }
        if (!strncasecmp(s, "@little-endian", 14)) { le = true; continue; }
        if (!strncasecmp(s, "@big-endian", 11)) { le = false; continue; }
        if (!strncasecmp(s, "@enabled", 8)) { on = true; continue; }
        if (!strncasecmp(s, "@disabled", 9)) { on = false; continue; }
        if (!strncasecmp(s, "@stop", 5)) break;
        if (*s == '@' || !ours || !on) continue;
        char *end;
        unsigned long off = strtoul(s, &end, 16);
        if (end == s || !isspace((unsigned char)*end)) continue;
        while (isspace((unsigned char)*end)) end++;
        uint8_t buf[2048];
        uint32_t n = 0;
        if (*end == '"') {  // a string, written without its terminator
            for (char *q = end + 1; *q && *q != '"' && n < sizeof buf; q++) {
                if (*q == '\\' && q[1]) {
                    q++;
                    buf[n++] = *q == 'n' ? '\n' : *q == 't' ? '\t' : *q == '0' ? 0 : (uint8_t)*q;
                } else {
                    buf[n++] = (uint8_t)*q;
                }
            }
        } else {
            for (char *q = end; hexval(q[0]) >= 0 && hexval(q[1]) >= 0 && n < sizeof buf; q += 2)
                buf[n++] = (uint8_t)(hexval(q[0]) << 4 | hexval(q[1]));
            if (le)
                for (uint32_t i = 0; i < n / 2; i++) { uint8_t t = buf[i]; buf[i] = buf[n - 1 - i]; buf[n - 1 - i] = t; }
        }
        long ips = (long)off + shift;
        if (ips < 0x100) { a->bad++; continue; }
        apply(a, (uint32_t)(ips - 0x100), buf, n);
    }
    fclose(f);
}

// IPS ("PATCH", 24-bit offsets, "EOF") and IPS32 ("IPS32", 32-bit offsets, "EEOF"); offsets include the NSO's
// 0x100-byte header. A size of 0 is a run of one byte.
static void ips(Apply *a, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(len > 0 ? len : 1);
    long got = (long)fread(b, 1, len, f);
    fclose(f);
    bool i32 = got >= 5 && !memcmp(b, "IPS32", 5);
    if (!i32 && (got < 5 || memcmp(b, "PATCH", 5))) { a->bad++; free(b); return; }
    int ow = i32 ? 4 : 3;
    long p = 5;
    while (p + ow <= got) {
        if (!memcmp(b + p, i32 ? "EEOF" : "EOF", ow)) break;
        uint32_t off = 0;
        for (int i = 0; i < ow; i++) off = off << 8 | b[p + i];
        p += ow;
        if (p + 2 > got) break;
        uint32_t size = (uint32_t)b[p] << 8 | b[p + 1];
        p += 2;
        uint8_t *data = b + p, run[0x10000];
        if (!size) {
            if (p + 3 > got) break;
            size = (uint32_t)b[p] << 8 | b[p + 1];
            memset(run, b[p + 2], size);
            data = run;
            p += 3;
        } else {
            if (p + size > got) break;
            p += size;
        }
        if (off < 0x100) { a->bad++; continue; }
        apply(a, off - 0x100, data, size);
    }
    free(b);
}

void mods_patch_module(Module *m, const uint8_t build_id[32]) {
    list_mods();
    for (int i = 0; i < g_nmods; i++) {
        char mod[1100], ex[1100];
        snprintf(mod, sizeof mod, "%s/%s", mods_dir(), g_names[i]);
        if (!mods_find_dir(mod, "exefs", 3, ex, sizeof ex)) continue;
        DIR *d = opendir(ex);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d))) {
            const char *dot = strrchr(e->d_name, '.');
            if (!dot) continue;
            char path[1300];
            snprintf(path, sizeof path, "%s/%s", ex, e->d_name);
            Apply a = {m, g_names[i], strdup(e->d_name), 0, 0, 0};  // (the name stays: mods_patch_code logs it)
            if (!strcasecmp(dot, ".pchtxt")) pchtxt(&a, path, build_id);
            else if (!strcasecmp(dot, ".ips") && id_matches(e->d_name, build_id)) ips(&a, path);
            if (!a.data && !a.code && !a.bad) {  // (for another module or version)
                free((char *)a.file);
                continue;
            }
            LOG("[mods] %s: %s for %s: %d data patch(es) applied", a.mod, a.file, m->name, a.data);
            if (a.code) LOG(", %d code patch(es) (applied once the game's code is set up)", a.code);
            if (a.bad) LOG(", %d outside the module (ignored)", a.bad);
            LOG("\n");
        }
        closedir(d);
    }
}
