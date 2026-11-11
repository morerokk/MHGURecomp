// Keyboard and mouse button prompts: while the keyboard and mouse are what the player last pressed, the game's button
// glyphs ("(A)", "(ZL)" in its texts, on its HUD and in its windows) show the input that gives that button instead: a
// mouse picture (left, right or middle button, the wheel) or a key's cap with the key's name on it. The controller
// keeps the game's.
//
// The glyphs are cells of a few textures (sheets; docs/GAME-ACTIONS.md has the map):
//  - button_festa_00_ID (256 x 256): the button font (resident.arc GUI\02_common\font\button_festa, 24 x 24 cells,
//    named by fontIcon: PAD_A at 72,0...), for prompts in texts: the menus, the town's HUD, the Hunting Horn's notes.
//  - HD_custom_pnl2 (512 x 512): the hunt's HUD (L Y X B A R ZL ZR, D-pad, L3 R3, Minus Plus; the 3DS had no ZL / ZR,
//    so they are pictures of their own, not glyphs of the font).
//  - HD_hud00 (512 x 512): the item bar's L Y X B A R.
//  - HD_window (512 x 512, words in it: one per language): L and R (the quest details' pages), ZL and ZR.
// All BC3, Tegra block-linear. The game copies a texture's data as it is into its GPU memory (once per arc that holds
// it: resident.arc's and the quest arcs' copies are textures of their own), and the renderer uploads a texture again
// whenever that memory changes (nvn/vulkan/textures.c: page tracking, hashes). So a sheet's cells are redrawn, only the
// 4 x 4 blocks they touch encoded again, and the result written over each of the game's copies; the file's data goes
// back for the controller. The renderer reports every upload (prompts_texture_seen); a texture whose data is a sheet's
// file (in any language) is a copy of it.
//
// Which input: the button's action in the context on screen (the menus, a town, a hunt; actions.c), its binding for
// the keyboard and mouse (its own, the one it follows, or the device's default: actions.c act_default_binding), else
// what the settings' keybinds give that button (Tier 0). A key's name is drawn with the game's festa font (its first
// glyph page, the letters measured from their ink), short: "SPC", "ENT", "F5", arrows as arrows.
//
// Render thread (prompts_frame, at present; prompts_texture_seen from the texture upload).
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include <SDL3/SDL.h>

#include "../actions.h"
#include "../config.h"
#include "../hle.h"
#include "../romfs.h"
#include "../settings_panel.h"
#include "input.h"
#include "input_view.h"
#include "menuptr.h"
#include "prompt_icons.h"

#define ARC_PATH   "nativeNX/loc/arc/resident.arc"
#define FONT_NAME  "GUI\\02_common\\font\\font_festa"
#define PAGE_NAME  "GUI\\02_common\\font\\font_festa_00_HQ"
#define CELL       PROMPT_CELL
#define MAX_COPIES 8

// ------------------------------------------------------------------ the sheets
// A cell: its box in the sheet (what was the glyph's, cleared), the Switch button it is (or two: a pair of D-pad
// directions, the button font's KEY_UD / KEY_RL). The picture is drawn 24 x 24, then fitted into the box.
typedef struct { int x, y, w, h, btn, btn2; } Cell;
#define G(x, y, b) {x, y, 24, 24, b, -1}  // (the button font's 24 x 24 cells)
static const Cell font_cells[] = {
    G(0, 0, BTN_Y), G(24, 0, BTN_X), G(48, 0, BTN_B), G(72, 0, BTN_A), G(96, 0, BTN_L), G(0, 24, BTN_R),
    G(120, 24, BTN_ZL), G(144, 24, BTN_ZR), G(0, 72, BTN_PLUS), G(24, 72, BTN_MINUS), G(168, 24, BTN_LSTICK),
    G(192, 24, BTN_RSTICK), G(72, 24, BTN_UP), G(96, 24, BTN_RIGHT), G(0, 48, BTN_DOWN), G(24, 48, BTN_LEFT),
    {48, 48, 24, 24, BTN_UP, BTN_DOWN}, {72, 48, 24, 24, BTN_RIGHT, BTN_LEFT},
};
#undef G
// (the HUD sheets' boxes: each picture's ink, measured, and a texel around it)
static const Cell panel_cells[] = {  // HD_custom_pnl2
    {5, 247, 29, 26, BTN_L, -1},      {38, 246, 29, 29, BTN_Y, -1},      {70, 246, 28, 29, BTN_X, -1},
    {102, 246, 28, 28, BTN_B, -1},    {135, 246, 28, 29, BTN_A, -1},     {167, 247, 28, 26, BTN_R, -1},
    {198, 247, 28, 27, BTN_ZL, -1},   {229, 247, 29, 27, BTN_ZR, -1},    {263, 247, 26, 27, BTN_UP, -1},
    {295, 246, 26, 27, BTN_DOWN, -1}, {327, 246, 26, 27, BTN_RIGHT, -1}, {359, 244, 26, 32, BTN_RSTICK, -1},
    {390, 246, 28, 29, BTN_MINUS, -1}, {422, 246, 28, 28, BTN_PLUS, -1}, {359, 183, 26, 30, BTN_LSTICK, -1},
};
static const Cell hud_cells[] = {  // HD_hud00
    {1, 411, 29, 26, BTN_L, -1},  {34, 410, 29, 29, BTN_Y, -1},  {66, 410, 28, 29, BTN_X, -1},
    {98, 410, 28, 28, BTN_B, -1}, {131, 410, 28, 28, BTN_A, -1}, {163, 411, 28, 26, BTN_R, -1},
};
static const Cell window_cells[] = {  // HD_window
    {50, 84, 27, 25, BTN_L, -1}, {82, 84, 27, 25, BTN_R, -1}, {0, 192, 29, 28, BTN_ZL, -1}, {32, 193, 29, 27, BTN_ZR, -1},
};
static const char *const langs[] = {"eng", "fre", "ger", "ita", "spa", "chS", "chT", "jpn", "kor"};
#define NLANG (int)(sizeof langs / sizeof *langs)
typedef struct {
    const char *file;  // under nativeNX/<language>/
    int w, h;          // texels (BC3: a byte a texel)
    const Cell *cells;
    int ncells;
    uint8_t *var[NLANG];  // each language's file's data (NULL: none, or the same as an earlier language's)
    int active;           // the language the game's copies are (-1: none seen)
    uint8_t *rgba;        // its texels
    uint8_t *cur;         // what the copies hold now
    uint8_t *work, *out;  // (a redraw's texels and data)
    uint32_t addrs[MAX_COPIES];
    int naddrs;
    bool fresh;  // (a copy was found: it gets what the others have)
} Sheet;
#define SHEET(f, w, h, c) {f, w, h, c, (int)(sizeof c / sizeof *c), {0}, -1, NULL, NULL, NULL, NULL, {0}, 0, false}
static Sheet sheets[] = {
    SHEET("GUI/02_common/font/button_festa_00_ID.tex", 256, 256, font_cells),
    SHEET("GUI/99_texture/HD_custom_pnl2_BM_NOMIP.tex", 512, 512, panel_cells),
    SHEET("GUI/99_texture/HD_hud00_BM_NOMIP.tex", 512, 512, hud_cells),
    SHEET("GUI/99_texture/HD_window_BM_MQ_NOMIP.tex", 512, 512, window_cells),
};
#define NSHEETS (int)(sizeof sheets / sizeof *sheets)
static volatile int g_ready;  // 1: loaded; -1: not available

