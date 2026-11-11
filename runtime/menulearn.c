// Where a menu's rows are on screen, learned from what it draws (runtime/hle/menuptr.c uses it for the mouse).
//
// The renderer gives every HUD piece of a frame here (nvn/vulkan/exec.c cbuf_info: a 2D piece's matrix: m[12] / m[13] its
// top left on the 1280x720 screen as the game lays it out, m[14] its layer, m[0] / m[5] half its width / height: the
// quads span 0..2). When the menu in focus has kept a row for a moment, the frame is kept; when it then keeps another
// row of the same page, the two frames are compared: the menu's highlight bar is what moved, by a whole number of row
// steps, along one axis, with its size and layer the same (the pause menu: one piece, layer 97, 282 x 34 at 970,101,
// 36 lower a row; nothing else moved but the name tags over the 3D world, whose place, size and layer all change). The
// pieces that moved that way, together, are the bar; the first row's place and the step make the menu's geometry, kept
// once two changes agree. Render thread.
//
// MHGU_CONTROL "pieces": log every piece of the next frame.
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "hle/menuptr.h"
#include "rt.h"

typedef struct { float x, y, z, w, h; } Piece;
#define MAX_PIECES 1024
#define STABLE_FRAMES 12  // presented frames a row must be kept before its frame counts
static Piece g_cur[MAX_PIECES], g_prev[MAX_PIECES];
static int g_ncur, g_nprev;
static uint32_t g_prev_lr;
static int g_prev_row = -1, g_prev_page;
static volatile int g_log_request;  // (control "pieces": log the next frame's)
static int g_logging;

// Where the pieces end up: on a picture not 16:9 the renderer moves them (narrowed toward their side, or flattened
// toward the top or bottom, a window's pieces as one: exec.c), into a copy of their matrix; its m[12] / m[13] are the
// piece's place in the picture's 1280 x 720 (its whole width and height), final once the frame is done. NULL: not moved.
static const float *g_final[MAX_PIECES];

void menulearn_request_log(void) { g_log_request = 1; }

void menulearn_piece(const float *m) {
    if (g_ncur >= MAX_PIECES) return;
    Piece p = {m[12], m[13], m[14], m[0], m[5]};
    g_final[g_ncur] = NULL;
    g_cur[g_ncur++] = p;
    if (g_logging)
        LOG("[pieces] z %.1f pos %.1f,%.1f m0 %.2f m1 %.2f m4 %.2f m5 %.2f\n", m[14], m[12], m[13], m[0], m[1], m[4], m[5]);
}
void menulearn_piece_final(const float *m) {
    if (g_ncur > 0 && g_ncur <= MAX_PIECES) g_final[g_ncur - 1] = m;
}

// The menu the pointer works on, from its layout to the picture: a piece of it (one inside its cells' area) and where
// that piece ended up give the offset; the scale is the renderer's (narrowed sideways wider than 16:9, flattened
// narrower). For host_input.c's mapping of the pointer.
static void map_menu(void) {
    MenuGeom g;
    if (!menuptr_geometry(&g)) return;
    int cells = menuptr_rows(), cols = menuptr_cols();
    if (cells <= 0) return;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    int last_in_row = (cells < cols ? cells : cols) - 1, corners[3] = {0, last_in_row > 0 ? last_in_row : 0, cells - 1};
    for (int k = 0; k < 3; k++) {
        float a0, b0, a1, b1;
        menuptr_row_rect(&g, corners[k], cols, &a0, &b0, &a1, &b1);
        x0 = fminf(x0, a0), y0 = fminf(y0, b0), x1 = fmaxf(x1, a1), y1 = fmaxf(y1, b1);
    }
    float asp = g_render_aspect > 0 ? g_render_aspect : 16.0f / 9.0f;
    float ax = asp > 16.0f / 9.0f ? (16.0f / 9.0f) / asp : 1.0f, ay = asp < 16.0f / 9.0f ? asp / (16.0f / 9.0f) : 1.0f;
    for (int i = 0; i < g_ncur; i++) {
        const Piece *p = &g_cur[i];
        if (p->z > 200.0f || p->x < x0 - 8 || p->x > x1 + 8 || p->y < y0 - 8 || p->y > y1 + 8) continue;
        float fx = g_final[i] ? g_final[i][12] : p->x, fy = g_final[i] ? g_final[i][13] : p->y;
        menuptr_set_screen_map(ax, fx - ax * p->x, ay, fy - ay * p->y);
        return;
    }
}

