// Game files from NSP files: see nspimport.h. The formats, bottom up: NSP = PFS0 archive of .nca / .tik files; NCA = a
// 0xC00 header (AES-XTS) and up to four sections (AES-CTR) of the Program NCA: 0 ExeFS (a PFS0), 1 RomFS (an IVFC hash
// tree around the RomFS image) or, in an update, a BKTR patch of the base game's RomFS (relocation table: which ranges
// come from the base and which from the patch; subsection table: the AES-CTR counter for each range of the patch).
#define _FILE_OFFSET_BITS 64
#include "nspimport.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

#define PROGRAM_ID_BASE 0x0100770008DD8000ull    // Monster Hunter Generations Ultimate
#define PROGRAM_ID_UPDATE 0x0100770008DD8800ull  // (the update's title id: the base's with bit 11 set)

static char g_err[600];
static bool fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err, sizeof g_err, fmt, ap);
    va_end(ap);
    return false;
}

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void put_be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (uint8_t)v; }
static void put_be64(uint8_t *p, uint64_t v) { put_be32(p, (uint32_t)(v >> 32)); put_be32(p + 4, (uint32_t)v); }

// ------------------------------------------------------------------ AES-128
// (table-driven encryption for the AES-CTR that decrypts gigabytes; decryption only for the few keys and headers)
static uint8_t g_sbox[256], g_isbox[256];
static uint32_t g_te0[256], g_te1[256], g_te2[256], g_te3[256];
static bool g_aes_ready;

static uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }
static uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    while (b) {
        if (b & 1) r ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return r;
}
static uint8_t rotl8(uint8_t x, int n) { return (uint8_t)((x << n) | (x >> (8 - n))); }

static void aes_tables(void) {
    if (g_aes_ready) return;
    uint8_t p = 1, q = 1;
    do {
        p = (uint8_t)(p ^ (uint8_t)(p << 1) ^ ((p & 0x80) ? 0x1b : 0));  // p *= 3
        q ^= (uint8_t)(q << 1);                                          // q /= 3
        q ^= (uint8_t)(q << 2);
        q ^= (uint8_t)(q << 4);
        if (q & 0x80) q ^= 0x09;
        g_sbox[p] = (uint8_t)(q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^ rotl8(q, 4) ^ 0x63);
    } while (p != 1);
    g_sbox[0] = 0x63;
    for (int i = 0; i < 256; i++) g_isbox[g_sbox[i]] = (uint8_t)i;
    for (int i = 0; i < 256; i++) {
        uint8_t s = g_sbox[i];
        uint32_t w = (uint32_t)gmul(s, 2) << 24 | (uint32_t)s << 16 | (uint32_t)s << 8 | gmul(s, 3);
        g_te0[i] = w;
        g_te1[i] = w >> 8 | w << 24;
        g_te2[i] = w >> 16 | w << 16;
        g_te3[i] = w >> 24 | w << 8;
    }
    g_aes_ready = true;
}

typedef struct {
    uint32_t ek[44];
    uint8_t rk[176];
} Aes;

static void aes_set_key(Aes *a, const uint8_t key[16]) {
    aes_tables();
    memcpy(a->rk, key, 16);
    uint8_t rcon = 1;
    for (int i = 16; i < 176; i += 4) {
        uint8_t t[4];
        memcpy(t, a->rk + i - 4, 4);
        if (i % 16 == 0) {
            uint8_t t0 = t[0];
            t[0] = (uint8_t)(g_sbox[t[1]] ^ rcon);
            t[1] = g_sbox[t[2]];
            t[2] = g_sbox[t[3]];
            t[3] = g_sbox[t0];
            rcon = xtime(rcon);
        }
        for (int j = 0; j < 4; j++) a->rk[i + j] = (uint8_t)(a->rk[i - 16 + j] ^ t[j]);
    }
    for (int i = 0; i < 44; i++) a->ek[i] = be32(a->rk + 4 * i);
}

static void aes_encrypt_block(const Aes *a, const uint8_t in[16], uint8_t out[16]) {
    const uint32_t *k = a->ek;
    uint32_t s0 = be32(in) ^ k[0], s1 = be32(in + 4) ^ k[1], s2 = be32(in + 8) ^ k[2], s3 = be32(in + 12) ^ k[3];
    for (int r = 1; r < 10; r++) {
        k += 4;
        uint32_t t0 = g_te0[s0 >> 24] ^ g_te1[(s1 >> 16) & 255] ^ g_te2[(s2 >> 8) & 255] ^ g_te3[s3 & 255] ^ k[0];
        uint32_t t1 = g_te0[s1 >> 24] ^ g_te1[(s2 >> 16) & 255] ^ g_te2[(s3 >> 8) & 255] ^ g_te3[s0 & 255] ^ k[1];
        uint32_t t2 = g_te0[s2 >> 24] ^ g_te1[(s3 >> 16) & 255] ^ g_te2[(s0 >> 8) & 255] ^ g_te3[s1 & 255] ^ k[2];
        uint32_t t3 = g_te0[s3 >> 24] ^ g_te1[(s0 >> 16) & 255] ^ g_te2[(s1 >> 8) & 255] ^ g_te3[s2 & 255] ^ k[3];
        s0 = t0; s1 = t1; s2 = t2; s3 = t3;
    }
    k += 4;
    put_be32(out, ((uint32_t)g_sbox[s0 >> 24] << 24 | (uint32_t)g_sbox[(s1 >> 16) & 255] << 16 |
                   (uint32_t)g_sbox[(s2 >> 8) & 255] << 8 | g_sbox[s3 & 255]) ^ k[0]);
    put_be32(out + 4, ((uint32_t)g_sbox[s1 >> 24] << 24 | (uint32_t)g_sbox[(s2 >> 16) & 255] << 16 |
                       (uint32_t)g_sbox[(s3 >> 8) & 255] << 8 | g_sbox[s0 & 255]) ^ k[1]);
    put_be32(out + 8, ((uint32_t)g_sbox[s2 >> 24] << 24 | (uint32_t)g_sbox[(s3 >> 16) & 255] << 16 |
                       (uint32_t)g_sbox[(s0 >> 8) & 255] << 8 | g_sbox[s1 & 255]) ^ k[2]);
    put_be32(out + 12, ((uint32_t)g_sbox[s3 >> 24] << 24 | (uint32_t)g_sbox[(s0 >> 16) & 255] << 16 |
                        (uint32_t)g_sbox[(s1 >> 8) & 255] << 8 | g_sbox[s2 & 255]) ^ k[3]);
}