// The festa font's letters for the keys' names: each code's ink (alpha) in its 24-high cell
#define FONT_H 24
typedef struct { uint32_t code; int w; uint8_t a[FONT_H * 32]; } Letter;
static Letter g_letters[160];
static int g_nletters;

// ------------------------------------------------------------------ the game's files
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
        uint8_t *src = malloc(cs ? cs : 1), *dst = malloc(ds ? ds : 1);
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
// A TEX's mip 0: its data, width, height and format byte (NULL: not a TEX)
static const uint8_t *tex_data(const uint8_t *t, uint32_t size, uint32_t *w, uint32_t *h, int *fmt) {
    if (size < 24 || memcmp(t, "TEX", 4) || t[4] != 0xa0) return NULL;
    uint32_t t1, off0;
    memcpy(&t1, t + 8, 4), memcpy(&off0, t + 20, 4);
    *w = (t1 >> 6) & 0x1fff, *h = t1 >> 19, *fmt = t[13];
    uint64_t start = 20 + 4ull * (t1 & 0x3f) + off0;
    return start < size ? t + start : NULL;
}

// ------------------------------------------------------------------ BC3 (DXT5)
static void bc3_decode_block(const uint8_t *b, uint8_t out[16][4]) {
    int a0 = b[0], a1 = b[1], al[8] = {a0, a1};
    if (a0 > a1) for (int i = 1; i < 7; i++) al[i + 1] = ((7 - i) * a0 + i * a1) / 7;
    else { for (int i = 1; i < 5; i++) al[i + 1] = ((5 - i) * a0 + i * a1) / 5; al[6] = 0, al[7] = 255; }
    uint64_t abits = 0;
    for (int i = 0; i < 6; i++) abits |= (uint64_t)b[2 + i] << (8 * i);
    uint16_t c0 = b[8] | b[9] << 8, c1 = b[10] | b[11] << 8;
    uint32_t cb = b[12] | b[13] << 8 | b[14] << 16 | (uint32_t)b[15] << 24;
    int p[4][3];
    p[0][0] = (c0 >> 11) * 255 / 31, p[0][1] = (c0 >> 5 & 63) * 255 / 63, p[0][2] = (c0 & 31) * 255 / 31;
    p[1][0] = (c1 >> 11) * 255 / 31, p[1][1] = (c1 >> 5 & 63) * 255 / 63, p[1][2] = (c1 & 31) * 255 / 31;
    for (int k = 0; k < 3; k++) p[2][k] = (2 * p[0][k] + p[1][k]) / 3, p[3][k] = (p[0][k] + 2 * p[1][k]) / 3;
    for (int i = 0; i < 16; i++) {
        const int *c = p[cb >> (2 * i) & 3];
        out[i][0] = (uint8_t)c[0], out[i][1] = (uint8_t)c[1], out[i][2] = (uint8_t)c[2];
        out[i][3] = (uint8_t)al[abits >> (3 * i) & 7];
    }
}
static uint16_t to565(const int c[3]) {
    return (uint16_t)(((c[0] * 31 + 127) / 255) << 11 | ((c[1] * 63 + 127) / 255) << 5 | ((c[2] * 31 + 127) / 255));
}
static void from565(uint16_t v, int c[3]) {
    c[0] = (v >> 11) * 255 / 31, c[1] = (v >> 5 & 63) * 255 / 63, c[2] = (v & 31) * 255 / 31;
}
static void bc3_encode_block(const uint8_t px[16][4], uint8_t *b) {
    // alpha: its extremes, eight steps between them
    int amin = 255, amax = 0;
    for (int i = 0; i < 16; i++) amin = px[i][3] < amin ? px[i][3] : amin, amax = px[i][3] > amax ? px[i][3] : amax;
    b[0] = (uint8_t)amax, b[1] = (uint8_t)amin;
    int al[8] = {amax, amin};
    for (int i = 1; i < 7; i++) al[i + 1] = ((7 - i) * amax + i * amin) / 7;
    uint64_t abits = 0;
    for (int i = 0; i < 16; i++) {
        int best = 0, bd = 1 << 30;
        for (int k = 0; k < (amax > amin ? 8 : 1); k++) {
            int d = abs(al[k] - px[i][3]);
            if (d < bd) bd = d, best = k;
        }
        abits |= (uint64_t)best << (3 * i);
    }
    for (int i = 0; i < 6; i++) b[2 + i] = (uint8_t)(abits >> (8 * i));
    // colour: the two visible texels furthest apart, four steps between them
    int e0 = -1, e1 = -1, far = -1;
    for (int i = 0; i < 16; i++) {
        if (!px[i][3]) continue;
        if (e0 < 0) e0 = e1 = i, far = 0;
        for (int j = i + 1; j < 16; j++) {
            if (!px[j][3]) continue;
            int d = 0;
            for (int k = 0; k < 3; k++) d += (px[i][k] - px[j][k]) * (px[i][k] - px[j][k]);
            if (d > far) far = d, e0 = i, e1 = j;
        }
    }
    int c0[3] = {0, 0, 0}, c1[3] = {0, 0, 0};
    if (e0 >= 0) for (int k = 0; k < 3; k++) c0[k] = px[e0][k], c1[k] = px[e1][k];
    uint16_t v0 = to565(c0), v1 = to565(c1);
    if (v0 < v1) { uint16_t t = v0; v0 = v1, v1 = t; }
    uint32_t cbits = 0;
    if (v0 != v1) {  // (four-colour mode: v0 > v1)
        int p[4][3];
        from565(v0, p[0]), from565(v1, p[1]);
        for (int k = 0; k < 3; k++) p[2][k] = (2 * p[0][k] + p[1][k]) / 3, p[3][k] = (p[0][k] + 2 * p[1][k]) / 3;
        for (int i = 0; i < 16; i++) {
            int best = 0, bd = 1 << 30;
            for (int q = 0; q < 4; q++) {
                int d = 0;
                for (int k = 0; k < 3; k++) d += (p[q][k] - px[i][k]) * (p[q][k] - px[i][k]);
                if (d < bd) bd = d, best = q;
            }
            cbits |= (uint32_t)best << (2 * i);
        }
    }
    b[8] = (uint8_t)v0, b[9] = (uint8_t)(v0 >> 8), b[10] = (uint8_t)v1, b[11] = (uint8_t)(v1 >> 8);
    b[12] = (uint8_t)cbits, b[13] = (uint8_t)(cbits >> 8), b[14] = (uint8_t)(cbits >> 16), b[15] = (uint8_t)(cbits >> 24);
}
// The byte offset of 4 x 4 block (bx, by) of a BC3 sheet in the Tegra block-linear layout (16 bytes a block; the
// block height its size gets by default: 8 GOBs at 256, 16 at 512)
static int bh_log2(int h) {
    int gobs = (h / 4 + 7) / 8, l = 0;
    while ((1 << l) < gobs && l < 4) l++;
    return l;
}
static uint32_t block_off(const Sheet *s, int bx, int by) {
    uint32_t width_gobs = (uint32_t)(s->w / 4 * 16 + 63) / 64, bh = (uint32_t)bh_log2(s->h);
    uint32_t xb = (uint32_t)bx * 16, gob_x = xb >> 6, gob_y = (uint32_t)by >> 3;
    uint32_t block_y = gob_y >> bh, gin = gob_y & ((1u << bh) - 1), bs = 512u << bh;
    uint32_t off = block_y * width_gobs * bs + gob_x * bs + gin * 512, xi = xb & 63, yi = (uint32_t)by & 7;
    return off + (((xi >> 5) << 8) | ((yi >> 1) << 6) | (((xi >> 4) & 1) << 5) | ((yi & 1) << 4) | (xi & 15));
}
static void sheet_decode(const Sheet *s, const uint8_t *data, uint8_t *rgba) {
    for (int by = 0; by < s->h / 4; by++)
        for (int bx = 0; bx < s->w / 4; bx++) {
            uint8_t px[16][4];
            bc3_decode_block(data + block_off(s, bx, by), px);
            for (int i = 0; i < 16; i++) memcpy(rgba + (((size_t)(by * 4 + i / 4) * s->w) + bx * 4 + i % 4) * 4, px[i], 4);
        }
}
// The blocks of box (x, y, w, h) encoded again from rgba into data
static void sheet_encode_box(const Sheet *s, const uint8_t *rgba, uint8_t *data, int x, int y, int w, int h) {
    for (int by = y / 4; by <= (y + h - 1) / 4 && by < s->h / 4; by++)
        for (int bx = x / 4; bx <= (x + w - 1) / 4 && bx < s->w / 4; bx++) {
            uint8_t px[16][4];
            for (int i = 0; i < 16; i++) memcpy(px[i], rgba + (((size_t)(by * 4 + i / 4) * s->w) + bx * 4 + i % 4) * 4, 4);
            bc3_encode_block(px, data + block_off(s, bx, by));
        }
}