static bool near(float a, float b) { return fabsf(a - b) < 0.01f; }
static bool same_piece(const Piece *a, const Piece *b) {
    return near(a->x, b->x) && near(a->y, b->y) && near(a->z, b->z) && near(a->w, b->w) && near(a->h, b->h);
}
static bool in(const Piece *list, int n, const Piece *p) {
    for (int i = 0; i < n; i++)
        if (same_piece(&list[i], p)) return true;
    return false;
}

// The highlight bar between g_prev (row r0) and g_cur (row r1): the step a row, the bar's box at r1, and its biggest piece
// (its signature: runtime/hle/menuptr.h). False: none.
static bool find_bar(int dr, float *sx, float *sy, float box[4], Piece *key) {
    // the step: the move of the biggest piece that moved along one axis by a multiple of dr, size and layer kept
    float best = 0;
    for (int i = 0; i < g_ncur; i++) {
        const Piece *b = &g_cur[i];
        if (in(g_prev, g_nprev, b)) continue;  // (it did not move)
        float area = fabsf(4.0f * b->w * b->h);
        if (area < 100.0f || area <= best) continue;
        for (int j = 0; j < g_nprev; j++) {
            const Piece *a = &g_prev[j];
            if (!near(a->z, b->z) || !near(a->w, b->w) || !near(a->h, b->h) || in(g_cur, g_ncur, a)) continue;
            float dx = (b->x - a->x) / (float)dr, dy = (b->y - a->y) / (float)dr;
            bool along_y = fabsf(b->x - a->x) < 0.5f && fabsf(dy) >= 4.0f, along_x = fabsf(b->y - a->y) < 0.5f && fabsf(dx) >= 4.0f;
            if (!along_x && !along_y) continue;
            best = area, *sx = along_x ? dx : 0, *sy = along_y ? dy : 0, *key = *b;
            break;
        }
    }
    if (best == 0) {
        // A frame drawn at a small scale (its size in its vertices, not its matrix: the item box's cursor at 1 x 1, the
        // character creation menu's at 2 x 2, the creation grids' in three pieces) has no size here: it counts when every
        // small piece that moved moved the same way (one frame, or the pieces of one). Its box is their top left corner
        // (its key the piece there); the cells take the steps.
        int found = 0;
        for (int i = 0; i < g_ncur; i++) {
            const Piece *b = &g_cur[i];
            if (fabsf(4.0f * b->w * b->h) >= 100.0f || in(g_prev, g_nprev, b)) continue;
            for (int j = 0; j < g_nprev; j++) {
                const Piece *a = &g_prev[j];
                if (!near(a->z, b->z) || !near(a->w, b->w) || !near(a->h, b->h) || in(g_cur, g_ncur, a)) continue;
                float dx = (b->x - a->x) / (float)dr, dy = (b->y - a->y) / (float)dr;
                bool along_y = fabsf(b->x - a->x) < 0.5f && fabsf(dy) >= 4.0f, along_x = fabsf(b->y - a->y) < 0.5f && fabsf(dx) >= 4.0f;
                if (!along_x && !along_y) continue;
                float mx = along_x ? dx : 0, my = along_y ? dy : 0;
                if (found && (fabsf(mx - *sx) > 0.5f || fabsf(my - *sy) > 0.5f)) return false;  // (moved differently: which is
                                                                                              // the cursor isn't known)
                if (!found || b->y < box[1] - 0.5f || (fabsf(b->y - box[1]) <= 0.5f && b->x < box[0]))
                    *key = *b, box[0] = box[2] = b->x, box[1] = box[3] = b->y;
                *sx = mx, *sy = my, found++;
                break;
            }
        }
        return found > 0;
    }
    // the bar: every piece that moved by that much (a bar drawn in pieces: ends and middle), as one box
    box[0] = box[1] = 1e9f, box[2] = box[3] = -1e9f;
    for (int i = 0; i < g_ncur; i++) {
        const Piece *b = &g_cur[i];
        if (in(g_prev, g_nprev, b)) continue;
        for (int j = 0; j < g_nprev; j++) {
            const Piece *a = &g_prev[j];
            if (!near(a->z, b->z) || !near(a->w, b->w) || !near(a->h, b->h)) continue;
            if (fabsf(b->x - a->x - *sx * (float)dr) > 0.5f || fabsf(b->y - a->y - *sy * (float)dr) > 0.5f) continue;
            float x0 = fminf(b->x, b->x + 2 * b->w), x1 = fmaxf(b->x, b->x + 2 * b->w);
            float y0 = fminf(b->y, b->y + 2 * b->h), y1 = fmaxf(b->y, b->y + 2 * b->h);
            box[0] = fminf(box[0], x0), box[1] = fminf(box[1], y0), box[2] = fmaxf(box[2], x1), box[3] = fmaxf(box[3], y1);
            break;
        }
    }
    return box[2] > box[0] && box[3] > box[1];
}