static void add_round_key(uint8_t s[16], const uint8_t *rk) {
    for (int i = 0; i < 16; i++) s[i] ^= rk[i];
}
static void aes_decrypt_block(const Aes *a, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16], t[16];
    memcpy(s, in, 16);
    add_round_key(s, a->rk + 160);
    for (int round = 9; round >= 0; round--) {
        // InvShiftRows (column-major state: s[4 * column + row]; row r moves right by r) and InvSubBytes
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++) t[4 * ((c + r) & 3) + r] = g_isbox[s[4 * c + r]];
        add_round_key(t, a->rk + 16 * round);
        if (round == 0) break;
        for (int c = 0; c < 4; c++) {  // InvMixColumns
            const uint8_t *x = t + 4 * c;
            s[4 * c + 0] = (uint8_t)(gmul(x[0], 14) ^ gmul(x[1], 11) ^ gmul(x[2], 13) ^ gmul(x[3], 9));
            s[4 * c + 1] = (uint8_t)(gmul(x[0], 9) ^ gmul(x[1], 14) ^ gmul(x[2], 11) ^ gmul(x[3], 13));
            s[4 * c + 2] = (uint8_t)(gmul(x[0], 13) ^ gmul(x[1], 9) ^ gmul(x[2], 14) ^ gmul(x[3], 11));
            s[4 * c + 3] = (uint8_t)(gmul(x[0], 11) ^ gmul(x[1], 13) ^ gmul(x[2], 9) ^ gmul(x[3], 14));
        }
    }
    memcpy(out, t, 16);
}

// AES-ECB decrypt of n bytes (a multiple of 16) with a 16-byte key.
static void ecb_decrypt(const uint8_t key[16], const uint8_t *in, uint8_t *out, size_t n) {
    Aes a;
    aes_set_key(&a, key);
    for (size_t i = 0; i < n; i += 16) aes_decrypt_block(&a, in + i, out + i);
}

// AES-XTS decrypt, Nintendo's way: the tweak is the sector number big-endian (not the IEEE little-endian one).
static void xts_decrypt(const uint8_t key[32], uint8_t *buf, size_t n, uint64_t sector, size_t sector_size) {
    Aes data, tweak_aes;
    aes_set_key(&data, key);
    aes_set_key(&tweak_aes, key + 16);
    for (size_t pos = 0; pos < n; pos += sector_size, sector++) {
        uint8_t tw[16] = {0};
        put_be64(tw + 8, sector);
        aes_encrypt_block(&tweak_aes, tw, tw);
        for (size_t o = 0; o < sector_size && pos + o < n; o += 16) {
            uint8_t x[16];
            for (int i = 0; i < 16; i++) x[i] = (uint8_t)(buf[pos + o + i] ^ tw[i]);
            aes_decrypt_block(&data, x, x);
            for (int i = 0; i < 16; i++) buf[pos + o + i] = (uint8_t)(x[i] ^ tw[i]);
            uint8_t carry = tw[15] >> 7;  // tw *= x in GF(2^128), byte 0 the least significant
            for (int i = 15; i > 0; i--) tw[i] = (uint8_t)(tw[i] << 1 | tw[i - 1] >> 7);
            tw[0] = (uint8_t)(tw[0] << 1 ^ (carry ? 0x87 : 0));
        }
    }
}

// AES-CTR over buf (n bytes) that sit at byte offset `pos` of the NCA. The counter: 8 bytes from the section header
// (hi), then pos / 16 big-endian. gen: replaces counter bytes 4..7 (the BKTR subsections' own value), or -1.
static void ctr_at(const Aes *a, const uint8_t hi[8], int64_t gen, uint64_t pos, uint8_t *buf, size_t n) {
    uint8_t ctr[16], ks[16];
    memcpy(ctr, hi, 8);
    if (gen >= 0) put_be32(ctr + 4, (uint32_t)gen);
    put_be64(ctr + 8, pos >> 4);
    size_t skip = (size_t)(pos & 15);
    while (n) {
        aes_encrypt_block(a, ctr, ks);
        size_t take = 16 - skip < n ? 16 - skip : n;
        for (size_t i = 0; i < take; i++) buf[i] ^= ks[skip + i];
        buf += take;
        n -= take;
        skip = 0;
        for (int i = 15; i >= 0; i--)
            if (++ctr[i]) break;
    }
}

// ------------------------------------------------------------------ prod.keys
typedef struct {
    uint8_t header_key[32];
    uint8_t master[0x20][16];
    uint8_t kak[0x20][3][16];  // key area keys: application, ocean, system
    uint8_t titlekek[0x20][16];
    uint8_t src_kak[3][16], kek_gen[16], key_gen[16], titlekek_src[16], header_kek_src[16], header_key_src[32];
    bool have_header, have_master[0x20], have_kak[0x20][3], have_tk[0x20];
    bool have_src_kak[3], have_kek_gen, have_key_gen, have_titlekek_src, have_header_kek_src, have_header_key_src;
} Keys;

static bool hex_decode(const char *s, uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (!isxdigit((unsigned char)s[2 * i]) || !isxdigit((unsigned char)s[2 * i + 1])) return false;
        char b[3] = {s[2 * i], s[2 * i + 1], 0};
        out[i] = (uint8_t)strtoul(b, NULL, 16);
    }
    return s[2 * n] == 0;
}

// name is prefix + two hex digits: the digits' value, else -1
static int key_index(const char *name, const char *prefix) {
    size_t n = strlen(prefix);
    if (strncmp(name, prefix, n) || strlen(name) != n + 2 || !isxdigit((unsigned char)name[n]) ||
        !isxdigit((unsigned char)name[n + 1]))
        return -1;
    int v = (int)strtol(name + n, NULL, 16);
    return v < 0x20 ? v : -1;
}

// key = KEK derived from the master key: AES-ECB-decrypt(AES-ECB-decrypt(master, kek_seed), src), then (key_seed) again
static void generate_kek(uint8_t dst[16], const uint8_t src[16], const uint8_t master[16], const uint8_t kek_seed[16],
                         const uint8_t key_seed[16]) {
    uint8_t kek[16], s[16];
    ecb_decrypt(master, kek_seed, kek, 16);
    ecb_decrypt(kek, src, s, 16);
    if (key_seed) ecb_decrypt(s, key_seed, dst, 16);
    else memcpy(dst, s, 16);
}

