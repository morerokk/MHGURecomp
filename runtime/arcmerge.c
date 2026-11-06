// Merging the same MT Framework archive (.arc) from several mods (arcmerge.h).
//
// Mods often ship a whole archive (resident_eng.arc) for a few changed textures, so the last mod in load order
// replaces the others' changes with its untouched copy of the game's. A mod can also ship only the entries it changed,
// and not redistribute the rest of the game's. Archive: "ARC\0", u16 version, u16 count, u32
// padding, then per file 80 bytes (char name[64], u32 type, u32 compressed size, u32 size + flags, u32 offset) and the
// compressed data. An entry is told as changed from the game's by its bytes as stored, so nothing is decompressed.
#include "arcmerge.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "rt.h"

#define HEAD 12u
#define ENTRY 80u
#define ALIGN 0x8000u

typedef struct {
    char name[65];
    uint32_t type, csize, sf;
    const uint8_t *blob;
} Ent;

typedef struct {
    uint8_t *buf;
    long len;
    uint16_t version;
    int n;
    Ent *e;
} Arc;

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = v, p[1] = v >> 8, p[2] = v >> 16, p[3] = v >> 24; }

// With table_only, the entries' data isn't read (blob stays NULL)
static bool arc_open(Arc *a, const char *path, bool table_only) {
    memset(a, 0, sizeof *a);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    a->len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t head[HEAD];
    if (a->len < (long)HEAD || fread(head, 1, HEAD, f) != HEAD || memcmp(head, "ARC", 4)) { fclose(f); return false; }
    a->version = (uint16_t)(head[4] | head[5] << 8);
    a->n = head[6] | head[7] << 8;
    long table = (long)(HEAD + (uint32_t)a->n * ENTRY), keep = table_only ? table : a->len;
    if (table > a->len || !(a->buf = malloc((size_t)keep))) { fclose(f); return false; }
    memcpy(a->buf, head, HEAD);
    bool ok = fread(a->buf + HEAD, 1, (size_t)keep - HEAD, f) == (size_t)keep - HEAD;
    fclose(f);
    if (!ok || !(a->e = calloc((size_t)a->n + 1, sizeof *a->e))) return false;
    for (int i = 0; i < a->n; i++) {
        const uint8_t *p = a->buf + HEAD + (uint32_t)i * ENTRY;
        memcpy(a->e[i].name, p, 64);
        a->e[i].type = rd32(p + 64), a->e[i].csize = rd32(p + 68), a->e[i].sf = rd32(p + 72);
        uint32_t off = rd32(p + 76);
        if ((uint64_t)off + a->e[i].csize > (uint64_t)a->len) return false;
        if (!table_only) a->e[i].blob = a->buf + off;
    }
    return true;
}

static void arc_free(Arc *a) { free(a->buf), free(a->e); }

// An entry is its name and type: most archives hold several files of one name (a model, its material, its textures;
// a sound bank and its requests)
static Ent *arc_find(Arc *a, const char *name, uint32_t type) {
    for (int i = 0; i < a->n; i++)
        if (a->e[i].type == type && !strcmp(a->e[i].name, name)) return &a->e[i];
    return NULL;
}

static bool same(const Ent *a, const Ent *b) {
    return a->type == b->type && a->csize == b->csize && a->sf == b->sf && !memcmp(a->blob, b->blob, a->csize);
}

static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    for (const uint8_t *q = p; n--; q++) h = (h ^ *q) * 1099511628211ull;
    return h;
}
static uint64_t fnv_file(uint64_t h, const char *path) {
    struct stat st;
    if (stat(path, &st)) return h;
    long long v[2] = {(long long)st.st_size, (long long)st.st_mtime};
    return fnv(fnv(h, path, strlen(path)), v, sizeof v);
}

bool arc_partial(const char *orig, const char *mod) {
    Arc o = {0}, m = {0};
    int shared = 0, lacks = 0;
    if (arc_open(&o, orig, true) && arc_open(&m, mod, true) && o.version == m.version)
        for (int i = 0; i < o.n; i++) {
            if (arc_find(&m, o.e[i].name, o.e[i].type)) shared++;
            else lacks++;
        }
    arc_free(&o), arc_free(&m);
    return shared && lacks;
}

