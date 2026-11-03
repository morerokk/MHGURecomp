// Virtual RomFS image (romfs.h). Layout as a real image: a 0x50-byte header, file data from 0x200 (each file
// 16-byte aligned), then the directory hash table, directory entries, file hash table and file entries. The game's
// RomFS code walks the tables itself, finding entries by hash, so their order only has to be consistent.
#include "romfs.h"

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "arcmerge.h"
#include "rt.h"

#define EMPTY 0xFFFFFFFFu
#define DATA_START 0x200u

typedef struct Dir Dir;
typedef struct {
    char *name, *path;  // path: the host file
    char *orig;         // the game's own file, once a mod has replaced it
    uint64_t size, data_off;
    uint32_t off;
    Dir *parent;
    char *rpath;        // path inside the RomFS
} File;
struct Dir {
    char *name;
    Dir *parent;
    Dir **dirs;
    File **files;
    int ndirs, nfiles, cdirs, cfiles;
    uint32_t off;
};

static int by_name_dir(const void *a, const void *b) { return strcmp((*(Dir *const *)a)->name, (*(Dir *const *)b)->name); }
static int by_name_file(const void *a, const void *b) { return strcmp((*(File *const *)a)->name, (*(File *const *)b)->name); }

static void add_dir(Dir *d, Dir *c) {
    if (d->ndirs == d->cdirs) d->dirs = realloc(d->dirs, sizeof *d->dirs * (d->cdirs = d->cdirs ? d->cdirs * 2 : 8));
    d->dirs[d->ndirs++] = c;
    c->parent = d;
}
static void add_file(Dir *d, File *f) {
    if (d->nfiles == d->cfiles) d->files = realloc(d->files, sizeof *d->files * (d->cfiles = d->cfiles ? d->cfiles * 2 : 16));
    d->files[d->nfiles++] = f;
    f->parent = d;
}