// Two changes of a menu that agree (the same bar and step: the place may differ, a popup) make a step of its geometry: a
// row's (Up / Down) or a column's (Left / Right), each kept apart until it is seen twice
static bool same_bar(const MenuGeom *a, const MenuGeom *b) {
    return fabsf(a->z - b->z) < 0.5f && fabsf(a->kw - b->kw) < 0.01f && fabsf(a->kh - b->kh) < 0.01f && fabsf(a->w - b->w) < 0.5f &&
           fabsf(a->h - b->h) < 0.5f && fabsf(a->dx - b->dx) < 0.5f && fabsf(a->dy - b->dy) < 0.5f && fabsf(a->cdx - b->cdx) < 0.5f &&
           fabsf(a->cdy - b->cdy) < 0.5f;
}
static struct { MenuGeom g; int axis, seen; } g_pending[16];
static void observe(const MenuGeom *g, int axis) {
    int k = -1, free_k = 0;
    for (int i = 0; i < 16; i++) {
        if (g_pending[i].g.lr == g->lr && g_pending[i].axis == axis) { k = i; break; }
        if (!g_pending[i].g.lr) free_k = i;
    }
    if (k < 0) k = free_k, g_pending[k].seen = 0, g_pending[k].axis = axis;
    bool agree = g_pending[k].seen && same_bar(&g_pending[k].g, g);
    g_pending[k].g = *g;
    g_pending[k].seen = agree ? g_pending[k].seen + 1 : 1;
    if (g_pending[k].seen >= 2) menuptr_learned(g);  // (again later: a new place of a known bar is not written)
}

// The geometry of lr with the bar of key (its steps and rows' places so far), else one with none
static MenuGeom known_like(uint32_t lr, const Piece *key) {
    MenuGeom v[4], none = {0};
    int n = menuptr_variants(lr, v, 4);
    for (int k = 0; k < n; k++)
        if (v[k].kw && near(v[k].z, key->z) && near(v[k].kw, key->w) && near(v[k].kh, key->h)) return v[k];
    return none;
}