static bool keys_load(Keys *k, const char *path) {
    memset(k, 0, sizeof *k);
    FILE *f = fopen(path, "rb");
    if (!f) return fail("The keys file %s could not be opened.", path);
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == ';' || line[0] == '#') continue;
        *eq = 0;
        char *name = line, *val = eq + 1;
        while (isspace((unsigned char)*name)) name++;
        for (char *e = name + strlen(name); e > name && isspace((unsigned char)e[-1]);) *--e = 0;
        while (isspace((unsigned char)*val)) val++;
        for (char *e = val + strlen(val); e > val && isspace((unsigned char)e[-1]);) *--e = 0;
        for (char *c = name; *c; c++) *c = (char)tolower((unsigned char)*c);
        int i;
        if (!strcmp(name, "header_key")) k->have_header = hex_decode(val, k->header_key, 32);
        else if (!strcmp(name, "aes_kek_generation_source")) k->have_kek_gen = hex_decode(val, k->kek_gen, 16);
        else if (!strcmp(name, "aes_key_generation_source")) k->have_key_gen = hex_decode(val, k->key_gen, 16);
        else if (!strcmp(name, "titlekek_source")) k->have_titlekek_src = hex_decode(val, k->titlekek_src, 16);
        else if (!strcmp(name, "header_kek_source")) k->have_header_kek_src = hex_decode(val, k->header_kek_src, 16);
        else if (!strcmp(name, "header_key_source")) k->have_header_key_src = hex_decode(val, k->header_key_src, 32);
        else if (!strcmp(name, "key_area_key_application_source")) k->have_src_kak[0] = hex_decode(val, k->src_kak[0], 16);
        else if (!strcmp(name, "key_area_key_ocean_source")) k->have_src_kak[1] = hex_decode(val, k->src_kak[1], 16);
        else if (!strcmp(name, "key_area_key_system_source")) k->have_src_kak[2] = hex_decode(val, k->src_kak[2], 16);
        else if ((i = key_index(name, "master_key_")) >= 0) k->have_master[i] = hex_decode(val, k->master[i], 16);
        else if ((i = key_index(name, "titlekek_")) >= 0) k->have_tk[i] = hex_decode(val, k->titlekek[i], 16);
        else if ((i = key_index(name, "key_area_key_application_")) >= 0) k->have_kak[i][0] = hex_decode(val, k->kak[i][0], 16);
        else if ((i = key_index(name, "key_area_key_ocean_")) >= 0) k->have_kak[i][1] = hex_decode(val, k->kak[i][1], 16);
        else if ((i = key_index(name, "key_area_key_system_")) >= 0) k->have_kak[i][2] = hex_decode(val, k->kak[i][2], 16);
    }
    fclose(f);
    // what a prod.keys without the ready-made keys can still give, from the master keys and the sources
    for (int i = 0; i < 0x20; i++) {
        if (!k->have_master[i]) continue;
        for (int t = 0; t < 3; t++)
            if (!k->have_kak[i][t] && k->have_src_kak[t] && k->have_kek_gen && k->have_key_gen) {
                generate_kek(k->kak[i][t], k->src_kak[t], k->master[i], k->kek_gen, k->key_gen);
                k->have_kak[i][t] = true;
            }
        if (!k->have_tk[i] && k->have_titlekek_src) {
            ecb_decrypt(k->master[i], k->titlekek_src, k->titlekek[i], 16);
            k->have_tk[i] = true;
        }
    }
    if (!k->have_header && k->have_header_kek_src && k->have_header_key_src && k->have_master[0] && k->have_kek_gen &&
        k->have_key_gen) {
        uint8_t kek[16];
        generate_kek(kek, k->header_kek_src, k->master[0], k->kek_gen, k->key_gen);
        ecb_decrypt(kek, k->header_key_src, k->header_key, 32);
        k->have_header = true;
    }
    if (!k->have_header) return fail("%s has no header_key: it is not a prod.keys from a Switch (Lockpick_RCM writes one).", path);
    return true;
}

// ------------------------------------------------------------------ storage: reading a byte range, whatever it is
typedef struct Stor Stor;
struct Stor {
    bool (*read)(Stor *s, uint64_t off, void *buf, size_t n);
    uint64_t size;
};

typedef struct {  // a window of a file
    Stor s;
    FILE *f;
    uint64_t base;
} FileStor;

static bool file_read(Stor *st, uint64_t off, void *buf, size_t n) {
    FileStor *fs = (FileStor *)st;
    if (off + n < off || off + n > st->size) return fail("The file is shorter than its headers say (cut off while copying or downloading?).");
    if (fseeko(fs->f, (off_t)(fs->base + off), SEEK_SET) || fread(buf, 1, n, fs->f) != n)
        return fail("Reading the file failed (%s).", strerror(errno));
    return true;
}
static void file_stor(FileStor *fs, FILE *f, uint64_t base, uint64_t size) {
    fs->s.read = file_read;
    fs->s.size = size;
    fs->f = f;
    fs->base = base;
}

// ------------------------------------------------------------------ PFS0 (the NSP itself, and the ExeFS)
typedef struct {
    char name[256];
    uint64_t off, size;  // in the PFS0's storage, from its start
} Pfs0File;
typedef struct {
    Pfs0File *files;
    int count;
} Pfs0;

static void pfs0_free(Pfs0 *p) {
    free(p->files);
    p->files = NULL;
    p->count = 0;
}

static bool pfs0_parse(Stor *st, uint64_t base, Pfs0 *p, const char *what) {
    uint8_t h[16];
    memset(p, 0, sizeof *p);
    if (!st->read(st, base, h, 16)) return false;
    if (memcmp(h, "PFS0", 4)) return fail("%s is not a PFS0 archive (not an NSP file?).", what);
    uint32_t n = le32(h + 4), strsz = le32(h + 8);
    if (n > 4096 || strsz > (1u << 20)) return fail("%s has a damaged file table.", what);
    size_t tab = 16 + (size_t)n * 24;
    uint8_t *raw = malloc(tab + strsz + 1);
    if (!raw) return fail("Out of memory.");
    if (!st->read(st, base, raw, tab + strsz)) { free(raw); return false; }
    raw[tab + strsz] = 0;
    p->files = calloc(n ? n : 1, sizeof *p->files);
    if (!p->files) { free(raw); return fail("Out of memory."); }
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = raw + 16 + (size_t)i * 24;
        uint32_t so = le32(e + 16);
        if (so >= strsz) { free(raw); pfs0_free(p); return fail("%s has a damaged file table.", what); }
        snprintf(p->files[i].name, sizeof p->files[i].name, "%s", (const char *)raw + tab + so);
        p->files[i].off = tab + strsz + le64(e);
        p->files[i].size = le64(e + 8);
        if (p->files[i].off + p->files[i].size < p->files[i].off || base + p->files[i].off + p->files[i].size > st->size) {
            free(raw);
            pfs0_free(p);
            return fail("%s is cut off: a file in it goes past its end.", what);
        }
    }
    free(raw);
    p->count = (int)n;
    return true;
}

static bool ends_with(const char *s, const char *suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    if (b > a) return false;
    for (size_t i = 0; i < b; i++)
        if (tolower((unsigned char)s[a - b + i]) != suffix[i]) return false;
    return true;
}

// ------------------------------------------------------------------ output files and folders
static bool mkdir_p(const char *path) {
    char tmp[1400];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/' && *p != '\\') continue;
        char c = *p;
        *p = 0;
        if (MKDIR(tmp) && errno != EEXIST) {
            struct stat st;
            if (stat(tmp, &st) || !S_ISDIR(st.st_mode)) return fail("The folder %s could not be made (%s).", tmp, strerror(errno));
        }
        *p = c;
    }
    if (MKDIR(tmp) && errno != EEXIST) return fail("The folder %s could not be made (%s).", tmp, strerror(errno));
    return true;
}

// dst = a/b, or false (and why) when it doesn't fit
static bool join_path(char *dst, size_t size, const char *a, const char *b) {
    int n = snprintf(dst, size, "%s/%s", a, b);
    if (n < 0 || (size_t)n >= size) return fail("The path %s/%s is too long: use a shorter folder for the game files.", a, b);
    return true;
}