static Dir *scan(const char *path, const char *name, int depth) {
    if (depth > 32) return NULL;
    DIR *dd = opendir(path);
    if (!dd) return NULL;
    Dir *dir = calloc(1, sizeof *dir);
    dir->name = strdup(name);
    struct dirent *e;
    while ((e = readdir(dd))) {
        if (e->d_name[0] == '.') continue;
        size_t n = strlen(path) + strlen(e->d_name) + 2;
        char *p = malloc(n);
        snprintf(p, n, "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) { free(p); continue; }
        if (S_ISDIR(st.st_mode)) {
            Dir *c = scan(p, e->d_name, depth + 1);
            free(p);
            if (c) add_dir(dir, c);
        } else if (S_ISREG(st.st_mode)) {
            File *f = calloc(1, sizeof *f);
            f->name = strdup(e->d_name), f->path = p, f->size = (uint64_t)st.st_size;
            add_file(dir, f);
        } else {
            free(p);
        }
    }
    closedir(dd);
    return dir;
}

// overlay `src` onto `dst`: its files replace or join dst's, its folders merge. Names match in any case (a mod's
// nativenx/ is the game's nativeNX/: Windows doesn't tell them apart either); the game's own spelling stays.
static int merge(Dir *dst, Dir *src) {
    int changed = 0;
    for (int i = 0; i < src->nfiles; i++) {
        File *f = src->files[i];
        int k = 0;
        while (k < dst->nfiles && strcasecmp(dst->files[k]->name, f->name)) k++;
        if (k < dst->nfiles) {
            File *d = dst->files[k];
            size_t nl = strlen(f->name);
            char merged[1200];
            // (two mods' archives: both's changes. One with only some of the game's entries: those over the rest)
            if (nl > 4 && !strcasecmp(f->name + nl - 4, ".arc") && (d->orig || arc_partial(d->path, f->path)) &&
                arc_merge(d->orig ? d->orig : d->path, d->path, f->path, merged, sizeof merged)) {
                struct stat st;
                if (!stat(merged, &st)) {
                    if (!d->orig) d->orig = strdup(d->path);
                    free(d->path);
                    d->path = strdup(merged);
                    d->size = (uint64_t)st.st_size;
                    changed++;
                    continue;
                }
            }
            if (!d->orig) d->orig = strdup(d->path);
            free(d->path);
            d->path = strdup(f->path);
            d->size = f->size;
        } else {
            File *n = calloc(1, sizeof *n);
            n->name = strdup(f->name), n->path = strdup(f->path), n->size = f->size;
            add_file(dst, n);
        }
        changed++;
    }
    for (int i = 0; i < src->ndirs; i++) {
        Dir *s = src->dirs[i];
        int k = 0;
        while (k < dst->ndirs && strcasecmp(dst->dirs[k]->name, s->name)) k++;
        if (k == dst->ndirs) {
            Dir *n = calloc(1, sizeof *n);
            n->name = strdup(s->name);
            add_dir(dst, n);
        }
        changed += merge(dst->dirs[k], s);
    }
    return changed;
}

static void sort_tree(Dir *d) {
    if (d->ndirs) qsort(d->dirs, d->ndirs, sizeof *d->dirs, by_name_dir);
    if (d->nfiles) qsort(d->files, d->nfiles, sizeof *d->files, by_name_file);
    for (int i = 0; i < d->ndirs; i++) sort_tree(d->dirs[i]);
}

typedef struct { Dir **dirs; File **files; int nd, nf, cd, cf; } Lists;
static void flatten(Dir *d, Lists *l) {
    for (int i = 0; i < d->nfiles; i++) {
        if (l->nf == l->cf) l->files = realloc(l->files, sizeof *l->files * (l->cf = l->cf ? l->cf * 2 : 256));
        l->files[l->nf++] = d->files[i];
    }
    for (int i = 0; i < d->ndirs; i++) {
        if (l->nd == l->cd) l->dirs = realloc(l->dirs, sizeof *l->dirs * (l->cd = l->cd ? l->cd * 2 : 64));
        l->dirs[l->nd++] = d->dirs[i];
    }
    for (int i = 0; i < d->ndirs; i++) flatten(d->dirs[i], l);
}

static uint32_t name_hash(uint32_t parent, const char *name) {
    uint32_t h = parent ^ 123456789u;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) h = ((h >> 5) | (h << 27)) ^ *p;
    return h;
}
static uint32_t bucket_count(uint32_t n) {
    if (n < 3) return 3;
    if (n < 19) return n | 1;
    uint32_t c = n;
    while (c % 2 == 0 || c % 3 == 0 || c % 5 == 0 || c % 7 == 0 || c % 11 == 0 || c % 13 == 0 || c % 17 == 0) c++;
    return c;
}
static uint32_t align4(uint32_t x) { return (x + 3) & ~3u; }
static uint64_t align16(uint64_t x) { return (x + 15) & ~15ull; }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

// ------------------------------------------------------------------ the image
static uint8_t g_hdr[DATA_START];
static uint8_t *g_tables;          // dir hash, dir meta, file hash, file meta (contiguous)
static uint64_t g_tables_off, g_tables_size, g_size;
static File **g_files;             // by data offset
static int g_nfiles;

static char *rel_path(File *f) {
    char *parts[40];
    int n = 0;
    for (Dir *d = f->parent; d && d->parent && n < 40; d = d->parent) parts[n++] = d->name;
    size_t len = strlen(f->name) + 1;
    for (int i = 0; i < n; i++) len += strlen(parts[i]) + 1;
    char *s = malloc(len);
    s[0] = 0;
    for (int i = n - 1; i >= 0; i--) { strcat(s, parts[i]); strcat(s, "/"); }
    strcat(s, f->name);
    return s;
}