// Where the bar of the menu the pointer works on is this frame: a piece of its signature (of any of the call's
// geometries; the one nearest where that geometry was, when there are several), the cells placed from it
static void follow(void) {
    uint32_t lr;
    int row, col;
    uint64_t since;
    if (!menuptr_pointer_focus(&lr, &row, &col, &since)) return;
    if (g_present_count - since < 3) return;  // (the cell just changed: the frame may still show the old one)
    MenuGeom v[4], best;
    int n = menuptr_variants(lr, v, 4);
    float best_d = 1e18f, bx = 0, by = 0;
    for (int k = 0; k < n; k++) {
        if (!v[k].kw) continue;  // (no signature: a fixed place)
        float ox, oy;  // (where this cell is from the first: its row's own place when known, else by steps)
        menuptr_row_offset(&v[k], row, &ox, &oy);
        ox += v[k].cdx * (float)col, oy += v[k].cdy * (float)col;
        // (a frame of a small scale, which many other pieces share - icons, letters: not followed, its menu stays where
        // it was learned; scan_small finds it on its cells)
        if (fabsf(4.0f * v[k].kw * v[k].kh) < 100.0f) continue;
        for (int i = 0; i < g_ncur; i++) {
            const Piece *p = &g_cur[i];
            if (!near(p->z, v[k].z) || !near(p->w, v[k].kw) || !near(p->h, v[k].kh)) continue;
            MenuGeom g = v[k];
            g.x = p->x + g.ox - ox, g.y = p->y + g.oy - oy;
            float d = (g.x - v[k].x) * (g.x - v[k].x) + (g.y - v[k].y) * (g.y - v[k].y);
            if (d < best_d) best_d = d, best = g, bx = p->x + g.ox - g.cdx * (float)col, by = p->y + g.oy - g.cdy * (float)col;
        }
    }
    menuptr_live(best_d < 1e18f ? &best : NULL);
    if (best_d < 1e18f) menuptr_row_seen(&best, row, bx, by);  // (rows not one step apart: their own places)
}

// The menus of a small-scale frame: a piece of the frame's signature exactly on one of a menu's cells (its key piece's
// place there, to a pixel) means that menu is open on that cell - also before its cursor first runs (character
// creation's grids run theirs only on a press, and are not known to be open until then otherwise)
static void scan_small(void) {
    MenuGeom sg[32];
    int n = menuptr_small_geoms(sg, 32);
    for (int k = 0; k < n; k++) {
        const MenuGeom *g = &sg[k];
        if (menuptr_frame_taken(g)) continue;  // (a look-alike ran lately: the frame is its - Style's grid and the creation menu)
        float kx0 = g->x - g->ox, ky0 = g->y - g->oy;  // (the key piece on the first cell)
        for (int i = 0; i < g_ncur; i++) {
            const Piece *p = &g_cur[i];
            if (!near(p->z, g->z) || !near(p->w, g->kw) || !near(p->h, g->kh)) continue;
            float rx = p->x - kx0, ry = p->y - ky0;
            int col = g->cdx ? (int)lroundf(rx / g->cdx) : g->cdy ? (int)lroundf(ry / g->cdy) : 0;
            if (col < 0 || col >= g->cols) continue;
            float ry2 = ry - g->cdy * (float)col, rx2 = rx - g->cdx * (float)col;
            int row = g->dy ? (int)lroundf(ry2 / g->dy) : g->dx ? (int)lroundf(rx2 / g->dx) : 0;
            if (row < 0 || row >= 64) continue;
            float ox, oy;
            menuptr_row_offset(g, row, &ox, &oy);
            if (fabsf(rx2 - ox) >= 1.0f || fabsf(ry2 - oy) >= 1.0f) continue;
            menuptr_bar_at(g, row, col);
            break;
        }
    }
}