// path and everything in it, deleted (the half-written import-tmp of an import that stopped)
static void remove_tree(const char *path) {
    DIR *d = opendir(path);
    if (d) {
        for (struct dirent *e; (e = readdir(d));) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char sub[1500];
            if (snprintf(sub, sizeof sub, "%s/%s", path, e->d_name) >= (int)sizeof sub) continue;
            struct stat st;
            if (stat(sub, &st) == 0 && S_ISDIR(st.st_mode)) remove_tree(sub);
            else remove(sub);
        }
        closedir(d);
    }
    rmdir(path);
}

static bool exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

// a file name from an archive: no path parts, nothing that would write outside the folder
static bool safe_name(const char *name, size_t len) {
    if (!len || len > 255) return false;
    if ((len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.')) return false;
    for (size_t i = 0; i < len; i++)
        if ((unsigned char)name[i] < 0x20 || name[i] == '/' || name[i] == '\\') return false;
    return true;
}

typedef struct {
    NspProgress progress;
    void *user;
    const char *what;
    uint64_t total, done, last_report;
} Report;

static void report(Report *r, bool force) {
    if (!r->progress) return;
    if (!force && r->done - r->last_report < (4u << 20)) return;
    r->last_report = r->done;
    r->progress(r->user, r->what, r->done, r->total);
}

// n bytes from st at off into the new file path
static bool copy_out(Stor *st, uint64_t off, uint64_t n, const char *path, Report *rep) {
    FILE *f = fopen(path, "wb");
    if (!f) return fail("The file %s could not be written (%s).", path, strerror(errno));
    static uint8_t *buf;
    const size_t chunk = 1u << 20;
    if (!buf && !(buf = malloc(chunk))) { fclose(f); return fail("Out of memory."); }
    bool ok = true;
    for (uint64_t done = 0; done < n && ok;) {
        size_t take = n - done < chunk ? (size_t)(n - done) : chunk;
        if (!st->read(st, off + done, buf, take)) ok = false;
        else if (fwrite(buf, 1, take, f) != take) ok = fail("Writing %s failed (%s): is the disk full?", path, strerror(errno));
        done += take;
        if (rep) {
            rep->done += take;
            report(rep, false);
        }
    }
    if (fclose(f) && ok) ok = fail("Writing %s failed (%s): is the disk full?", path, strerror(errno));
    return ok;
}

// ------------------------------------------------------------------ tickets
typedef struct {
    uint8_t rights_id[16], enc_key[16];
} Ticket;

// A common ticket's title key (encrypted with the console's titlekek, which prod.keys has).
static bool ticket_parse(Stor *st, const Pfs0File *pf, Ticket *t) {
    uint8_t b[0x240 + 0x180];
    size_t n = pf->size < sizeof b ? (size_t)pf->size : sizeof b;
    if (n < 0x180 || !st->read(st, pf->off, b, n)) return fail("The ticket %s is damaged.", pf->name);
    uint32_t sig = le32(b);
    size_t body = sig == 0x10000 || sig == 0x10003 ? 0x240 : sig == 0x10001 || sig == 0x10004 ? 0x140 : sig == 0x10002 || sig == 0x10005 ? 0x80 : 0;
    if (!body || body + 0x170 > n) return fail("The ticket %s is not in a format this reads.", pf->name);
    if (b[body + 0x141] != 0)
        return fail("The ticket in this NSP is personalized (tied to one console): it can't be read without that console's "
                    "keys. Dump the game's NSP again as \"standard crypto\" (NXDumpTool / DBI convert the ticket), or use "
                    "an NSP from your Switch's own dump.");
    memcpy(t->enc_key, b + body + 0x40, 16);
    memcpy(t->rights_id, b + body + 0x160, 16);
    return true;
}

// ------------------------------------------------------------------ NCA
typedef struct {
    bool present;
    uint64_t start, size;     // in the NCA
    uint8_t partition, fs_type, crypt;  // crypt: 1 none, 2 XTS, 3 CTR, 4 CTR-EX (BKTR)
    const uint8_t *fsh;       // its FS header (0x200 bytes in the NCA's header)
    uint8_t ctr_hi[8];
} NcaSec;

typedef struct {
    FileStor fs;  // the whole NCA
    uint8_t hdr[0xC00];
    uint8_t content_type, rights_id[16];
    bool has_rights;
    uint64_t program_id;
    int keygen;   // the master key index
    uint8_t kaek;
    NcaSec sec[4];
    Aes aes;      // the sections' AES-CTR key
    bool keyed;
} Nca;

static bool nca_open(Nca *n, FILE *f, uint64_t base, uint64_t size, const Keys *k, const char *name) {
    memset(n, 0, sizeof *n);
    file_stor(&n->fs, f, base, size);
    if (size < 0xC00) return fail("%s is too short to be an NCA.", name);
    if (!n->fs.s.read(&n->fs.s, 0, n->hdr, 0xC00)) return false;
    xts_decrypt(k->header_key, n->hdr, 0x400, 0, 0x200);
    if (!memcmp(n->hdr + 0x200, "NCA3", 4)) {
        uint8_t raw[0x800];
        if (!n->fs.s.read(&n->fs.s, 0x400, raw, 0x800)) return false;
        xts_decrypt(k->header_key, raw, 0x800, 2, 0x200);
        memcpy(n->hdr + 0x400, raw, 0x800);
    } else if (!memcmp(n->hdr + 0x200, "NCA2", 4)) {
        uint8_t raw[0x800];
        if (!n->fs.s.read(&n->fs.s, 0x400, raw, 0x800)) return false;
        for (int i = 0; i < 4; i++) {  // (NCA2: each FS header its own, from sector 0)
            xts_decrypt(k->header_key, raw + 0x200 * i, 0x200, 0, 0x200);
            memcpy(n->hdr + 0x400 + 0x200 * i, raw + 0x200 * i, 0x200);
        }
    } else {
        return fail("%s can't be read as an NCA: its header doesn't decrypt. The header_key in prod.keys is wrong or from another "
                    "console generation, or the file is damaged.", name);
    }
    n->content_type = n->hdr[0x205];
    n->program_id = le64(n->hdr + 0x210);
    n->kaek = n->hdr[0x207];
    int gen = n->hdr[0x206] > n->hdr[0x220] ? n->hdr[0x206] : n->hdr[0x220];
    n->keygen = gen ? gen - 1 : 0;  // (0 and 1 are both the first master key)
    memcpy(n->rights_id, n->hdr + 0x230, 16);
    for (int i = 0; i < 16; i++)
        if (n->rights_id[i]) n->has_rights = true;
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = n->hdr + 0x240 + 0x10 * i;
        uint32_t start = le32(e), end = le32(e + 4);
        if (!start) continue;
        NcaSec *s = &n->sec[i];
        s->present = true;
        s->start = (uint64_t)start * 0x200;
        s->size = ((uint64_t)end - start) * 0x200;
        if (end < start || s->start + s->size > size) return fail("%s has a section that goes past its end.", name);
        s->fsh = n->hdr + 0x400 + 0x200 * i;
        s->partition = s->fsh[2];
        s->fs_type = s->fsh[3];
        s->crypt = s->fsh[4];
        for (int j = 0; j < 8; j++) s->ctr_hi[j] = s->fsh[0x140 + 7 - j];
    }
    return true;
}

// The AES-CTR key for the sections: from the key area (the master key), or the ticket's title key.
static bool nca_keys(Nca *n, const Keys *k, const Ticket *tik, int ntik, const char *tik_why, const char *name) {
    uint8_t key[16];
    if (n->has_rights) {
        const Ticket *t = NULL;
        for (int i = 0; i < ntik; i++)
            if (!memcmp(tik[i].rights_id, n->rights_id, 16)) t = &tik[i];
        if (!t && tik_why && tik_why[0]) return fail("%s", tik_why);
        if (!t) return fail("%s needs a ticket (.tik) that isn't in the NSP: use an NSP that has its ticket.", name);
        if (!k->have_tk[n->keygen])
            return fail("prod.keys has no titlekek_%02x / master_key_%02x: this game's update needs the keys of a newer firmware. "
                        "Run Lockpick_RCM again on a Switch updated to at least this game's required firmware.", n->keygen, n->keygen);
        ecb_decrypt(k->titlekek[n->keygen], t->enc_key, key, 16);
    } else {
        if (n->kaek > 2) return fail("%s has a key area type this doesn't know (%d).", name, n->kaek);
        if (!k->have_kak[n->keygen][n->kaek])
            return fail("prod.keys has no key_area_key_%s_%02x (or the master_key_%02x it comes from): the NSP needs the keys of "
                        "a newer firmware. Run Lockpick_RCM again on a Switch updated to at least this game's required "
                        "firmware.", n->kaek == 0 ? "application" : n->kaek == 1 ? "ocean" : "system", n->keygen, n->keygen);
        uint8_t area[0x40];
        ecb_decrypt(k->kak[n->keygen][n->kaek], n->hdr + 0x300, area, 0x40);
        memcpy(key, area + 0x20, 16);  // (key 2 of the four: the AES-CTR one)
    }
    aes_set_key(&n->aes, key);
    n->keyed = true;
    return true;
}

// A section's bytes, decrypted (AES-CTR; a BKTR section's tables are CTR with the section's own counter).
typedef struct {
    Stor s;
    Nca *nca;
    int idx;
} SecStor;

static bool sec_read(Stor *st, uint64_t off, void *buf, size_t n) {
    SecStor *ss = (SecStor *)st;
    NcaSec *sec = &ss->nca->sec[ss->idx];
    if (off + n < off || off + n > sec->size) return fail("A read went past the end of an NCA section (damaged NSP?).");
    if (sec->crypt == 2) return fail("An NCA section uses AES-XTS, which Program NCAs of this game don't.");
    if (!ss->nca->fs.s.read(&ss->nca->fs.s, sec->start + off, buf, n)) return false;
    if (sec->crypt == 3 || sec->crypt == 4) ctr_at(&ss->nca->aes, sec->ctr_hi, -1, sec->start + off, buf, n);
    return true;
}
static void sec_stor(SecStor *ss, Nca *nca, int idx) {
    ss->s.read = sec_read;
    ss->s.size = nca->sec[idx].size;
    ss->nca = nca;
    ss->idx = idx;
}

// ------------------------------------------------------------------ BKTR: an update's RomFS over the base game's
typedef struct {
    uint64_t virt, phys;
    uint32_t patch;  // 1: the update's own data, 0: the base game's
} Reloc;
typedef struct {
    uint64_t off;
    uint32_t ctr;
} Subsec;

typedef struct {
    Stor s;      // the patched section as one piece: size = its virtual size
    Nca *nca;
    int idx;
    Stor *base;  // the base game's RomFS section
    Reloc *rel;
    size_t nrel;
    Subsec *sub;
    size_t nsub;
    uint64_t data_end;  // where the patch's own data ends (the tables start)
} BktrStor;

static bool bktr_patch_read(BktrStor *b, uint64_t phys, uint8_t *buf, size_t n) {
    NcaSec *sec = &b->nca->sec[b->idx];
    while (n) {
        if (phys >= b->data_end) return fail("The update's patch data is damaged (a read past its end).");
        size_t lo = 0, hi = b->nsub;  // the last subsection starting at or before phys
        while (lo + 1 < hi) {
            size_t mid = (lo + hi) / 2;
            if (b->sub[mid].off <= phys) lo = mid; else hi = mid;
        }
        uint64_t end = lo + 1 < b->nsub ? b->sub[lo + 1].off : b->data_end;
        size_t take = end - phys < n ? (size_t)(end - phys) : n;
        if (!b->nca->fs.s.read(&b->nca->fs.s, sec->start + phys, buf, take)) return false;
        ctr_at(&b->nca->aes, sec->ctr_hi, b->sub[lo].ctr, sec->start + phys, buf, take);
        buf += take;
        phys += take;
        n -= take;
    }
    return true;
}

static bool bktr_read(Stor *st, uint64_t off, void *vbuf, size_t n) {
    BktrStor *b = (BktrStor *)st;
    uint8_t *buf = vbuf;
    if (off + n < off || off + n > st->size) return fail("A read went past the end of the update's RomFS (damaged NSP?).");
    while (n) {
        size_t lo = 0, hi = b->nrel;
        while (lo + 1 < hi) {
            size_t mid = (lo + hi) / 2;
            if (b->rel[mid].virt <= off) lo = mid; else hi = mid;
        }
        uint64_t end = lo + 1 < b->nrel ? b->rel[lo + 1].virt : st->size;
        size_t take = end - off < n ? (size_t)(end - off) : n;
        uint64_t phys = b->rel[lo].phys + (off - b->rel[lo].virt);
        bool ok = b->rel[lo].patch ? bktr_patch_read(b, phys, buf, take) : b->base->read(b->base, phys, buf, take);
        if (!ok) return false;
        buf += take;
        off += take;
        n -= take;
    }
    return true;
}

static void bktr_free(BktrStor *b) {
    free(b->rel);
    free(b->sub);
    b->rel = NULL;
    b->sub = NULL;
}

// Reads the update RomFS section's two tables (bucket trees: a 0x4000-byte header node with the first offsets, then
// one 0x4000-byte node per bucket of entries).
static bool bktr_open(BktrStor *b, Nca *nca, int idx, Stor *base) {
    memset(b, 0, sizeof *b);
    NcaSec *sec = &nca->sec[idx];
    const uint8_t *sb = sec->fsh + 0x8;
    uint64_t roff = le64(sb + 0xF8), rsize = le64(sb + 0x100), soff = le64(sb + 0x118), ssize = le64(sb + 0x120);
    if (memcmp(sb + 0x108, "BKTR", 4) || memcmp(sb + 0x128, "BKTR", 4))
        return fail("The update's RomFS has no patch tables (this NSP isn't a v1.4.0 update?).");
    if (roff + rsize != soff || soff + ssize != sec->size || rsize < 0x8000 || ssize < 0x8000 || rsize > (64u << 20) || ssize > (64u << 20))
        return fail("The update's RomFS patch tables are damaged.");
    SecStor ss;
    sec_stor(&ss, nca, idx);
    uint8_t *rt = malloc(rsize), *st = malloc(ssize);
    bool ok = rt && st;
    if (!ok) fail("Out of memory.");
    ok = ok && ss.s.read(&ss.s, roff, rt, rsize) && ss.s.read(&ss.s, soff, st, ssize);
    if (ok) {
        uint32_t nb = le32(rt + 4);
        uint64_t total = le64(rt + 8);
        if (!nb || (uint64_t)(1 + nb) * 0x4000 > rsize) ok = fail("The update's relocation table is damaged.");
        size_t count = 0;
        for (uint32_t i = 0; ok && i < nb; i++) {
            uint32_t c = le32(rt + 0x4000 * (size_t)(1 + i) + 4);
            if (c > (0x4000 - 0x10) / 0x14) ok = fail("The update's relocation table is damaged.");
            count += c;
        }
        if (ok && !count) ok = fail("The update's relocation table is empty.");
        if (ok) b->rel = calloc(count, sizeof *b->rel);
        if (ok && !b->rel) ok = fail("Out of memory.");
        for (uint32_t i = 0; ok && i < nb; i++) {
            const uint8_t *node = rt + 0x4000 * (size_t)(1 + i);
            uint32_t c = le32(node + 4);
            for (uint32_t j = 0; j < c; j++) {
                const uint8_t *e = node + 0x10 + 0x14 * (size_t)j;
                b->rel[b->nrel++] = (Reloc){le64(e), le64(e + 8), le32(e + 16)};
            }
        }
        if (ok && (b->rel[0].virt != 0 || total == 0)) ok = fail("The update's relocation table is damaged.");
        for (size_t i = 1; ok && i < b->nrel; i++)
            if (b->rel[i].virt <= b->rel[i - 1].virt || b->rel[i].virt >= total) ok = fail("The update's relocation table is damaged.");
        if (ok) b->s.size = total;
    }
    if (ok) {
        uint32_t nb = le32(st + 4);
        if (!nb || (uint64_t)(1 + nb) * 0x4000 > ssize) ok = fail("The update's subsection table is damaged.");
        size_t count = 0;
        for (uint32_t i = 0; ok && i < nb; i++) {
            uint32_t c = le32(st + 0x4000 * (size_t)(1 + i) + 4);
            if (c > (0x4000 - 0x10) / 0x10) ok = fail("The update's subsection table is damaged.");
            count += c;
        }
        if (ok) b->sub = calloc(count ? count : 1, sizeof *b->sub);
        if (ok && !b->sub) ok = fail("Out of memory.");
        for (uint32_t i = 0; ok && i < nb; i++) {
            const uint8_t *node = st + 0x4000 * (size_t)(1 + i);
            uint32_t c = le32(node + 4);
            for (uint32_t j = 0; j < c; j++) {
                const uint8_t *e = node + 0x10 + 0x10 * (size_t)j;
                uint64_t o = le64(e);
                if (o >= roff) continue;  // (an entry for the tables themselves: those are read with the section's own counter)
                b->sub[b->nsub++] = (Subsec){o, le32(e + 12)};
            }
        }
        if (ok && (!b->nsub || b->sub[0].off != 0)) ok = fail("The update's subsection table is damaged.");
        for (size_t i = 1; ok && i < b->nsub; i++)
            if (b->sub[i].off <= b->sub[i - 1].off) ok = fail("The update's subsection table is damaged.");
    }
    free(rt);
    free(st);
    if (!ok) {
        bktr_free(b);
        return false;
    }
    b->s.read = bktr_read;
    b->nca = nca;
    b->idx = idx;
    b->base = base;
    b->data_end = roff;
    return true;
}

// ------------------------------------------------------------------ RomFS
typedef struct {
    Stor *st;
    uint64_t root;  // where the RomFS image starts in st
    uint8_t *dirs, *files;
    uint64_t dsize, fsize, data_off;
    const char *out;
    bool dry;       // only count
    uint64_t nfiles, bytes;
    Report *rep;
    int guard;
} Rfs;

static bool rfs_walk(Rfs *r, uint32_t doff, char *path, size_t plen, int depth) {
    if (depth > 64) return fail("The RomFS folders nest deeper than a RomFS does (damaged?).");
    if ((uint64_t)doff + 0x18 > r->dsize) return fail("The RomFS's folder table is damaged.");
    const uint8_t *d = r->dirs + doff;
    uint32_t child = le32(d + 8), file = le32(d + 12);
    for (uint32_t f = file; f != 0xFFFFFFFFu;) {
        if ((uint64_t)f + 0x20 > r->fsize || ++r->guard > 4000000) return fail("The RomFS's file table is damaged.");
        const uint8_t *e = r->files + f;
        uint32_t nlen = le32(e + 0x1C);
        if ((uint64_t)f + 0x20 + nlen > r->fsize || !safe_name((const char *)e + 0x20, nlen)) return fail("The RomFS has a file name this won't write (damaged NSP?).");
        uint64_t off = le64(e + 8), size = le64(e + 16);
        r->nfiles++;
        r->bytes += size;
        if (!r->dry) {
            if (plen + 1 + nlen + 1 > 1400) return fail("A RomFS path is too long.");
            memcpy(path + plen, "/", 1);
            memcpy(path + plen + 1, e + 0x20, nlen);
            path[plen + 1 + nlen] = 0;
            if (!copy_out(r->st, r->root + r->data_off + off, size, path, r->rep)) return false;
            path[plen] = 0;
        }
        f = le32(e + 4);
    }
    for (uint32_t c = child; c != 0xFFFFFFFFu;) {
        if ((uint64_t)c + 0x18 > r->dsize || ++r->guard > 4000000) return fail("The RomFS's folder table is damaged.");
        const uint8_t *e = r->dirs + c;
        uint32_t nlen = le32(e + 0x14);
        if ((uint64_t)c + 0x18 + nlen > r->dsize || !safe_name((const char *)e + 0x18, nlen)) return fail("The RomFS has a folder name this won't write (damaged NSP?).");
        if (plen + 1 + nlen + 1 > 1400) return fail("A RomFS path is too long.");
        memcpy(path + plen, "/", 1);
        memcpy(path + plen + 1, e + 0x18, nlen);
        path[plen + 1 + nlen] = 0;
        if (!r->dry && !mkdir_p(path)) return false;
        if (!rfs_walk(r, c, path, plen + 1 + nlen, depth + 1)) return false;
        path[plen] = 0;
        c = le32(e + 4);
    }
    return true;
}

static bool romfs_extract(Stor *st, uint64_t root, const char *outdir, NspProgress progress, void *user) {
    uint8_t h[0x50];
    if (!st->read(st, root, h, sizeof h)) return false;
    Rfs r = {.st = st, .root = root, .out = outdir};
    uint64_t dm_off = le64(h + 0x18), dm_size = le64(h + 0x20), fm_off = le64(h + 0x38), fm_size = le64(h + 0x40);
    r.data_off = le64(h + 0x48);
    if (le64(h) != 0x50 || dm_size < 0x18 || dm_size > (256u << 20) || fm_size > (512u << 20) || root + dm_off + dm_size > st->size ||
        root + fm_off + fm_size > st->size || root + r.data_off > st->size)
        return fail("The RomFS header is damaged (wrong key, or a damaged NSP).");
    r.dsize = dm_size;
    r.fsize = fm_size;
    r.dirs = malloc(dm_size);
    r.files = malloc(fm_size ? fm_size : 1);
    bool ok = r.dirs && r.files;
    if (!ok) fail("Out of memory.");
    ok = ok && st->read(st, root + dm_off, r.dirs, dm_size) && (!fm_size || st->read(st, root + fm_off, r.files, fm_size));
    char path[1400];
    snprintf(path, sizeof path, "%s", outdir);
    ok = ok && mkdir_p(path);
    Report rep = {.progress = progress, .user = user, .what = "RomFS"};
    r.rep = &rep;
    r.dry = true;
    ok = ok && rfs_walk(&r, 0, path, strlen(path), 0);
    if (ok && (!r.nfiles || r.nfiles > 200000)) ok = fail("The RomFS has no files in it (wrong key, or a damaged NSP).");
    if (ok) {
        rep.total = r.bytes;
        r.dry = false;
        r.guard = 0;
        r.nfiles = 0;
        r.bytes = 0;
        report(&rep, true);
        ok = rfs_walk(&r, 0, path, strlen(path), 0);
        report(&rep, true);
    }
    free(r.dirs);
    free(r.files);
    return ok;
}

// ------------------------------------------------------------------ one NCA's two parts
// The ExeFS: section 0, a PFS0 inside the hash layer (its offset is in the FS header).
static bool exefs_extract(Nca *nca, const char *outdir, NspProgress progress, void *user) {
    NcaSec *sec = &nca->sec[0];
    if (!sec->present || sec->partition != 1 || sec->fs_type != 2) return fail("The Program NCA has no ExeFS section.");
    SecStor ss;
    sec_stor(&ss, nca, 0);
    uint64_t pfs_off = le64(sec->fsh + 0x40);
    Pfs0 p;
    if (!pfs0_parse(&ss.s, pfs_off, &p, "The ExeFS")) return false;
    if (!mkdir_p(outdir)) { pfs0_free(&p); return false; }
    Report rep = {.progress = progress, .user = user, .what = "ExeFS"};
    for (int i = 0; i < p.count; i++) rep.total += p.files[i].size;
    bool ok = true;
    for (int i = 0; ok && i < p.count; i++) {
        size_t nlen = strlen(p.files[i].name);
        if (!safe_name(p.files[i].name, nlen)) { ok = fail("The ExeFS has a file name this won't write (damaged NSP?)."); break; }
        char path[1400];
        ok = join_path(path, sizeof path, outdir, p.files[i].name) &&
             copy_out(&ss.s, pfs_off + p.files[i].off, p.files[i].size, path, &rep);
    }
    report(&rep, true);
    pfs0_free(&p);
    return ok;
}

// where the RomFS image starts in section 1: the last of the IVFC header's levels (the others are hash levels)
static uint64_t romfs_start(const NcaSec *sec) { return le64(sec->fsh + 0x8 + 0x10 + 5 * 0x18); }

// ------------------------------------------------------------------ the NSP files
typedef struct {
    char path[1024];
    FILE *f;
    FileStor fs;
    Pfs0 pfs;
    Ticket tik[8];
    int ntik;
    char tik_why[600];  // why a ticket couldn't be read (said only if the NCA needs one)
    Nca prog;           // the Program NCA
    bool have_prog;
    int prog_idx;       // its entry in pfs
} NspFile;

static void nspfile_close(NspFile *n) {
    pfs0_free(&n->pfs);
    if (n->f) fclose(n->f);
    n->f = NULL;
}

static bool nspfile_open(NspFile *n, const char *path, const Keys *k) {
    memset(n, 0, sizeof *n);
    snprintf(n->path, sizeof n->path, "%s", path);
    n->f = fopen(path, "rb");
    if (!n->f) return fail("The file %s could not be opened (%s).", path, strerror(errno));
    if (fseeko(n->f, 0, SEEK_END)) return fail("The file %s could not be read.", path);
    off_t size = ftello(n->f);
    if (size < 0x10) return fail("The file %s is empty or too short.", path);
    file_stor(&n->fs, n->f, 0, (uint64_t)size);
    if (!pfs0_parse(&n->fs.s, 0, &n->pfs, path)) return false;
    for (int i = 0; i < n->pfs.count; i++) {
        const char *name = n->pfs.files[i].name;
        if (ends_with(name, ".ncz") || ends_with(name, ".nsz"))
            return fail("%s is compressed (NSZ): extract it to a normal NSP first (the nsz tool does).", path);
        if (!ends_with(name, ".tik")) continue;
        if (n->ntik < 8 && ticket_parse(&n->fs.s, &n->pfs.files[i], &n->tik[n->ntik])) n->ntik++;
        else snprintf(n->tik_why, sizeof n->tik_why, "%s", n->ntik < 8 ? g_err : "The NSP has too many tickets.");
    }
    for (int i = 0; i < n->pfs.count; i++) {
        const Pfs0File *pf = &n->pfs.files[i];
        if (!ends_with(pf->name, ".nca") || ends_with(pf->name, ".cnmt.nca")) continue;
        Nca nca;
        if (!nca_open(&nca, n->f, pf->off, pf->size, k, pf->name)) return false;
        if (nca.content_type != 0 || n->have_prog) continue;  // (the Program NCA; Control, Legal... are not needed)
        n->prog = nca;
        for (int j = 0; j < 4; j++)  // (the sections' FS headers are in the NCA's own header: now the copy's)
            if (n->prog.sec[j].present) n->prog.sec[j].fsh = n->prog.hdr + 0x400 + 0x200 * j;
        n->have_prog = true;
        n->prog_idx = i;
    }
    if (!n->have_prog) return fail("%s has no game in it (no Program NCA): it's not the base game's or the update's NSP.", path);
    return true;
}

// ------------------------------------------------------------------ the import
static bool unique_aside(const char *out, char *dir, size_t size) {
    for (int i = 1; i < 1000; i++) {
        snprintf(dir, size, "%s/previous-%d", out, i);
        if (!exists(dir)) return true;
    }
    return fail("%s has too many previous-N folders: delete the ones you don't need.", out);
}

// The game folder's own exefs / romfs (the ones that failed the check) out of the way, to previous-N/.
static bool move_aside(const char *out) {
    char ex[1400], ro[1400], dir[1400] = "";
    if (!join_path(ex, sizeof ex, out, "exefs") || !join_path(ro, sizeof ro, out, "romfs")) return false;
    for (int i = 0; i < 2; i++) {
        const char *src = i ? ro : ex;
        if (!exists(src)) continue;
        if (!dir[0]) {
            if (!unique_aside(out, dir, sizeof dir) || !mkdir_p(dir)) return false;
        }
        char dst[1500];
        snprintf(dst, sizeof dst, "%s/%s", dir, i ? "romfs" : "exefs");
        if (rename(src, dst)) return fail("%s could not be moved to %s (%s).", src, dst, strerror(errno));
    }
    return true;
}

static void say(const NspImport *job, const char *what) {
    if (job->progress) job->progress(job->user, what, 0, 0);
}

bool nsp_import(const NspImport *job, char *err, size_t err_size) {
    g_err[0] = 0;
    NspFile nsp[4];
    int n = 0;
    bool ok = true;
    Keys *keys = calloc(1, sizeof *keys);
    char found[1024];
    const char *keys_path = job->keys_path;
    if (!keys) ok = fail("Out of memory.");
    if (ok && !keys_path) {
        if (!nsp_find_keys(found, sizeof found))
            ok = fail("prod.keys was not found. It holds the keys that unlock the NSP files, from your own Switch (Lockpick_RCM "
                      "writes it). Put it in one of these places:\n%s", nsp_keys_places());
        keys_path = found;
    }
    if (ok && (job->nsp_count < 2 || job->nsp_count > 4))
        ok = fail("Two NSP files are needed: the base game's and the 1.4.0 update's (%d given).", job->nsp_count);
    if (ok) ok = keys_load(keys, keys_path);
    for (int i = 0; ok && i < job->nsp_count; i++) {
        ok = nspfile_open(&nsp[n], job->nsp_paths[i], keys);
        n++;  // (counted before the check, so a half-open one is closed too)
    }
    NspFile *base = NULL, *upd = NULL;
    for (int i = 0; ok && i < n; i++) {
        Nca *p = &nsp[i].prog;
        if (p->program_id != PROGRAM_ID_BASE && p->program_id != PROGRAM_ID_UPDATE) {
            ok = fail("%s is not Monster Hunter Generations Ultimate (title id %016llX).", nsp[i].path, (unsigned long long)p->program_id);
            break;
        }
        NspFile **slot = p->program_id == PROGRAM_ID_BASE ? &base : &upd;
        if (*slot) {
            ok = fail("%s and %s are both the %s: give the base game's NSP and the update's.", (*slot)->path, nsp[i].path,
                      slot == &base ? "base game" : "update");
            break;
        }
        *slot = &nsp[i];
    }
    if (ok && (!base || !upd)) ok = fail("Two NSP files are needed: the base game's and the 1.4.0 update's. This has only the %s.", base ? "base game" : "update");
    if (ok) ok = nca_keys(&base->prog, keys, base->tik, base->ntik, base->tik_why, "The base game's Program NCA") &&
                 nca_keys(&upd->prog, keys, upd->tik, upd->ntik, upd->tik_why, "The update's Program NCA");
    if (ok && !(base->prog.sec[1].present && base->prog.sec[1].partition == 0 && base->prog.sec[1].crypt == 3))
        ok = fail("The base game's NSP has no plain RomFS section (is it the update's NSP twice?).");
    if (ok && !(upd->prog.sec[1].present && upd->prog.sec[1].partition == 0 && upd->prog.sec[1].crypt == 4))
        ok = fail("The update's NSP has no patch RomFS section (is it the base game's NSP twice?).");

    char tmp[1400] = "", tex[1400], tro[1400];
    if (ok) ok = join_path(tmp, sizeof tmp, job->out_dir, "import-tmp") && join_path(tex, sizeof tex, tmp, "exefs") &&
                 join_path(tro, sizeof tro, tmp, "romfs");
    if (ok) {
        remove_tree(tmp);  // (a stopped import's leftovers)
        ok = mkdir_p(tmp);
    }

    // the ExeFS first (small): a wrong version stops here, not after 12 GB
    if (ok) {
        say(job, "ExeFS");
        ok = exefs_extract(&upd->prog, tex, job->progress, job->user);
    }
    if (ok && job->verify_exefs) {
        char why[500] = "";
        if (!job->verify_exefs(tex, why, sizeof why)) ok = fail("%s", why[0] ? why : "The update's ExeFS isn't v1.4.0's.");
    }
    if (ok) {
        SecStor base_sec;
        sec_stor(&base_sec, &base->prog, 1);
        BktrStor bk;
        ok = bktr_open(&bk, &upd->prog, 1, &base_sec.s);
        if (ok) {
            // (the patched section is as big as the relocation table says; its RomFS image is the IVFC header's last level)
            ok = romfs_extract(&bk.s, romfs_start(&upd->prog.sec[1]), tro, job->progress, job->user);
            bktr_free(&bk);
        }
    }
    if (ok) ok = move_aside(job->out_dir);
    if (ok) {
        char ex[1400], ro[1400];
        ok = join_path(ex, sizeof ex, job->out_dir, "exefs") && join_path(ro, sizeof ro, job->out_dir, "romfs");
        if (!ok) {}
        else if (rename(tex, ex)) ok = fail("%s could not be moved to %s (%s).", tex, ex, strerror(errno));
        else if (rename(tro, ro)) ok = fail("%s could not be moved to %s (%s).", tro, ro, strerror(errno));
        else rmdir(tmp);
    }
    for (int i = 0; i < n; i++) nspfile_close(&nsp[i]);
    free(keys);
    if (!ok) {
        snprintf(err, err_size, "%s", g_err[0] ? g_err : "The import failed.");
        if (tmp[0]) remove_tree(tmp);  // (nothing half-written stays behind; the game folder's own files were not touched)
    }
    return ok;
}

// ------------------------------------------------------------------ prod.keys
static const char *const k_places =
    "  - prod.keys or keys\\prod.keys next to mhgurecomp.exe (keys/prod.keys on Linux)\n"
    "  - the .switch folder in your user folder (the one hactool uses)\n"
    "  - Ryujinx's system folder (%APPDATA%\\Ryujinx\\system, Linux ~/.config/Ryujinx/system)\n"
    "  - yuzu's keys folder (%APPDATA%\\yuzu\\keys, Linux ~/.local/share/yuzu/keys)";
const char *nsp_keys_places(void) { return k_places; }

bool nsp_find_keys(char *path, size_t size) {
    const char *env = getenv("MHGU_KEYS");
    const char *home = getenv("HOME");
    if (!home) home = getenv("USERPROFILE");
    const char *appdata = getenv("APPDATA");
    char cand[8][1100];
    int n = 0;
    if (env && *env) snprintf(cand[n++], sizeof cand[0], "%s", env);
    snprintf(cand[n++], sizeof cand[0], "prod.keys");
    snprintf(cand[n++], sizeof cand[0], "keys/prod.keys");
    if (home) snprintf(cand[n++], sizeof cand[0], "%s/.switch/prod.keys", home);
    if (appdata) {
        snprintf(cand[n++], sizeof cand[0], "%s/Ryujinx/system/prod.keys", appdata);
        snprintf(cand[n++], sizeof cand[0], "%s/yuzu/keys/prod.keys", appdata);
    }
    if (home) {
        snprintf(cand[n++], sizeof cand[0], "%s/.config/Ryujinx/system/prod.keys", home);
        snprintf(cand[n++], sizeof cand[0], "%s/.local/share/yuzu/keys/prod.keys", home);
    }
    for (int i = 0; i < n; i++) {
        FILE *f = fopen(cand[i], "rb");
        if (!f) continue;
        fclose(f);
        snprintf(path, size, "%s", cand[i]);
        return true;
    }
    return false;
}