bool romfs_open(const char *base_dir, const char *const *overlays, int noverlays) {
    Dir *root = scan(base_dir, "", 0);
    if (!root) { LOG("[romfs] cannot read %s\n", base_dir); return false; }
    for (int i = 0; i < noverlays; i++) {
        Dir *o = scan(overlays[i], "", 0);
        if (!o) continue;
        int n = merge(root, o);
        LOG("[romfs] mod %s: %d files\n", overlays[i], n);
    }
    sort_tree(root);
    Lists l = {0};
    l.dirs = malloc(sizeof *l.dirs * (l.cd = 64));
    l.dirs[l.nd++] = root;
    flatten(root, &l);

    uint32_t dsize = 0, fsize = 0;
    for (int i = 0; i < l.nd; i++) l.dirs[i]->off = dsize, dsize += 0x18 + align4((uint32_t)strlen(l.dirs[i]->name));
    for (int i = 0; i < l.nf; i++) l.files[i]->off = fsize, fsize += 0x20 + align4((uint32_t)strlen(l.files[i]->name));
    uint64_t data = 0;
    for (int i = 0; i < l.nf; i++) {
        data = align16(data);
        l.files[i]->data_off = data;
        l.files[i]->rpath = rel_path(l.files[i]);
        data += l.files[i]->size;
    }
    uint32_t nbd = bucket_count((uint32_t)l.nd), nbf = bucket_count((uint32_t)l.nf);
    uint64_t dhash_off = (DATA_START + data + 3) & ~3ull;
    uint64_t dmeta_off = dhash_off + 4ull * nbd, fhash_off = dmeta_off + dsize, fmeta_off = fhash_off + 4ull * nbf;
    g_tables_off = dhash_off;
    g_tables_size = fmeta_off + fsize - dhash_off;
    g_tables = calloc(1, g_tables_size);
    uint32_t *dbuckets = (uint32_t *)g_tables, *fbuckets = (uint32_t *)(g_tables + (fhash_off - dhash_off));
    uint8_t *dtab = g_tables + (dmeta_off - dhash_off), *ftab = g_tables + (fmeta_off - dhash_off);
    for (uint32_t i = 0; i < nbd; i++) dbuckets[i] = EMPTY;
    for (uint32_t i = 0; i < nbf; i++) fbuckets[i] = EMPTY;
    for (int i = 0; i < l.nd; i++) {
        Dir *d = l.dirs[i];
        uint8_t *e = dtab + d->off;
        uint32_t parent = d->parent ? d->parent->off : 0, nl = (uint32_t)strlen(d->name), sib = EMPTY;
        if (d->parent)
            for (int k = 0; k + 1 < d->parent->ndirs; k++)
                if (d->parent->dirs[k] == d) sib = d->parent->dirs[k + 1]->off;
        uint32_t h = name_hash(parent, d->name) % nbd;
        put32(e, parent);
        put32(e + 4, sib);
        put32(e + 8, d->ndirs ? d->dirs[0]->off : EMPTY);
        put32(e + 12, d->nfiles ? d->files[0]->off : EMPTY);
        put32(e + 16, dbuckets[h]);
        put32(e + 20, nl);
        memcpy(e + 0x18, d->name, nl);
        dbuckets[h] = d->off;
    }
    for (int i = 0; i < l.nf; i++) {
        File *f = l.files[i];
        uint8_t *e = ftab + f->off;
        uint32_t nl = (uint32_t)strlen(f->name), sib = EMPTY;
        for (int k = 0; k + 1 < f->parent->nfiles; k++)
            if (f->parent->files[k] == f) sib = f->parent->files[k + 1]->off;
        uint32_t h = name_hash(f->parent->off, f->name) % nbf;
        put32(e, f->parent->off);
        put32(e + 4, sib);
        put64(e + 8, f->data_off);
        put64(e + 16, f->size);
        put32(e + 24, fbuckets[h]);
        put32(e + 28, nl);
        memcpy(e + 0x20, f->name, nl);
        fbuckets[h] = f->off;
    }
    uint64_t hv[10] = {0x50, dhash_off, 4ull * nbd, dmeta_off, dsize, fhash_off, 4ull * nbf, fmeta_off, fsize, DATA_START};
    memcpy(g_hdr, hv, sizeof hv);
    g_size = fmeta_off + fsize;
    g_files = l.files;  // (flatten order is data order)
    g_nfiles = l.nf;
    free(l.dirs);
    LOG("[romfs] %d files, %d folders, image %.1f GB\n", l.nf, l.nd, g_size / 1e9);
    return true;
}