bool arc_merge(const char *orig, const char *prev, const char *mod, char *out, size_t outsz) {
    uint64_t h = 14695981039346656037ull;
    h = fnv(h, "2", 1);  // (merge version: 0.10.8's matched entries by name alone, its merges aren't to be reused)
    h = fnv_file(fnv_file(fnv_file(h, orig), prev), mod);
    snprintf(out, outsz, "cache/arcmerge/%016llx.arc", (unsigned long long)h);
    struct stat st;
    bool cached = !stat(out, &st) && st.st_size > 0;  // (the same files merged on an earlier start)
    if (cached && !strcmp(orig, prev)) return true;   // (no earlier mod to report overlaps with: nothing to read)

    Arc o = {0}, p = {0}, m = {0};
    bool ok = false;
    Ent *res = NULL;
    uint8_t *img = NULL;
    if (!arc_open(&o, orig, false) || !arc_open(&p, prev, false) || !arc_open(&m, mod, false)) goto done;
    if (o.version != p.version || o.version != m.version) goto done;

    res = calloc((size_t)p.n + (size_t)m.n + 1, sizeof *res);
    int n = p.n, changed = 0;
    if (!res) goto done;
    memcpy(res, p.e, sizeof *res * (size_t)p.n);
    for (int i = 0; i < m.n; i++) {
        Ent *base = arc_find(&o, m.e[i].name, m.e[i].type);
        if (base && same(base, &m.e[i])) continue;  // (as the game's: not this mod's change)
        Ent *r = NULL;
        for (int k = 0; k < n && !r; k++)
            if (res[k].type == m.e[i].type && !strcmp(res[k].name, m.e[i].name)) r = &res[k];
        if (r && base && !same(base, r) && !same(r, &m.e[i]))  // (an earlier mod changed it as well)
            LOG("[mods] %s changes %s, which an earlier mod changed too: this one is used. The order the mods "
                "load in (alphabetical) can be set in mods/load_order.txt\n", mod, m.e[i].name);
        if (!r) r = &res[n++];
        *r = m.e[i];
        changed++;
    }
    if (cached) { ok = true; goto done; }

    uint64_t data = ((uint64_t)HEAD + (uint64_t)n * ENTRY + ALIGN - 1) / ALIGN * ALIGN, total = data;
    for (int i = 0; i < n; i++) total += res[i].csize;
    if (total > 0x7FFFFFFFu || !(img = calloc(1, (size_t)total))) goto done;
    memcpy(img, "ARC", 4);
    img[4] = (uint8_t)o.version, img[5] = (uint8_t)(o.version >> 8), img[6] = (uint8_t)n, img[7] = (uint8_t)(n >> 8);
    uint64_t off = data;
    for (int i = 0; i < n; i++) {
        uint8_t *e = img + HEAD + (uint32_t)i * ENTRY;
        memcpy(e, res[i].name, 64);
        wr32(e + 64, res[i].type), wr32(e + 68, res[i].csize), wr32(e + 72, res[i].sf), wr32(e + 76, (uint32_t)off);
        memcpy(img + off, res[i].blob, res[i].csize);
        off += res[i].csize;
    }

    mkdir("cache", 0755);
    mkdir("cache/arcmerge", 0755);
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.tmp", out);
    FILE *f = fopen(tmp, "wb");
    if (!f) goto done;
    ok = fwrite(img, 1, (size_t)total, f) == (size_t)total;
    ok = fclose(f) == 0 && ok;
    if (ok) ok = rename(tmp, out) == 0;
    if (!ok) remove(tmp);
    else LOG("[romfs] archives merged: %s <- %d entries changed by %s\n", prev, changed, mod);
done:
    free(res), free(img);
    arc_free(&o), arc_free(&p), arc_free(&m);
    return ok;
}

int arc_overlap(const char *orig, const char *a, const char *b, char *name, size_t namesz) {
    Arc o = {0}, x = {0}, y = {0};
    int n = -1;
    if (arc_open(&o, orig, false) && arc_open(&x, a, false) && arc_open(&y, b, false) && o.version == x.version &&
        o.version == y.version) {
        n = 0;
        for (int i = 0; i < x.n; i++) {
            Ent *base = arc_find(&o, x.e[i].name, x.e[i].type), *other = arc_find(&y, x.e[i].name, x.e[i].type);
            if (!other || (base && same(base, &x.e[i])) || (base && same(base, other)) || same(&x.e[i], other)) continue;
            if (!n && name) snprintf(name, namesz, "%s", x.e[i].name);
            n++;
        }
    }
    arc_free(&o), arc_free(&x), arc_free(&y);
    return n;
}
