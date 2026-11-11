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
#include <string.h>

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

void menulearn_request_log(void) { g_log_request = 1; }

void menulearn_piece(const float *m) {
    if (g_ncur >= MAX_PIECES) return;
    Piece p = {m[12], m[13], m[14], m[0], m[5]};
    g_cur[g_ncur++] = p;
    if (g_logging)
        LOG("[pieces] z %.1f pos %.1f,%.1f m0 %.2f m1 %.2f m4 %.2f m5 %.2f\n", m[14], m[12], m[13], m[0], m[1], m[4], m[5]);
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
    if (best == 0) return false;
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

// Two changes of a menu that agree (the same bar and steps: the place may differ, a popup) make its geometry
static bool same_bar(const MenuGeom *a, const MenuGeom *b) {
    return fabsf(a->z - b->z) < 0.5f && fabsf(a->kw - b->kw) < 0.01f && fabsf(a->kh - b->kh) < 0.01f && fabsf(a->w - b->w) < 0.5f &&
           fabsf(a->h - b->h) < 0.5f && fabsf(a->dx - b->dx) < 0.5f && fabsf(a->dy - b->dy) < 0.5f;
}
static struct { MenuGeom g; int seen; } g_pending[16];
static void observe(const MenuGeom *g) {
    int k = -1, free_k = 0;
    for (int i = 0; i < 16; i++) {
        if (g_pending[i].g.lr == g->lr) { k = i; break; }
        if (!g_pending[i].g.lr) free_k = i;
    }
    if (k < 0) k = free_k, g_pending[k].seen = 0;
    bool agree = g_pending[k].seen && same_bar(&g_pending[k].g, g);
    g_pending[k].g = *g;
    g_pending[k].seen = agree ? g_pending[k].seen + 1 : 1;
    if (g_pending[k].seen >= 2) menuptr_learned(g);  // (again later: a new place of a known bar is not written)
}

// Where the bar of the menu the pointer works on is this frame: a piece of its signature (of any of the call's
// geometries; the one nearest where that geometry was, when there are several), the rows placed from it
static void follow(void) {
    uint32_t lr;
    int row;
    uint64_t since;
    if (!menuptr_pointer_focus(&lr, &row, &since)) return;
    if (g_present_count - since < 3) return;  // (the row just changed: the frame may still show the old one)
    MenuGeom v[4], best;
    int n = menuptr_variants(lr, v, 4);
    float best_d = 1e18f;
    for (int k = 0; k < n; k++) {
        if (!v[k].kw) continue;
        for (int i = 0; i < g_ncur; i++) {
            const Piece *p = &g_cur[i];
            if (!near(p->z, v[k].z) || !near(p->w, v[k].kw) || !near(p->h, v[k].kh)) continue;
            MenuGeom g = v[k];
            g.x = p->x + g.ox - g.dx * (float)row, g.y = p->y + g.oy - g.dy * (float)row;
            float d = (g.x - v[k].x) * (g.x - v[k].x) + (g.y - v[k].y) * (g.y - v[k].y);
            if (d < best_d) best_d = d, best = g;
        }
    }
    menuptr_live(best_d < 1e18f ? &best : NULL);
}

void menulearn_frame(void) {
    if (g_logging) LOG("[pieces] frame done: %d pieces\n", g_ncur), g_logging = 0;
    if (g_log_request) g_log_request = 0, g_logging = 1;
    follow();
    uint32_t lr;
    int row, page;
    uint64_t since;
    if (!menuptr_focus(&lr, &row, &page, &since)) {
        g_prev_row = -1;
    } else if (g_present_count - since >= STABLE_FRAMES) {
        if (g_prev_row >= 0 && lr == g_prev_lr && page == g_prev_page && row != g_prev_row) {
            float sx, sy, box[4];
            Piece key;
            if (find_bar(row - g_prev_row, &sx, &sy, box, &key)) {
                MenuGeom g = {lr, box[0] - sx * (float)row, box[1] - sy * (float)row, box[2] - box[0], box[3] - box[1], sx, sy,
                              key.z, key.w, key.h, box[0] - key.x, box[1] - key.y, false};
                observe(&g);
            }
        }
        memcpy(g_prev, g_cur, sizeof *g_cur * (size_t)g_ncur);  // (the latest frame of a kept row)
        g_nprev = g_ncur, g_prev_lr = lr, g_prev_row = row, g_prev_page = page;
    }
    g_ncur = 0;
}