uint64_t romfs_size(void) { return g_size; }

// ---- host file descriptors (a small cache: the image has tens of thousands of files)
#define FDC 64
static struct { File *f; int fd; uint64_t used; } fdc[FDC];
static uint64_t fdc_clock;
static pthread_mutex_t fdc_lock = PTHREAD_MUTEX_INITIALIZER;

static int64_t read_file(File *f, uint8_t *dst, uint64_t off, uint64_t n) {
    pthread_mutex_lock(&fdc_lock);
    int slot = -1, oldest = 0;
    for (int i = 0; i < FDC; i++) {
        if (fdc[i].f == f) { slot = i; break; }
        if (fdc[i].used < fdc[oldest].used) oldest = i;
    }
    if (slot < 0) {
        slot = oldest;
        if (fdc[slot].f) close(fdc[slot].fd);
        fdc[slot].f = f;
        fdc[slot].fd = open(f->path, O_RDONLY | O_BINARY);
        if (fdc[slot].fd < 0) LOG("[romfs] cannot open %s\n", f->path);
    }
    fdc[slot].used = ++fdc_clock;
    int fd = fdc[slot].fd;
    pthread_mutex_unlock(&fdc_lock);  // (pread doesn't move the file position: safe while others use it)
    if (fd < 0) return -1;
    return pread(fd, dst, n, (off_t)off);
}

static int file_at(uint64_t rel) {  // index of the file whose padded data range holds rel, or -1
    int lo = 0, hi = g_nfiles - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_files[mid]->data_off <= rel) { best = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    return best;
}

const char *romfs_file_at(uint64_t off) {
    if (off < DATA_START) return NULL;
    int i = file_at(off - DATA_START);
    return i >= 0 && off - DATA_START < g_files[i]->data_off + g_files[i]->size ? g_files[i]->rpath : NULL;
}

const char *romfs_host_path(const char *rpath) {
    while (*rpath == '/') rpath++;
    for (int i = 0; i < g_nfiles; i++)  // (rare: movies)
        if (!strcasecmp(g_files[i]->rpath, rpath)) return g_files[i]->path;
    return NULL;
}

uint64_t romfs_read(uint8_t *dst, uint64_t off, uint64_t size) {
    if (off >= g_size) return 0;
    if (off + size > g_size) size = g_size - off;
    uint64_t done = 0;
    while (done < size) {
        uint64_t o = off + done, n = size - done;
        if (o < DATA_START) {  // header
            if (n > DATA_START - o) n = DATA_START - o;
            memcpy(dst + done, g_hdr + o, n);
        } else if (o >= g_tables_off) {  // tables
            memcpy(dst + done, g_tables + (o - g_tables_off), n);
        } else {  // file data (and the padding between files)
            if (o + n > g_tables_off) n = g_tables_off - o;
            uint64_t rel = o - DATA_START;
            int i = file_at(rel);
            File *f = i >= 0 ? g_files[i] : NULL;
            if (f && rel < f->data_off + f->size) {
                uint64_t in = rel - f->data_off, avail = f->size - in;
                if (n > avail) n = avail;
                int64_t r = read_file(f, dst + done, in, n);
                if (r < 0) r = 0;
                if ((uint64_t)r < n) memset(dst + done + r, 0, n - r);
            } else {  // padding up to the next file
                uint64_t next = i + 1 < g_nfiles ? g_files[i + 1]->data_off : g_tables_off - DATA_START;
                if (n > next - rel) n = next - rel;
                memset(dst + done, 0, n);
            }
        }
        done += n;
    }
    return done;
}
