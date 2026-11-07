// UI text spacing: letters at exact places, even margins and pair kerning (the game's festa font, Latin glyphs).
//
// How the game sets a text (cGUI text, 0xc43d38): a glyph node per letter; for each, x = the line's pen + the glyph's
// x offset (the font entry's signed byte at +0x10) x the text's scale (11/12 for 22 px text from the 24 px font), then
// 0xc45e64 moves the pen on by the glyph's cell width x scale (node +4) + the text's letter spacing. After the lines
// are aligned, every glyph is snapped: x = floor(x + 0.5) + 0.5, y alike (0xc45548). Three things make the letters
// look unevenly spaced, and [Graphics] FixKerning (on by default, live; off: all as the game does) fixes each:
//  - The snap is to the 1280x720 2D canvas. At 1080p a canvas unit is 1.5 px, so letters start on whole or half
//    pixels by turns and the same pair of letters sits up to 1.5 px further apart in one word than in another.
//    tools/gen.py PATCHES (0xc45554, 0xc4555c) leave x where the layout put it (+0.5, as the snap on average).
//  - The font's cells hold their letters unevenly (font_festa.gfd: 'e' has two blank columns on its left and none on
//    its right, 'n' one each side...), and the x offsets meant to correct that (-1 on c f m r s t, +1 on i l E H M N)
//    only move the letter, not the pen: a hole after the 'r' of "relies". Here every letter is placed as if its cell
//    were cut to one blank column each side of its ink (alpha >= 100, keeping every inked column), offsets ignored.
//  - The font has no kerning: "Va", "To", "r." keep the gap of their widest rows. Pairs whose ink, row by row, stays
//    2+ px further apart than the plain 2 px get closer (by 3/4 of the extra beyond 1 px, at most 3 px). Only pairs
//    with a letter, and none with a digit (numbers keep their widths).
// The measures come from the font the game reads (loc/arc/resident.arc, a mod's copy if one replaces it): the font
// file and its first glyph page, read once when the first text is set. A glyph whose entry isn't the measured one (an
// other font) keeps the game's placement.
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "../config.h"
#include "../hle.h"
#include "../romfs.h"

#define FN_PLACE_GLYPH 0xc45e64u  // (text, glyph node): moves the line's pen on past the glyph
#define TX_MODE        0x2cu      // text: 3 = a layout without pen (left alone)
#define TX_VERTICAL    0x44u      // text: 0 horizontal, 1 vertical
#define TX_LINE        0x50u      // text: the line being set (pen x at +4)
#define GN_ADVANCE     0x04u      // glyph node: how far the pen moves (cell width x scale)
#define GN_ENTRY       0x0cu      // glyph node: the font entry (code, packed cell, sizes, offsets)
#define GN_X           0x10u      // glyph node: x of the cell's left edge
#define GN_QUAD_W      0x18u      // glyph node: the cell's width x scale

#define ARC_PATH  "nativeNX/loc/arc/resident.arc"
#define FONT_NAME "GUI\\02_common\\font\\font_festa"
#define PAGE_NAME "GUI\\02_common\\font\\font_festa_00_HQ"
#define LATIN_END 0x250u
#define STRONG    100  // alpha of a letter's ink, for its margins
#define KERN_INK  102  // alpha of ink, for kerning (0.4)
#define ROWS      32
#define GAP       2    // px between letters' ink with plain margins
#define KERN_MAX  3

typedef struct {
    uint32_t code;
    uint32_t a, b;        // the font entry's packed cell (texture | x << 8 | y << 20; width | height << 12)
    int8_t left, width;   // the cut cell: from the cell's column `left`, `width` columns
    int8_t lp[ROWS], rp[ROWS];  // per row, the first / last inked column of the cut cell (99 / -99: none)
} Glyph;

static Glyph *g_glyph;          // [g_count]
static int16_t g_index[LATIN_END];  // code -> g_glyph index, -1: none
static int8_t *g_kern;          // [g_count * g_count], px
static int g_count;
static volatile int g_ready;    // 1: measured; -1: unavailable
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

// ------------------------------------------------------------------ the font, from resident.arc
static uint8_t *arc_entry(FILE *f, const char *want, uint32_t *size) {
    uint8_t head[12];
    if (fseek(f, 0, SEEK_SET) || fread(head, 1, 12, f) != 12 || memcmp(head, "ARC", 4)) return NULL;
    uint32_t n = head[6] | head[7] << 8;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t e[80];
        if (fseek(f, 12 + 80 * (long)i, SEEK_SET) || fread(e, 1, 80, f) != 80) return NULL;
        if (strncmp((const char *)e, want, 64)) continue;
        uint32_t cs, ds, off;
        memcpy(&cs, e + 68, 4), memcpy(&ds, e + 72, 4), memcpy(&off, e + 76, 4);
        ds &= 0x1fffffffu;
        uint8_t *src = malloc(cs), *dst = malloc(ds ? ds : 1);
        uLongf len = ds;
        bool ok = src && dst && !fseek(f, off, SEEK_SET) && fread(src, 1, cs, f) == cs &&
                  (cs == ds ? (memcpy(dst, src, ds), true) : uncompress(dst, &len, src, cs) == Z_OK && len == ds);
        free(src);
        if (!ok) { free(dst); return NULL; }
        *size = ds;
        return dst;
    }
    return NULL;
}

