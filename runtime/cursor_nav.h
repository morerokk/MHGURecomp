// The mouse as a menu cursor, by POSITION and without the game's own row data: the finished frame is scanned for the
// menu's highlight (the gold selection bar every list of the game draws) and the host presses Up / Down in the menu view
// (MenuUp / MenuDown of runtime/actions.h) until the highlighted row is the one under the pointer. Pure logic (host-tested
// in tests/test_actions.c); the frame scan is called by the renderer (nvn/vulkan/device.c), the loop by host_input.c.
#pragma once
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

enum { CNAV_UP, CNAV_DOWN };

typedef struct {
    int y0, y1, x0, x1;  // frame pixels, inclusive
} MenuBand;
#define MENU_BANDS_MAX 6

// What the renderer found in the last scanned frame (written by the presenting thread, read by the game thread)
typedef struct {
    volatile uint32_t seq;   // odd while being written
    uint64_t ns;             // when
    int w, h;                // the frame's size in pixels
    int count;
    MenuBand band[MENU_BANDS_MAX];
} MenuScan;

// The highlight's colour: a saturated gold (the bar is about 202,156,19; the dimmed one in a list that does not have
// the focus is the same hue)
static inline bool menu_gold(uint8_t r, uint8_t g, uint8_t b) {
    return r > 150 && g > 100 && b < 100 && r - g > 15 && r - g < 90 && g - b > 50;
}

// Find the highlight bars of an RGBA / BGRA frame (4 bytes a pixel): runs of scanlines (12 to 90 tall) that are mostly
// gold over at least 120 pixels. Returns how many were put into out.
static inline int menu_scan_bands(const uint8_t *px, int w, int h, bool bgr, MenuBand *out, int max) {
    enum { MAXW = 8192 };
    if (w < 64 || h < 64 || w > MAXW) return 0;
    static int cnt_col[MAXW];
    int n = 0, y = 0;
    int ro = bgr ? 2 : 0, bo = bgr ? 0 : 2;
    int run0 = -1;
    for (y = 0; y <= h; y++) {
        int cnt = 0;
        if (y < h) {
            const uint8_t *row = px + (size_t)y * w * 4;
            for (int x = 0; x < w; x += 2) {
                const uint8_t *p = row + x * 4;
                cnt += menu_gold(p[ro], p[1], p[bo]);
            }
            cnt *= 2;
        }
        bool in = y < h && cnt >= 80;
        if (in && run0 < 0) run0 = y;
        if (!in && run0 >= 0) {
            int len = y - run0;
            if (len >= 12 && len <= 90 && n < max) {
                for (int x = 0; x < w; x++) cnt_col[x] = 0;
                for (int yy = run0; yy < y; yy += 2) {
                    const uint8_t *row = px + (size_t)yy * w * 4;
                    for (int x = 0; x < w; x++) cnt_col[x] += menu_gold(row[x * 4 + ro], row[x * 4 + 1], row[x * 4 + bo]);
                }
                int need = (len / 2) / 2 + 1, x0 = -1, x1 = -1;
                for (int x = 0; x < w; x++)
                    if (cnt_col[x] >= need) {
                        if (x0 < 0) x0 = x;
                        x1 = x;
                    }
                if (x0 >= 0 && x1 - x0 >= 120) out[n++] = (MenuBand){run0, y - 1, x0, x1};
            }
            run0 = -1;
        }
    }
    return n;
}

// The loop that walks the highlight to the row under the pointer.
typedef struct {
    float pitch;      // the distance between two rows, frame pixels (0: not known yet, from the bar's height)
    float prev_c;     // where the bar's centre was at the last decision
    int last_dir;     // the last press of this episode (-1: none)
    int stuck;        // presses in a row that moved nothing
    int presses;      // in this episode
    bool pressed;     // a press was made since the last decision
} CursorNav;

static inline void cursor_nav_reset(CursorNav *n) {  // a new episode: the pointer moved to a new place
    n->last_dir = -1, n->stuck = 0, n->presses = 0, n->pressed = false;
}

// One decision from a frame's bars and the pointer's place (frame pixels). Returns CNAV_UP / CNAV_DOWN to press, or -1.
// Rules: only a bar under the pointer's x counts (the nearest to its y); within 0.6 of a row of it: arrived; never
// turn back inside an episode (a disabled row is skipped, so the bar lands past it and stays there); give up after two
// presses that moved nothing or 40 presses. *count: how many presses to make now (1 to 3).
static inline int cursor_nav_decide(CursorNav *n, const MenuBand *b, int nb, float cx, float cy, int *count) {
    if (count) *count = 0;
    int best = -1;
    float bd = 1e9f;
    for (int i = 0; i < nb; i++) {
        if (cx < b[i].x0 - 8 || cx > b[i].x1 + 8) continue;
        float d = fabsf(cy - (b[i].y0 + b[i].y1) * 0.5f);
        if (d < bd) bd = d, best = i;
    }
    if (best < 0) {
        n->pressed = false;
        return -1;
    }
    float c = (b[best].y0 + b[best].y1) * 0.5f, hgt = (float)(b[best].y1 - b[best].y0 + 1);
    float def = hgt * 1.05f;
    if (n->pitch <= 0) n->pitch = def;
    if (n->pressed) {  // what the last press did
        float d = fabsf(c - n->prev_c);
        if (d < 2.0f) n->stuck++;
        else {
            n->stuck = 0;
            if (d > 0.7f * def && d < 1.3f * def) n->pitch = d;  // (one row: not a skipped disabled one)
        }
        n->pressed = false;
    }
    n->prev_c = c;
    float off = cy - c;
    if (fabsf(off) <= 0.6f * n->pitch) return -1;
    int dir = off < 0 ? CNAV_UP : CNAV_DOWN;
    if (n->stuck >= 2 || n->presses >= 40) return -1;
    if (n->last_dir >= 0 && n->last_dir != dir) return -1;
    // far away: up to three presses at once, always leaving a row for the next look (a disabled row can make a press move two)
    int k = (int)(fabsf(off) / n->pitch) - 1;
    k = k < 1 ? 1 : k > 3 ? 3 : k;
    if (count) *count = k;
    n->last_dir = dir, n->presses += k, n->pressed = true;
    return dir;
}