// ------------------------------------------------------------------ the festa font's letters
// (mip 0 of an RGBA8 block-linear page: textlayout.c's reading)
static int page_alpha(const uint8_t *data, uint32_t w, uint32_t h, uint32_t x, uint32_t y) {
    if (x >= w || y >= h) return 0;
    uint32_t bh = 1;
    while (bh < (h + 7) / 8 && bh < 16) bh <<= 1;
    uint32_t gobs_w = (w * 4 + 63) / 64, bx = x * 4;
    uint32_t off = (y / (8 * bh)) * gobs_w * bh * 512 + (bx / 64) * bh * 512 + ((y % (8 * bh)) / 8) * 512 +
                   ((bx % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((bx % 32) / 16) * 32 + (y % 2) * 16 + bx % 16;
    return data[off + 3];
}
static bool wanted_letter(uint32_t c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c >= 0x2190 && c <= 0x2193) || strchr("[];,./-=`'\\#", (int)c);
}
static void load_font(FILE *f) {
    uint32_t gsize = 0, psize = 0;
    uint8_t *gfd = arc_entry(f, FONT_NAME, &gsize), *tex = arc_entry(f, PAGE_NAME, &psize);
    uint32_t pw, ph;
    int pfmt;
    const uint8_t *page = tex ? tex_data(tex, psize, &pw, &ph, &pfmt) : NULL;
    if (gfd && gsize >= 0x48 && !memcmp(gfd, "GFD", 4) && page && pfmt == 7) {
        uint32_t count, name_len;
        memcpy(&count, gfd + 0x1c, 4), memcpy(&name_len, gfd + 0x44, 4);
        uint32_t base = 0x48 + name_len + 1;
        for (uint32_t i = 0; i < count && base + 20ull * (i + 1) <= gsize && g_nletters < 160; i++) {
            uint32_t e[3];
            memcpy(e, gfd + base + 20 * i, 12);
            if (!wanted_letter(e[0]) || (e[1] & 0xff)) continue;  // (first page only)
            int x = (int)((e[1] >> 8) & 0xfff), y = (int)(e[1] >> 20), w = (int)(e[2] & 0xfff), h = (int)((e[2] >> 12) & 0xfff);
            if (w > 32 || h > FONT_H) continue;
            Letter *l = &g_letters[g_nletters++];
            l->code = e[0], l->w = w;
            memset(l->a, 0, sizeof l->a);
            for (int r = 0; r < h; r++)
                for (int cx = 0; cx < w; cx++) l->a[r * 32 + cx] = (uint8_t)page_alpha(page, pw, ph, (uint32_t)(x + cx), (uint32_t)(y + r));
        }
    }
    free(gfd), free(tex);
}
static const Letter *letter(uint32_t c) {
    for (int i = 0; i < g_nletters; i++)
        if (g_letters[i].code == c) return &g_letters[i];
    return NULL;
}

static void load_sheets(void) {
    int found = 0;
    for (int i = 0; i < NSHEETS; i++) {
        Sheet *s = &sheets[i];
        for (int l = 0; l < NLANG; l++) {
            char rpath[160];
            snprintf(rpath, sizeof rpath, "nativeNX/%s/%s", langs[l], s->file);
            const char *path = romfs_host_path(rpath);
            FILE *f = path ? fopen(path, "rb") : NULL;
            if (!f) continue;
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            uint8_t *t = n > 0 ? malloc((size_t)n) : NULL;
            bool ok = t && fread(t, 1, (size_t)n, f) == (size_t)n;
            fclose(f);
            uint32_t w, h;
            int fmt;
            const uint8_t *data = ok ? tex_data(t, (uint32_t)n, &w, &h, &fmt) : NULL;
            uint64_t bytes = (uint64_t)s->w * s->h;
            if (data && (int)w == s->w && (int)h == s->h && fmt == 0x17 && data + bytes <= t + n) {
                bool same = false;  // (the languages' files are often one picture)
                for (int k = 0; k < l && !same; k++) same = s->var[k] && !memcmp(s->var[k], data, bytes);
                if (!same && (s->var[l] = malloc(bytes))) memcpy(s->var[l], data, bytes), found++;
            }
            free(t);
        }
    }
    const char *path = romfs_host_path(ARC_PATH);
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (f) load_font(f), fclose(f);
    g_ready = found ? 1 : -1;
    LOG("[prompts] %d button glyph pictures read (sheets x languages), %d letters of the festa font for keys' names\n", found,
        g_nletters);
}
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

// ------------------------------------------------------------------ the game's copies
// A copy is a texture whose data is a sheet's file on an upload; it then holds the file's data or what was last
// written (cur), else its memory went to something else (a quest's, after the quest) and it is forgotten, not written.
void prompts_texture_seen(uint32_t addr, const uint8_t *mem, uint64_t size) {
    bool any = false;
    for (int i = 0; i < NSHEETS && !any; i++) any = (uint64_t)sheets[i].w * sheets[i].h == size;
    if (!any) return;
    pthread_once(&g_once, load_sheets);
    if (g_ready <= 0) return;
    for (int i = 0; i < NSHEETS; i++) {
        Sheet *s = &sheets[i];
        uint64_t bytes = (uint64_t)s->w * s->h;
        if (bytes != size) continue;
        for (int l = 0; l < NLANG; l++) {
            if (!s->var[l] || memcmp(mem, s->var[l], bytes)) continue;
            if (s->active != l) {  // (the first copy, or another language: the copies so far were the other one's)
                if (!s->rgba) s->rgba = malloc(bytes * 4), s->work = malloc(bytes * 4), s->cur = malloc(bytes), s->out = malloc(bytes);
                if (!s->rgba || !s->work || !s->cur || !s->out) return;
                s->active = l, s->naddrs = 0;
                sheet_decode(s, s->var[l], s->rgba);
                memcpy(s->cur, s->var[l], bytes);
            }
            for (int k = 0; k < s->naddrs; k++)
                if (s->addrs[k] == addr) { s->fresh = true; return; }
            if (s->naddrs == MAX_COPIES) memmove(s->addrs, s->addrs + 1, sizeof s->addrs[0] * (MAX_COPIES - 1)), s->naddrs--;
            s->addrs[s->naddrs++] = addr, s->fresh = true;
            LOG("[prompts] %s (%s): a copy at %08x (%d known)\n", s->file, langs[l], addr, s->naddrs);
            return;
        }
    }
}
static void write_copies(Sheet *s, const uint8_t *data) {
    uint64_t bytes = (uint64_t)s->w * s->h;
    for (int i = 0; i < s->naddrs; i++) {
        uint8_t *dst = (uint8_t *)G2H(s->addrs[i]);
        if (memcmp(dst, s->var[s->active], bytes) && memcmp(dst, s->cur, bytes)) {  // (no longer the texture)
            LOG("[prompts] %s: the copy at %08x is gone\n", s->file, s->addrs[i]);
            memmove(s->addrs + i, s->addrs + i + 1, sizeof s->addrs[0] * (size_t)(s->naddrs - i - 1)), s->naddrs--, i--;
            continue;
        }
        memcpy(dst, data, bytes);
    }
    memcpy(s->cur, data, bytes);
}

// ------------------------------------------------------------------ which input a button is now
enum { SRC_NONE, SRC_LMB, SRC_RMB, SRC_MMB, SRC_WHEEL_UP, SRC_WHEEL_DOWN, SRC_KEY };
static int view_now(void) {
    const char *ctx = input_view_context();
    // (a hunt's menu: its pause menu handler stopped, a known menu in the pointer's focus, or any menu cursor running -
    // the pointer's focus alone missed one with the Hunting Horn out)
    if (!strcmp(ctx, "FIELD") && !input_view_hunt_menu_open() && !menuptr_open() && !menuptr_cursor_ran(150000000ull))
        return ACT_VIEW_MB1;
    if (!strcmp(ctx, "TOWN") && !menuptr_open()) return ACT_VIEW_TOWN;
    return ACT_VIEW_MB2;
}
// The first input the keyboard and mouse have for Switch button btn in a context; 0: none
static int input_for(const ActProfile *p, int view, int btn) {
    bool zr = input_view_dash_zr();
    bool layer = p->enable && (view != ACT_VIEW_MB2 || p->menus);
    if (layer) {
        int ids[ACT_COUNT], n = act_order(view, ids, ACT_COUNT);
        for (int k = 0; k < n; k++) {
            if (act_default_button(ids[k], zr) != btn) continue;
            const ActBinding *b = act_effective(p, 1, ids[k], 0, NULL);
            if (!b) b = act_default_binding(ids[k], 1);
            if (b) {
                if (!b->n) return 0;  // (bound to nothing)
                int c = b->alt[0].code[0];
                if (c >= ACT_SW && c < ACT_SW + 0x60) btn = (c - ACT_SW) % 0x20;  // (a Switch button's keys: below)
                else return c;
            }
            break;
        }
    }
    int src[8], n = host_input_sw_sources(btn, 1, src, 8);
    return n > 0 ? src[0] : 0;
}
static int kind_of(int code) {
    if (code == KEY_MOUSE + SDL_BUTTON_LEFT) return SRC_LMB;
    if (code == KEY_MOUSE + SDL_BUTTON_RIGHT) return SRC_RMB;
    if (code == KEY_MOUSE + SDL_BUTTON_MIDDLE) return SRC_MMB;
    if (code == KEY_WHEEL_UP) return SRC_WHEEL_UP;
    if (code == KEY_WHEEL_DOWN) return SRC_WHEEL_DOWN;
    return code > 0 ? SRC_KEY : SRC_NONE;
}
// A key's short name on its cap (Unicode code points; at most 3)
static int key_label(int code, uint32_t out[3]) {
    if (code == KEY_MOUSE + SDL_BUTTON_X1) return out[0] = 'M', out[1] = '4', 2;
    if (code == KEY_MOUSE + SDL_BUTTON_X2) return out[0] = 'M', out[1] = '5', 2;
    if (code <= 0 || code >= SDL_SCANCODE_COUNT) return 0;
    static const struct { SDL_Scancode sc; const char *s; uint32_t arrow; } named[] = {
        {SDL_SCANCODE_SPACE, "SPC", 0},      {SDL_SCANCODE_RETURN, "ENT", 0},    {SDL_SCANCODE_KP_ENTER, "ENT", 0},
        {SDL_SCANCODE_TAB, "TAB", 0},        {SDL_SCANCODE_BACKSPACE, "BS", 0},  {SDL_SCANCODE_ESCAPE, "ESC", 0},
        {SDL_SCANCODE_LSHIFT, "SHF", 0},     {SDL_SCANCODE_RSHIFT, "SHF", 0},    {SDL_SCANCODE_LCTRL, "CTL", 0},
        {SDL_SCANCODE_RCTRL, "CTL", 0},      {SDL_SCANCODE_LALT, "ALT", 0},      {SDL_SCANCODE_RALT, "ALT", 0},
        {SDL_SCANCODE_CAPSLOCK, "CAP", 0},   {SDL_SCANCODE_DELETE, "DEL", 0},    {SDL_SCANCODE_INSERT, "INS", 0},
        {SDL_SCANCODE_HOME, "HOM", 0},       {SDL_SCANCODE_END, "END", 0},       {SDL_SCANCODE_PAGEUP, "PGU", 0},
        {SDL_SCANCODE_PAGEDOWN, "PGD", 0},   {SDL_SCANCODE_UP, NULL, 0x2191},    {SDL_SCANCODE_DOWN, NULL, 0x2193},
        {SDL_SCANCODE_LEFT, NULL, 0x2190},   {SDL_SCANCODE_RIGHT, NULL, 0x2192},
    };
    for (unsigned i = 0; i < sizeof named / sizeof *named; i++)
        if ((int)named[i].sc == code) {
            if (named[i].arrow) return out[0] = named[i].arrow, 1;
            int n = 0;
            for (const char *s = named[i].s; *s && n < 3; s++) out[n++] = (uint8_t)*s;
            return n;
        }
    if (code >= SDL_SCANCODE_F1 && code <= SDL_SCANCODE_F12) {
        int f = code - SDL_SCANCODE_F1 + 1;
        out[0] = 'F';
        if (f < 10) return out[1] = (uint32_t)('0' + f), 2;
        return out[1] = '1', out[2] = (uint32_t)('0' + f - 10), 3;
    }
    if (code >= SDL_SCANCODE_KP_1 && code <= SDL_SCANCODE_KP_0)
        return out[0] = 'N', out[1] = (uint32_t)(code == SDL_SCANCODE_KP_0 ? '0' : '1' + code - SDL_SCANCODE_KP_1), 2;
    const char *name = SDL_GetScancodeName((SDL_Scancode)code);
    int n = 0;
    for (const char *s = name; s && *s && n < 3; s++)
        if (*s != ' ') out[n++] = (uint32_t)(*s >= 'a' && *s <= 'z' ? *s - 32 : (uint8_t)*s);
    return n;
}

// The label's letters, scaled down into the cap's face (ink at most FACE_W x FACE_H, centred on it), dark
#define FACE_X0 4.0f
#define FACE_X1 20.0f
#define FACE_Y0 3.5f
#define FACE_Y1 16.5f
#define STRIP_H (2 * FONT_H)
#define STRIP_W (3 * 32 + 8)
static uint8_t strip[STRIP_H][STRIP_W];
// The letters in a strip at the font's size (side by side, or two one above the other); its ink box and the scale
// that fits it into the cap's face. False: no ink.
static bool lay_out(const uint32_t *text, int n, bool stacked, int box[4], float *scale) {
    memset(strip, 0, sizeof strip);
    int sw = 0, below = 0;  // (stacked: the next letter's ink goes below the last one's, 3 rows apart)
    for (int i = 0; i < n; i++) {
        const Letter *l = letter(text[i]);
        if (!l) continue;
        int top = FONT_H, bottom = -1;
        for (int r = 0; r < FONT_H; r++)
            for (int cx = 0; cx < l->w; cx++)
                if (l->a[r * 32 + cx] > 24) top = r < top ? r : top, bottom = r > bottom ? r : bottom;
        if (bottom < 0) continue;
        int ox = stacked ? 0 : sw, oy = stacked ? below - top : 0;
        if (stacked) below += bottom - top + 1 + 3;
        if (oy + FONT_H > STRIP_H) oy = STRIP_H - FONT_H;
        for (int r = stacked ? top : 0; r < (stacked ? bottom + 1 : FONT_H); r++)  // (stacked: its ink rows only)
            for (int cx = 0; cx < l->w && ox + cx < STRIP_W; cx++)
                if (l->a[r * 32 + cx] > strip[oy + r][ox + cx]) strip[oy + r][ox + cx] = l->a[r * 32 + cx];
        sw = stacked ? (l->w > sw ? l->w : sw) : sw + l->w;
    }
    int x0 = 1 << 20, x1 = -1, y0 = 1 << 20, y1 = -1;
    for (int r = 0; r < STRIP_H; r++)
        for (int cx = 0; cx < sw && cx < STRIP_W; cx++)
            if (strip[r][cx] > 24) x0 = cx < x0 ? cx : x0, x1 = cx > x1 ? cx : x1, y0 = r < y0 ? r : y0, y1 = r > y1 ? r : y1;
    if (x1 < 0) return false;
    float iw = (float)(x1 - x0 + 1), ih = (float)(y1 - y0 + 1), fw = FACE_X1 - FACE_X0, fh = FACE_Y1 - FACE_Y0;
    box[0] = x0, box[1] = y0, box[2] = x1, box[3] = y1;
    *scale = fw / iw < fh / ih ? fw / iw : fh / ih;
    return true;
}
static void put_label(uint8_t *tile, const uint32_t *text, int n) {
    // side by side, or for two symbols one above the other where that draws them bigger (two arrows across)
    int box[4];
    float s;
    if (!lay_out(text, n, false, box, &s)) return;
    if (n == 2) {
        int box2[4];
        float s2;
        if (lay_out(text, n, true, box2, &s2) && s2 > s * 1.2f) memcpy(box, box2, sizeof box), s = s2;
        else lay_out(text, n, false, box, &s);
    }
    int x0 = box[0], x1 = box[2], y0 = box[1], y1 = box[3];
    float iw = (float)(x1 - x0 + 1), ih = (float)(y1 - y0 + 1);
    float fw = FACE_X1 - FACE_X0, fh = FACE_Y1 - FACE_Y0;
    if (s > 0.62f) s = 0.62f;  // (one letter: not taller than the cap's face wants)
    float ow = iw * s, oh = ih * s, ox = FACE_X0 + (fw - ow) / 2, oy = FACE_Y0 + (fh - oh) / 2;
    for (int py = 0; py < CELL; py++)
        for (int px = 0; px < CELL; px++) {
            // the strip's area this pixel covers: its ink, averaged
            float sx0 = ((float)px - ox) / s + (float)x0, sx1 = ((float)px + 1 - ox) / s + (float)x0;
            float sy0 = ((float)py - oy) / s + (float)y0, sy1 = ((float)py + 1 - oy) / s + (float)y0;
            if (sx1 <= (float)x0 || sx0 >= (float)x1 + 1 || sy1 <= (float)y0 || sy0 >= (float)y1 + 1) continue;
            float acc = 0, area = (sx1 - sx0) * (sy1 - sy0);
            for (int r = (int)sy0; r < (int)(sy1 + 0.999f); r++)
                for (int cx = (int)sx0; cx < (int)(sx1 + 0.999f); cx++) {
                    if (r < 0 || r >= STRIP_H || cx < 0 || cx >= STRIP_W) continue;
                    float wx = ((float)cx + 1 < sx1 ? (float)cx + 1 : sx1) - ((float)cx > sx0 ? (float)cx : sx0);
                    float wy = ((float)r + 1 < sy1 ? (float)r + 1 : sy1) - ((float)r > sy0 ? (float)r : sy0);
                    if (wx > 0 && wy > 0) acc += (float)strip[r][cx] * wx * wy;
                }
            float a = acc / area / 255.0f;
            if (a <= 0) continue;
            if (a > 1) a = 1;
            uint8_t *d = tile + (py * CELL + px) * 4;
            for (int k = 0; k < 3; k++) d[k] = (uint8_t)((float)d[k] * (1 - a) + 32.0f * a);  // (the ink: near black)
            if (d[3] < 255) d[3] = (uint8_t)((float)d[3] + (255.0f - (float)d[3]) * a);
        }
}
// ------------------------------------------------------------------ drawing a cell
// A cell's picture, 24 x 24: false where the game's glyph stays (no input, or none that can be drawn)
static bool draw_tile(uint8_t *tile, const Cell *c, int code, int code2) {
    int k = kind_of(code), k2 = c->btn2 >= 0 ? kind_of(code2) : SRC_NONE;
    if (k == SRC_NONE) return false;
    if (c->btn2 >= 0) {  // (two D-pad directions: both keys' symbols on one cap, else the game's)
        uint32_t a[3], b[3];
        int na = k == SRC_KEY ? key_label(code, a) : 0, nb = k2 == SRC_KEY ? key_label(code2, b) : 0;
        if (na != 1 || nb != 1) return false;
        uint32_t t[2] = {a[0], b[0]};
        memcpy(tile, prompt_icons[PROMPT_KEYCAP], CELL * CELL * 4);
        put_label(tile, t, 2);
        return true;
    }
    int icon = k == SRC_LMB ? PROMPT_LMB : k == SRC_RMB ? PROMPT_RMB : k == SRC_MMB ? PROMPT_MMB
             : k == SRC_WHEEL_UP ? PROMPT_WHEEL_UP : k == SRC_WHEEL_DOWN ? PROMPT_WHEEL_DOWN : -1;
    if (icon >= 0) return memcpy(tile, prompt_icons[icon], CELL * CELL * 4), true;
    uint32_t t[3];
    int n = key_label(code, t);
    if (!n) return false;
    memcpy(tile, prompt_icons[PROMPT_KEYCAP], CELL * CELL * 4);
    put_label(tile, t, n);
    return true;
}
// The tile into a cell's box of a sheet's texels: the box cleared, the tile fitted into its square (the smaller side),
// centred; scaled by area, colour weighted by alpha
static void put_tile(const Sheet *s, uint8_t *rgba, const Cell *c, const uint8_t *tile) {
    for (int y = c->y; y < c->y + c->h; y++) memset(rgba + ((size_t)y * s->w + c->x) * 4, 0, (size_t)c->w * 4);
    int side = c->w < c->h ? c->w : c->h, ox = c->x + (c->w - side) / 2, oy = c->y + (c->h - side) / 2;
    float f = (float)CELL / (float)side;
    for (int py = 0; py < side; py++)
        for (int px = 0; px < side; px++) {
            float sx0 = (float)px * f, sx1 = (float)(px + 1) * f, sy0 = (float)py * f, sy1 = (float)(py + 1) * f;
            float acc[4] = {0, 0, 0, 0}, area = 0;
            for (int ty = (int)sy0; ty < (int)(sy1 + 0.999f) && ty < CELL; ty++)
                for (int tx = (int)sx0; tx < (int)(sx1 + 0.999f) && tx < CELL; tx++) {
                    float wx = ((float)tx + 1 < sx1 ? (float)tx + 1 : sx1) - ((float)tx > sx0 ? (float)tx : sx0);
                    float wy = ((float)ty + 1 < sy1 ? (float)ty + 1 : sy1) - ((float)ty > sy0 ? (float)ty : sy0);
                    if (wx <= 0 || wy <= 0) continue;
                    const uint8_t *p = tile + (ty * CELL + tx) * 4;
                    float a = (float)p[3] * wx * wy;
                    acc[0] += (float)p[0] * a, acc[1] += (float)p[1] * a, acc[2] += (float)p[2] * a, acc[3] += a, area += wx * wy;
                }
            uint8_t *d = rgba + ((size_t)(oy + py) * s->w + ox + px) * 4;
            if (acc[3] > 0) for (int k = 0; k < 3; k++) d[k] = (uint8_t)(acc[k] / acc[3] + 0.5f);
            d[3] = (uint8_t)(area > 0 ? acc[3] / area + 0.5f : 0);
        }
}

// ------------------------------------------------------------------ each frame
void prompts_frame(void) {
    if (g_ready <= 0) return;
    static int shown[16];
    static bool shown_kbm, any_shown;
    static int shown_view = -1;
    bool kbm = config_live_kbm_prompts() && host_input_last_device() == 1;
    int want[16], view = -1;  // (each Switch button's input now; 0: the game's glyph)
    memset(want, 0, sizeof want);
    if (kbm) {
        ActProfile p;
        act_get_profile(&p);
        view = view_now();
        for (int b = 0; b < 16; b++) want[b] = input_for(&p, view, b);
    }
    if (view != shown_view && kbm) {
        static int logged_views;
        if (logged_views++ < 200)
            LOG("[prompts] context: %s\n", view == ACT_VIEW_MB1 ? "hunt" : view == ACT_VIEW_TOWN ? "town" : "menus");
        shown_view = view;
    }
    bool changed = !any_shown || kbm != shown_kbm || memcmp(want, shown, sizeof want);
    memcpy(shown, want, sizeof want), shown_kbm = kbm, any_shown = true;
    const char *dump = getenv("MHGU_PROMPTS_DUMP");  // (tests: each sheet's texels as drawn, <prefix><n>.raw)
    for (int i = 0; i < NSHEETS; i++) {
        Sheet *s = &sheets[i];
        if (!s->naddrs || (!changed && !s->fresh)) continue;
        s->fresh = false;
        uint64_t bytes = (uint64_t)s->w * s->h;
        if (!kbm) { write_copies(s, s->var[s->active]); continue; }
        memcpy(s->work, s->rgba, bytes * 4);
        memcpy(s->out, s->var[s->active], bytes);
        int drawn = 0;
        for (int c = 0; c < s->ncells; c++) {
            const Cell *cell = &s->cells[c];
            uint8_t tile[CELL * CELL * 4];
            if (!draw_tile(tile, cell, want[cell->btn], cell->btn2 >= 0 ? want[cell->btn2] : 0)) continue;
            put_tile(s, s->work, cell, tile);
            sheet_encode_box(s, s->work, s->out, cell->x, cell->y, cell->w, cell->h);
            drawn++;
        }
        write_copies(s, s->out);
        if (dump) {
            char path[512];
            snprintf(path, sizeof path, "%s%d.raw", dump, i);
            FILE *df = fopen(path, "wb");
            if (df) {
                sheet_decode(s, s->out, s->work);  // (what the GPU gets: after BC3)
                fwrite(s->work, 1, bytes * 4, df), fclose(df);
            }
        }
        static int logged;
        if (logged++ < 32) LOG("[prompts] %s: %d of %d button glyphs drawn for keyboard and mouse\n", s->file, drawn, s->ncells);
    }
}