typedef struct {
    const uint8_t *data;
    uint32_t w, h, bh, gobs_w;
} Page;  // mip 0 of an RGBA8 block-linear (Tegra) texture

static bool page_open(Page *p, const uint8_t *t, uint32_t size) {
    if (size < 24 || memcmp(t, "TEX", 4) || t[4] != 0xa0 || t[13] != 7) return false;
    uint32_t t1, off0;
    memcpy(&t1, t + 8, 4);
    uint32_t mips = t1 & 0x3f;
    p->w = (t1 >> 6) & 0x1fff, p->h = t1 >> 19;
    memcpy(&off0, t + 20, 4);
    uint64_t start = 20 + 4ull * mips + off0;
    uint32_t bh = 1;
    while (bh < (p->h + 7) / 8 && bh < 16) bh <<= 1;
    p->bh = bh, p->gobs_w = (p->w * 4 + 63) / 64;
    uint64_t need = (uint64_t)p->gobs_w * 64 * ((p->h + 8 * bh - 1) / (8 * bh)) * 8 * bh;
    if (start + need > size) return false;
    p->data = t + start;
    return true;
}

static int alpha(const Page *p, uint32_t x, uint32_t y) {
    if (x >= p->w || y >= p->h) return 0;
    uint32_t bx = x * 4, bh = p->bh;
    uint32_t off = (y / (8 * bh)) * p->gobs_w * bh * 512 + (bx / 64) * bh * 512 + ((y % (8 * bh)) / 8) * 512 +
                   ((bx % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((bx % 32) / 16) * 32 + (y % 2) * 16 + bx % 16;
    return p->data[off + 3];
}

static int column_max(const Page *p, int x, uint32_t y, uint32_t h) {
    int m = 0;
    for (uint32_t r = 0; r < h && x >= 0; r++) {
        int v = alpha(p, (uint32_t)x, y + r);
        if (v > m) m = v;
    }
    return m;
}

static bool is_letter(uint32_t c) {  // (Unicode category L, U+0000-024F)
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == 0xaa || c == 0xb5 || c == 0xba ||
           (c >= 0xc0 && c < LATIN_END && c != 0xd7 && c != 0xf7);
}

static void measure(const Page *p, uint32_t code, uint32_t a, uint32_t b, Glyph *g) {
    int x = (int)((a >> 8) & 0xfff), w = (int)(b & 0xfff);
    uint32_t y = a >> 20, h = (b >> 12) & 0xfff;
    g->code = code, g->a = a, g->b = b, g->left = 0, g->width = (int8_t)w;
    if (is_letter(code)) {  // the cut cell: 1 blank column each side of the strong ink, every inked column kept
        int s0 = -1, s1 = -1, i0 = -1, i1 = -1;
        for (int cx = 0; cx < w; cx++) {
            int m = column_max(p, x + cx, y, h);
            if (m >= STRONG) { if (s0 < 0) s0 = cx; s1 = cx; }
            if (m) { if (i0 < 0) i0 = cx; i1 = cx; }
        }
        if (s0 >= 0) {
            int l = s0 - 1 < i0 ? s0 - 1 : i0, r = s1 + 1 > i1 ? s1 + 1 : i1;
            bool clear = true;  // columns taken in from outside the cell must be blank (another glyph's pixels)
            for (int cx = l; cx <= r && clear; cx++)
                if ((cx < 0 || cx >= w) && column_max(p, x + cx, y, h)) clear = false;
            if (clear) g->left = (int8_t)l, g->width = (int8_t)(r - l + 1);
        }
    }
    for (uint32_t r = 0; r < ROWS; r++) {
        g->lp[r] = 99, g->rp[r] = -99;
        if (r >= h) continue;
        for (int cx = 0; cx < g->width; cx++)
            if (alpha(p, (uint32_t)(x + g->left + cx), y + r) >= KERN_INK) {
                if (g->lp[r] == 99) g->lp[r] = (int8_t)cx;
                g->rp[r] = (int8_t)cx;
            }
    }
}

static bool is_digit(uint32_t c) { return c >= '0' && c <= '9'; }

static int kern_pair(const Glyph *l, const Glyph *r) {
    if (((l->b >> 12) & 0xfff) != ((r->b >> 12) & 0xfff)) return 0;  // (cells of one height: rows line up)
    // a letter and a letter or punctuation; digits never (numbers keep their widths: columns of figures line up)
    if (is_digit(l->code) || is_digit(r->code) || !(is_letter(l->code) || is_letter(r->code))) return 0;
    int gap = 99;
    for (int y = 0; y < ROWS; y++)
        if (l->rp[y] > -99 && r->lp[y] < 99) {
            int d = l->width + r->lp[y] - l->rp[y] - 1;
            if (d < gap) gap = d;
        }
    int excess = gap - GAP;
    if (gap == 99 || excess < 2) return 0;
    int k = (int)floorf((excess - 1) * 0.75f + 0.5f);
    return -(k < KERN_MAX ? k : KERN_MAX);
}

static void load(void) {
    uint64_t t0 = plat_now_ns();
    g_ready = -1;
    for (uint32_t c = 0; c < LATIN_END; c++) g_index[c] = -1;
    const char *path = romfs_host_path(ARC_PATH);
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (!f) { LOG("[text] %s not found: the game's spacing\n", ARC_PATH); return; }
    uint32_t gsize = 0, psize = 0;
    uint8_t *gfd = arc_entry(f, FONT_NAME, &gsize), *tex = arc_entry(f, PAGE_NAME, &psize);
    fclose(f);
    Page page;
    uint32_t count = 0, base = 0;
    if (gfd && gsize >= 0x48 && !memcmp(gfd, "GFD", 4)) {
        uint32_t name_len;
        memcpy(&count, gfd + 0x1c, 4), memcpy(&name_len, gfd + 0x44, 4);
        base = 0x48 + name_len + 1;
    }
    if (!count || base + (uint64_t)count * 20 > gsize || !tex || !page_open(&page, tex, psize)) {
        LOG("[text] %s: font %s unreadable: the game's spacing\n", ARC_PATH, FONT_NAME);
        free(gfd), free(tex);
        return;
    }
    g_glyph = calloc(LATIN_END, sizeof *g_glyph);
    for (uint32_t i = 0; i < count && g_glyph; i++) {
        uint32_t e[5];
        memcpy(e, gfd + base + 20 * i, 20);
        if (e[0] >= LATIN_END || (e[1] & 0xff) || g_index[e[0]] >= 0) continue;  // (Latin, on the first page)
        measure(&page, e[0], e[1], e[2], &g_glyph[g_count]);
        g_index[e[0]] = (int16_t)g_count++;
    }
    free(gfd), free(tex);
    g_kern = g_glyph ? malloc((size_t)g_count * g_count) : NULL;
    if (!g_kern) return;
    int pairs = 0;
    for (int i = 0; i < g_count; i++)
        for (int j = 0; j < g_count; j++) pairs += (g_kern[i * g_count + j] = (int8_t)kern_pair(&g_glyph[i], &g_glyph[j])) != 0;
    g_ready = 1;
    LOG("[text] %s: %d Latin glyphs measured, %d kerning pairs (%.0f ms)\n", FONT_NAME, g_count, pairs,
        (double)(plat_now_ns() - t0) / 1e6);
}

// ------------------------------------------------------------------ setting a text
static __thread uint32_t t_text, t_line;
static __thread int t_prev = -1;

static void place_glyph(Cpu *c) {
    uint32_t text = c->r[0], node = c->r[1];
    if (config_live_fix_kerning() && LD32(text + TX_MODE) != 3 && LD32(text + TX_VERTICAL) == 0) {
        pthread_once(&g_once, load);
        uint32_t entry = LD32(node + GN_ENTRY), line = LD32(text + TX_LINE);
        uint32_t code = entry ? LD32(entry) : 0xffffffffu;
        int i = g_ready > 0 && line && code < LATIN_END ? g_index[code] : -1;
        const Glyph *g = i >= 0 ? &g_glyph[i] : NULL;
        if (g && LD32(entry + 4) == g->a && LD32(entry + 8) == g->b && (g->b & 0xfff)) {
            float s = LDF(node + GN_QUAD_W) / (float)(g->b & 0xfff);
            if (s > 0.0f && s < 16.0f) {
                int k = text == t_text && line == t_line && t_prev >= 0 ? g_kern[t_prev * g_count + i] : 0;
                STF(node + GN_X, LDF(line + 4) + (float)(k - g->left) * s);
                STF(node + GN_ADVANCE, (float)(g->width + k) * s);
            }
            t_text = text, t_line = line, t_prev = i;
        } else {
            t_prev = -1;  // (a space, an icon, another font: no pair across it)
        }
    }
    fnhook_call_original(c, FN_PLACE_GLYPH);
}

void textlayout_init(void) {
    fnhook_set(FN_PLACE_GLYPH, hle_register("text:place_glyph", place_glyph));
}