static int g_prev_col;
static uint64_t g_prev_ns;  // when the kept frame was taken
#define PREV_KEEP_NS 30000000000ull
void menulearn_frame(void) {
    if (g_logging) LOG("[pieces] frame done: %d pieces\n", g_ncur), g_logging = 0;
    if (g_log_request) g_log_request = 0, g_logging = 1;
    scan_small();
    follow();
    map_menu();
    uint32_t lr = 0;
    int row = 0, col = 0, page = 0;
    uint64_t since = 0;
    int cols = 0;
    bool focus = menuptr_focus(&lr, &row, &col, &page, &since, &cols);
    static int dbg_focus = -1;
    if (dbg_focus < 0) dbg_focus = getenv("MHGU_MENULEARN_LOG") != NULL;
    if (dbg_focus && g_present_count % 240 == 0)
        LOG("[menulearn] focus %s %08x row %d column %d page %d, unchanged for %llu frames\n", focus ? "on" : "none", focus ? lr : 0,
            row, col, page, focus ? (unsigned long long)(g_present_count - since) : 0ull);
    // (no menu in focus: the last kept frame stays, for a cursor that runs only on a button press - it drops out between
    // presses, and its next move is compared with it; for at most PREV_KEEP_NS, and only the same menu on the same page)
    if (!focus) {
        if (g_prev_row >= 0 && plat_now_ns() - g_prev_ns > PREV_KEEP_NS) g_prev_row = -1;
    } else if (g_present_count - since >= STABLE_FRAMES) {
        // a row changed in the same column (Up / Down), or a column in the same row (Left / Right)
        bool by_row = row != g_prev_row && col == g_prev_col, by_col = col != g_prev_col && row == g_prev_row;
        static int dbg = -1;  // (MHGU_MENULEARN_LOG=1: each change looked at)
        if (dbg < 0) dbg = getenv("MHGU_MENULEARN_LOG") != NULL;
        if (dbg && g_prev_row >= 0 && (lr != g_prev_lr || row != g_prev_row || col != g_prev_col || page != g_prev_page))
            LOG("[menulearn] %08x row %d -> %d, column %d -> %d, page %d -> %d (was %08x), %d pieces\n", lr, g_prev_row, row,
                g_prev_col, col, g_prev_page, page, g_prev_lr, g_ncur);
        if (g_prev_row >= 0 && lr == g_prev_lr && page == g_prev_page && (by_row || by_col)) {
            float sx, sy, box[4];
            Piece key;
            bool found = find_bar(by_row ? row - g_prev_row : col - g_prev_col, &sx, &sy, box, &key);
            if (dbg) LOG("[menulearn]   %s step: %s %+.1f,%+.1f at %.0f,%.0f\n", by_row ? "row" : "column", found ? "found" : "none", sx, sy,
                         box[0], box[1]);
            if (found) {
                MenuGeom k = known_like(lr, &key), g = {0};
                g.lr = lr, g.w = box[2] - box[0], g.h = box[3] - box[1];
                g.z = key.z, g.kw = key.w, g.kh = key.h, g.ox = box[0] - key.x, g.oy = box[1] - key.y;
                g.cols = cols;
                if (by_row) g.dx = sx, g.dy = sy, g.cdx = k.cdx, g.cdy = k.cdy;
                else g.cdx = sx, g.cdy = sy, g.dx = k.dx, g.dy = k.dy;
                g.offmask = k.offmask, memcpy(g.offx, k.offx, sizeof g.offx), memcpy(g.offy, k.offy, sizeof g.offy);
                float ox, oy;  // (the first cell: back from this one by its row's place and its column's steps)
                menuptr_row_offset(&g, row, &ox, &oy);
                g.x = box[0] - ox - g.cdx * (float)col, g.y = box[1] - oy - g.cdy * (float)col;
                MenuGeom step = g;  // (what is learned: only the step this change shows)
                if (by_row) step.cdx = step.cdy = 0;
                else step.dx = step.dy = 0;
                step.offmask = 0;
                observe(&step, by_row ? 0 : 1);
            }
        }
        memcpy(g_prev, g_cur, sizeof *g_cur * (size_t)g_ncur);  // (the latest frame of a kept cell)
        g_nprev = g_ncur, g_prev_lr = lr, g_prev_row = row, g_prev_col = col, g_prev_page = page, g_prev_ns = plat_now_ns();
    }
    g_ncur = 0;
}
