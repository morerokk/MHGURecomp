// The mouse in the game's list and grid menus: the cell under the pointer is selected, a click picks it, the wheel
// turns the page.
//
// The game's menus have no pointer input (docs/GAME-ACTIONS.md, P1), but they move their selection with two cursor
// classes, both updated with the menu word's pad bits (Up 0x10, Down 0x40, Left 0x80, Right 0x20):
// - a list cursor (docs/research/title-menu.md): update 0x74ee98(cursor, pad bits), nonzero when it moved; +0xd the row
//   (u8, on the page: the real one, unlike the pause menu's own copy at +0xe4, which writing does not move), +0xe the
//   rows a page has, +0x10 / +0x12 page / pages (u16), +0x14 the items in all; Left / Right turn its page. The
//   recompiled code calls it from 408 places (a hunt's pause menu: 0x86f8ec, 13 items, 7 a page, greyed-out rows not
//   skipped). A screen in sections runs a second one of one row a page whose page is the section (a tab bar).
// - a grid cursor (vtable 0x196cb9c), updated through its vtable by 0x7530b8, which runs 0x7530d4 when +0x1e is 1 and
//   0x7536e8 otherwise (both move the same fields the same way): +0x14 the column, +0x15 the row, +0x16 / +0x17 columns
//   / rows a page, +0x18 / +0x1a page / pages, +0x1c items in all; Up / Down move the row and Left / Right the column
//   (wrapping), and the masks 0x85f8c4 (previous page) / 0x85f914 (next page) return (the cursor's paging buttons) turn
//   its page. Item Sets (one column), the Multiplayer menu (0x7536e8; rows under group headers), the item box (10 x 10,
//   23 pages: 0x9169dc).
// A menu is told apart by its call's return address. A cell is row * columns + column on the page.
//
// For another cell the cursor is put beside the target (its column set, its row one off; or in the target's row, its
// column one off) and the update gets one step bit, so the game makes a single ordinary step onto it (its highlight,
// sound and help line follow): a jump in one update.
//
// Where a menu's cells are: geometries by call (built in, and learned by runtime/menulearn.c from the HUD: the highlight
// bar that moves when the row or column changes; kept in menus_learned.txt beside the settings). A geometry carries its
// bar's signature (layer and size), so the renderer finds the bar every frame and the cells go where it is (a popup that
// opens beside the row it came from); a call shared by several screens has a geometry for each, told apart the same
// way. The host side (host_input.c) frees the mouse while such a menu is open, maps the pointer to a cell and presses
// Confirm once a clicked cell is selected.
//
// MHGU_MENUPTR_PROBE=1 logs every cursor that changes, with its call.
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../config.h"
#include "../hle.h"
#include "menuptr.h"
#include "menuptr_table.h"

#define FN_CURSOR_UPDATE 0x74ee98u  // (cursor, pad bits) -> nonzero when it moved
#define FN_GRID_UPDATE   0x7530d4u
#define FN_GRID_UPDATE2  0x7536e8u
#define FN_GRID_PREV     0x85f8c4u  // (grid cursor) the pad bits that turn to the previous page
#define FN_GRID_NEXT     0x85f914u  // ... the next page
#define CALL_PAUSE_MENU  0x86f8ecu  // a hunt's pause menu's call of the list update (return address)
#define BIT_UP    0x10u
#define BIT_DOWN  0x40u
#define BIT_LEFT  0x80u
#define BIT_RIGHT 0x20u
#define OPEN_NS   150000000ull  // a cursor counts as open this long after it last ran
#define LIVE_NS   200000000ull  // a bar the renderer found counts this long
#define MAX_VARIANTS 4          // geometries one call may have (screens that share it)

// ---- geometries
#define MAX_GEOMS 512
static MenuGeom g_geoms[MAX_GEOMS];
static int g_ngeoms;
static pthread_mutex_t g_geom_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_learned_path[1200];

// Built in, measured (the pause menu also has its page arrows, at 1280x720)
static const MenuGeom builtin[] = {
    {CALL_PAUSE_MENU, 970, 101, 282, 34, 0, 36, 97, 141, 17, 0, 0, true},
};

static bool g_no_builtin;  // (MHGU_MENUPTR_NO_BUILTIN=1, tests: the learner finds them again)
static bool is_builtin(uint32_t lr) {
    for (unsigned i = 0; i < sizeof builtin / sizeof *builtin && !g_no_builtin; i++)
        if (builtin[i].lr == lr) return true;
    return false;
}

static bool nearf(float a, float b) { return fabsf(a - b) < 0.5f; }
// The same bar (its layer and sizes): the same menu, wherever it is now (a popup). Not its steps: a list with rows not
// one step apart (the Multiplayer menu's headers) shows other steps when the cursor jumps across a gap.
static bool same_kind(const MenuGeom *a, const MenuGeom *b) {
    if (!a->kw != !b->kw) return false;
    if (!a->kw) return true;  // (no signature: one geometry a call, the last one wins)
    return nearf(a->z, b->z) && fabsf(a->kw - b->kw) < 0.01f && fabsf(a->kh - b->kh) < 0.01f && nearf(a->w, b->w) &&
           nearf(a->h, b->h);
}

// (the lock held) a geometry like g for its call, else NULL
static MenuGeom *geom_kind(const MenuGeom *g) {
    for (int i = 0; i < g_ngeoms; i++)
        if (g_geoms[i].lr == g->lr && same_kind(&g_geoms[i], g)) return &g_geoms[i];
    return NULL;
}
// (the lock held) a geometry of g's call with g's bar (layer and size), else NULL
static MenuGeom *geom_kind_sig(const MenuGeom *g) {
    for (int i = 0; i < g_ngeoms; i++) {
        const MenuGeom *o = &g_geoms[i];
        if (o->lr == g->lr && o->kw && nearf(o->z, g->z) && fabsf(o->kw - g->kw) < 0.01f && fabsf(o->kh - g->kh) < 0.01f)
            return &g_geoms[i];
    }
    return NULL;
}
// (the lock held) keep g: a known bar seen again gets only its new place and the steps it didn't have yet (its own
// steps and rows' places stay); else added (a call's oldest goes when it has too many). Returns the geometry kept when
// it is new or got a step, else NULL.
static MenuGeom *geom_put(const MenuGeom *g) {
    MenuGeom *o = geom_kind(g);
    if (o) {
        if (!o->kw) { *o = *g; return NULL; }
        bool more = false;
        if (!o->dx && !o->dy && (g->dx || g->dy)) o->dx = g->dx, o->dy = g->dy, more = true;
        if (!o->cdx && !o->cdy && (g->cdx || g->cdy)) o->cdx = g->cdx, o->cdy = g->cdy, more = true;
        o->x = g->x, o->y = g->y;
        return more ? o : NULL;
    }
    int n = 0, first = -1;
    for (int i = 0; i < g_ngeoms; i++)
        if (g_geoms[i].lr == g->lr) n++, first = first < 0 ? i : first;
    if (n >= MAX_VARIANTS) { g_geoms[first] = *g; return &g_geoms[first]; }
    if (g_ngeoms >= MAX_GEOMS) return NULL;
    g_geoms[g_ngeoms] = *g;
    return &g_geoms[g_ngeoms++];
}

int menuptr_variants(uint32_t lr, MenuGeom *out, int max) {
    int n = 0;
    pthread_mutex_lock(&g_geom_lock);
    for (int i = 0; i < g_ngeoms && n < max; i++)
        if (g_geoms[i].lr == lr) out[n++] = g_geoms[i];
    pthread_mutex_unlock(&g_geom_lock);
    return n;
}
static bool known(uint32_t lr) {
    MenuGeom g;
    return menuptr_variants(lr, &g, 1) > 0;
}

// (render thread, runtime/menulearn.c) a step seen twice: kept, and written to menus_learned.txt when it is new
void menuptr_learned(const MenuGeom *g) {
    if (is_builtin(g->lr)) return;
    MenuGeom kept;
    pthread_mutex_lock(&g_geom_lock);
    MenuGeom *o = geom_put(g);
    if (o) kept = *o;
    pthread_mutex_unlock(&g_geom_lock);
    if (!o) return;  // (a bar known already, maybe somewhere else: the renderer follows it)
    LOG("[menuptr] learned menu %08x: first cell %.0f,%.0f %.0fx%.0f, next row %+.0f,%+.0f, next column %+.0f,%+.0f (bar: layer "
        "%.0f, %.1f x %.1f)\n", kept.lr, kept.x, kept.y, kept.w, kept.h, kept.dx, kept.dy, kept.cdx, kept.cdy, kept.z, kept.kw,
        kept.kh);
    FILE *f = g_learned_path[0] ? fopen(g_learned_path, "a") : NULL;
    if (f) {
        fprintf(f, "%08x %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.2f %.2f %.1f %.1f %.1f %.1f\n", kept.lr, kept.x, kept.y, kept.w,
                kept.h, kept.dx, kept.dy, kept.z, kept.kw, kept.kh, kept.ox, kept.oy, kept.cdx, kept.cdy);
        fclose(f);
    }
}

// One line of menus_learned.txt (or of the shipped table); false: not a geometry or a row's place
static bool geoms_line(const char *line) {
    MenuGeom g = {0};
    int row;
    if (sscanf(line, "%x off %d %f %f %f %f %f", &g.lr, &row, &g.ox, &g.oy, &g.z, &g.kw, &g.kh) == 7) {
        MenuGeom *o = geom_kind_sig(&g);  // (a row of a geometry above: its bar's signature tells which)
        if (o && row >= 0 && row < MENUPTR_ROWS) {
            if (!(o->offmask & 1)) o->offmask |= 1, o->offx[0] = o->offy[0] = 0;
            o->offmask |= 1u << row, o->offx[row] = g.ox, o->offy[row] = g.oy;
        }
        return o != NULL;
    }
    // lr x y w h dx dy [z kw kh ox oy [cdx cdy]]: older lines have no bar signature (a fixed place) or no columns
    int k = sscanf(line, "%x %f %f %f %f %f %f %f %f %f %f %f %f %f", &g.lr, &g.x, &g.y, &g.w, &g.h, &g.dx, &g.dy, &g.z,
                   &g.kw, &g.kh, &g.ox, &g.oy, &g.cdx, &g.cdy);
    if (k != 7 && k != 12 && k != 14) return false;
    if (k == 7) g.z = g.kw = g.kh = g.ox = g.oy = 0;
    if (k < 14) g.cdx = g.cdy = 0;
    if (is_builtin(g.lr)) return false;
    geom_put(&g);
    return true;
}

static void geoms_load(void) {
    g_no_builtin = getenv("MHGU_MENUPTR_NO_BUILTIN") != NULL;
    for (unsigned i = 0; i < sizeof builtin / sizeof *builtin && !g_no_builtin; i++) geom_put(&builtin[i]);
    // the menus the recomp ships (learned in playthroughs: menuptr_table.h), then the player's own, which add to them
    int shipped = 0;
    if (!g_no_builtin && !getenv("MHGU_MENUPTR_NO_SHIPPED"))
        for (const char *s = menuptr_shipped; *s;) {
            const char *nl = strchr(s, '\n');
            char line[256];
            snprintf(line, sizeof line, "%.*s", (int)(nl ? nl - s : (ptrdiff_t)strlen(s)), s);
            shipped += geoms_line(line);
            s = nl ? nl + 1 : s + strlen(s);
        }
    char ini[1100];
    config_path(ini, sizeof ini);
    char *slash = strrchr(ini, '/'), *bslash = strrchr(ini, '\\');
    if (bslash > slash) slash = bslash;
    snprintf(g_learned_path, sizeof g_learned_path, "%.*smenus_learned.txt", slash ? (int)(slash - ini + 1) : 0, ini);
    FILE *f = fopen(g_learned_path, "r");
    char line[256];
    int n = 0;
    while (f && fgets(line, sizeof line, f)) n += geoms_line(line);  // (later lines win: a menu learned again)
    if (f) fclose(f);
    LOG("[menuptr] %d shipped menu line(s), %d learned from %s\n", shipped, n, f ? g_learned_path : "(none yet)");
}

static void write_offset(const MenuGeom *g, int row) {
    FILE *f = g_learned_path[0] ? fopen(g_learned_path, "a") : NULL;
    if (!f) return;
    fprintf(f, "%08x off %d %.1f %.1f %.1f %.2f %.2f\n", g->lr, row, g->offx[row], g->offy[row], g->z, g->kw, g->kh);
    fclose(f);
}

// (the lock held) a row's place, from the first row's bar: kept in the geometry like g (row 0 is the origin)
static MenuGeom *set_offset(const MenuGeom *g, int row, float x, float y) {
    MenuGeom *o = geom_kind(g);
    if (!o || row < 0 || row >= MENUPTR_ROWS) return NULL;
    if (!(o->offmask & 1)) o->offmask |= 1, o->offx[0] = o->offy[0] = 0;
    o->offmask |= 1u << row, o->offx[row] = x, o->offy[row] = y;
    return o;
}

// The rows of the menu the pointer works on, where the renderer saw its bar since the menu opened: the first row's place
// is the origin; a row seen elsewhere than the geometry says (by more than a pixel) is kept and written down
static struct { uint32_t lr; uint64_t opened; bool have0; float x0, y0; } g_seen;
static uint64_t pointer_opened(uint32_t lr);
void menuptr_row_seen(const MenuGeom *g, int row, float x, float y) {
    uint64_t opened = pointer_opened(g->lr);
    if (g_seen.lr != g->lr || g_seen.opened != opened) g_seen.lr = g->lr, g_seen.opened = opened, g_seen.have0 = false;
    if (row == 0) g_seen.have0 = true, g_seen.x0 = x, g_seen.y0 = y;
    if (!g_seen.have0 || row <= 0 || row >= MENUPTR_ROWS || is_builtin(g->lr)) return;
    float ox, oy;
    menuptr_row_offset(g, row, &ox, &oy);
    float rx = x - g_seen.x0, ry = y - g_seen.y0;
    if (fabsf(rx - ox) <= 1.0f && fabsf(ry - oy) <= 1.0f) return;  // (where the geometry says)
    pthread_mutex_lock(&g_geom_lock);
    MenuGeom *o = set_offset(g, row, rx, ry), copy;
    if (o) copy = *o;
    pthread_mutex_unlock(&g_geom_lock);
    if (!o) return;
    LOG("[menuptr] menu %08x: row %d is at %+.0f,%+.0f from the first (not %+.0f,%+.0f)\n", g->lr, row, rx, ry, ox, oy);
    write_offset(&copy, row);
}

// ---- where the renderer found the bar of the menu in focus this frame
static pthread_mutex_t g_live_lock = PTHREAD_MUTEX_INITIALIZER;
static MenuGeom g_live;
static uint64_t g_live_ns;
void menuptr_live(const MenuGeom *g) {
    pthread_mutex_lock(&g_live_lock);
    if (g) g_live = *g, g_live_ns = plat_now_ns();
    else g_live_ns = 0;
    pthread_mutex_unlock(&g_live_lock);
}

// ---- the cursors
typedef struct { uint32_t fn, row, rows, page, pages, total, col, cols; } Layout;  // (col, cols 0: none)
static const Layout LIST = {FN_CURSOR_UPDATE, 0xd, 0xe, 0x10, 0x12, 0x14, 0, 0};
static const Layout GRID = {FN_GRID_UPDATE, 0x15, 0x17, 0x18, 0x1a, 0x1c, 0x14, 0x16};
static const Layout GRID2 = {FN_GRID_UPDATE2, 0x15, 0x17, 0x18, 0x1a, 0x1c, 0x14, 0x16};

#define MAX_CURSORS 16
typedef struct {
    uint32_t lr;
    uint64_t seen_ns, appeared_ns, moved_ns, since;  // since: presented frame its cell / page last changed (or it appeared)
    int row, col, cells, cols, page, pages;          // cells: on its page now
    bool known;                                      // its cells' places are known
} Cursor;
static Cursor g_cursors[MAX_CURSORS];
static pthread_mutex_t g_cur_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_uint g_target_lr;    // the menu a target is for
static atomic_int g_target = -1;   // the cell asked for (-1 none)
static atomic_bool g_click;        // ... and picked: Confirm once it is selected
static atomic_bool g_confirm;      // a picked cell is selected now: the host presses Confirm
static atomic_uint g_page_lr;      // a page to turn: the menu, and which way (> 0 next)
static atomic_int g_page_dir;
static int g_probe = -1;

// (g_cur_lock held) the open cursor that last moved or appeared, of those whose cells are known (for the pointer) or of
// those with more than one cell (for learning: a section bar's one row never moves)
static Cursor *pick(bool for_pointer) {
    uint64_t now = plat_now_ns(), best_t = 0;
    Cursor *best = NULL;
    for (int i = 0; i < MAX_CURSORS; i++) {
        Cursor *c = &g_cursors[i];
        if (!c->lr || now - c->seen_ns >= OPEN_NS) continue;
        if (for_pointer ? !c->known : c->cells < 2) continue;
        uint64_t t = c->moved_ns > c->appeared_ns ? c->moved_ns : c->appeared_ns;
        if (!best || t >= best_t) best = c, best_t = t;
    }
    return best;
}

static int cols_of(uint32_t cur, const Layout *L) {
    int n = L->cols ? LD8(cur + L->cols) : 1;
    return n < 1 ? 1 : n;
}
static int cells_now(uint32_t cur, const Layout *L) {  // cells on the cursor's page
    int per = LD8(cur + L->rows) * cols_of(cur, L), left = LD16(cur + L->total) - LD16(cur + L->page) * per;
    return left < 0 ? 0 : left < per ? left : per;
}

static uint32_t call1(Cpu *c, uint32_t fn, uint32_t r0) {
    uint32_t saved[16];
    memcpy(saved, c->r, sizeof saved);
    c->r[0] = r0;
    guest_call(c, fn);
    uint32_t v = c->r[0];
    memcpy(c->r, saved, sizeof saved);
    return v;
}

static void probe(Cpu *c, uint32_t cur, uint32_t bits, uint32_t lr, int row0, int col0, const Layout *L) {
    static struct { uint32_t cur, lr; int row, col, cnt, page; } last[32];  // (per call: menus run several at once)
    static int next;
    int row = LD8(cur + L->row), col = L->col ? LD8(cur + L->col) : 0, cnt = LD8(cur + L->rows), page = LD16(cur + L->page);
    int k = -1;
    for (int i = 0; i < 32 && k < 0; i++)
        if (last[i].lr == lr) k = i;
    if (k >= 0 && last[k].cur == cur && last[k].row == row && last[k].col == col && last[k].cnt == cnt && last[k].page == page)
        return;
    if (k < 0) k = next++ % 32, last[k].lr = lr;
    LOG("[menuptr] %s %08x from %08x row %d -> %d of %d, column %d -> %d of %d, page %d / %u, %u items, bits %08x%s\n",
        L->col ? "grid" : "cursor", cur, lr, row0, row, cnt, col0, col, cols_of(cur, L), page, LD16(cur + L->pages),
        LD16(cur + L->total), bits, known(lr) ? " (known)" : "");
    last[k].cur = cur, last[k].row = row, last[k].col = col, last[k].cnt = cnt, last[k].page = page;
}

static void cursor_update_as(Cpu *c, const Layout *L) {
    uint32_t cur = c->r[0], lr = c->r[14] & ~1u;
    int row0 = LD8(cur + L->row), col0 = L->col ? LD8(cur + L->col) : 0, cols = cols_of(cur, L), cells = cells_now(cur, L);
    if (atomic_load(&g_page_lr) == lr && atomic_load(&g_page_dir)) {  // (the wheel: a page)
        int dir = atomic_exchange(&g_page_dir, 0);
        atomic_store(&g_target, -1), atomic_store(&g_click, false);
        if (L->col) c->r[1] |= call1(c, dir > 0 ? FN_GRID_NEXT : FN_GRID_PREV, cur);
        else c->r[1] |= dir > 0 ? BIT_RIGHT : BIT_LEFT;
    } else if (atomic_load(&g_target_lr) == lr) {
        static int steps;  // (updates spent on this target: a cell that can't be reached is given up)
        static int last_target = -1;
        int target = atomic_load(&g_target);
        if (target != last_target) steps = 0, last_target = target;
        if (target >= cells || (target >= 0 && ++steps > 4)) {
            atomic_store(&g_target, -1), atomic_store(&g_click, false);
            target = last_target = -1;
        }
        // (a jump: the cursor is put beside the target and makes one ordinary step onto it)
        int tr = target >= 0 ? target / cols : 0, tc = target >= 0 ? target % cols : 0, rows = (cells + cols - 1) / cols;
        if (target >= 0 && tr != row0 && rows > 1) {
            MenuGeom g;
            bool across = !L->col && menuptr_geometry(&g) && g.dy == 0 && g.dx != 0;  // (a list of items side by side)
            int from = tr > 0 ? tr - 1 : 1;
            if (L->col) ST8(cur + L->col, (uint8_t)tc);
            ST8(cur + L->row, (uint8_t)from);
            c->r[1] |= tr > from ? (across ? BIT_RIGHT : BIT_DOWN) : (across ? BIT_LEFT : BIT_UP);
        } else if (target >= 0 && tc != col0 && cols > 1) {
            int from = tc > 0 ? tc - 1 : 1;
            ST8(cur + L->col, (uint8_t)from);
            c->r[1] |= tc > from ? BIT_RIGHT : BIT_LEFT;
        }
    }
    uint32_t bits = c->r[1];
    fnhook_call_original(c, L->fn);
    if (g_probe > 0) probe(c, cur, bits, lr, row0, col0, L);
    uint64_t now = plat_now_ns();
    int row = LD8(cur + L->row), col = L->col ? LD8(cur + L->col) : 0, page = LD16(cur + L->page);
    bool is_known = known(lr);
    pthread_mutex_lock(&g_cur_lock);
    Cursor *e = NULL, *oldest = &g_cursors[0];
    for (int i = 0; i < MAX_CURSORS && !e; i++) {
        if (g_cursors[i].lr == lr) e = &g_cursors[i];
        else if (g_cursors[i].seen_ns < oldest->seen_ns) oldest = &g_cursors[i];
    }
    bool fresh = !e || now - e->seen_ns >= OPEN_NS;
    if (!e) e = oldest, e->lr = lr;
    if (fresh) e->appeared_ns = now, e->moved_ns = 0, e->since = g_present_count;
    else if (e->row != row || e->col != col || e->page != page) e->moved_ns = now, e->since = g_present_count;
    e->seen_ns = now, e->row = row, e->col = col, e->page = page, e->pages = LD16(cur + L->pages), e->known = is_known;
    e->cols = cols_of(cur, L), e->cells = cells_now(cur, L);
    pthread_mutex_unlock(&g_cur_lock);
    if (atomic_load(&g_target_lr) == lr && atomic_load(&g_target) == row * e->cols + col) {
        atomic_store(&g_target, -1);
        if (atomic_exchange(&g_click, false)) atomic_store(&g_confirm, true);
    }
}
static void cursor_update(Cpu *c) { cursor_update_as(c, &LIST); }
static void grid_update(Cpu *c) { cursor_update_as(c, &GRID); }
static void grid_update2(Cpu *c) { cursor_update_as(c, &GRID2); }

static uint64_t pointer_opened(uint32_t lr) {  // when that call's cursor appeared (0: not open)
    uint64_t t = 0;
    pthread_mutex_lock(&g_cur_lock);
    for (int i = 0; i < MAX_CURSORS; i++)
        if (g_cursors[i].lr == lr) t = g_cursors[i].appeared_ns;
    pthread_mutex_unlock(&g_cur_lock);
    return t;
}

// The menu the pointer works on; false when none is open
static bool pointer_menu(Cursor *out) {
    pthread_mutex_lock(&g_cur_lock);
    Cursor *c = pick(true);
    if (c) *out = *c;
    pthread_mutex_unlock(&g_cur_lock);
    return c != NULL;
}

bool menuptr_focus(uint32_t *lr, int *row, int *col, int *page, uint64_t *since) {
    pthread_mutex_lock(&g_cur_lock);
    Cursor *c = pick(false);
    if (c) *lr = c->lr, *row = c->row, *col = c->col, *page = c->page, *since = c->since;
    pthread_mutex_unlock(&g_cur_lock);
    return c != NULL;
}
bool menuptr_pointer_focus(uint32_t *lr, int *row, int *col, uint64_t *since) {
    Cursor c;
    if (!pointer_menu(&c)) return false;
    *lr = c.lr, *row = c.row, *col = c.col, *since = c.since;
    return true;
}

bool menuptr_open(void) {
    Cursor c;
    return pointer_menu(&c);
}
int menuptr_row(void) {
    Cursor c;
    return pointer_menu(&c) ? c.row * c.cols + c.col : -1;
}
int menuptr_rows(void) {
    Cursor c;
    return pointer_menu(&c) ? c.cells : 0;
}
int menuptr_cols(void) {
    Cursor c;
    return pointer_menu(&c) ? c.cols : 1;
}
int menuptr_pages(void) {
    Cursor c;
    return pointer_menu(&c) ? c.pages : 0;
}

bool menuptr_geometry(MenuGeom *out) {
    Cursor c;
    if (!pointer_menu(&c)) return false;
    pthread_mutex_lock(&g_live_lock);
    bool live = g_live_ns && g_live.lr == c.lr && plat_now_ns() - g_live_ns < LIVE_NS;
    if (live) *out = g_live;
    pthread_mutex_unlock(&g_live_lock);
    if (live) return true;
    MenuGeom v[MAX_VARIANTS];
    int n = menuptr_variants(c.lr, v, MAX_VARIANTS);
    if (n) *out = v[n - 1];  // (the last learned)
    return n > 0;
}

void menuptr_row_offset(const MenuGeom *g, int row, float *x, float *y) {
    int j = -1;  // (the nearest row whose place is known; ties: the one above)
    for (int i = 0; i < MENUPTR_ROWS; i++)
        if ((g->offmask >> i & 1) && (j < 0 || abs(i - row) < abs(j - row))) j = i;
    if (j < 0) return (void)(*x = g->dx * (float)row, *y = g->dy * (float)row);
    *x = g->offx[j] + g->dx * (float)(row - j), *y = g->offy[j] + g->dy * (float)(row - j);
}

void menuptr_row_rect(const MenuGeom *g, int cell, int cols, float *x0, float *y0, float *x1, float *y1) {
    // (the box a cell has: the highlight bar's size, grown to the steps between rows and columns)
    int row = cols > 0 ? cell / cols : cell, col = cols > 0 ? cell % cols : 0;
    float ox, oy;
    menuptr_row_offset(g, row, &ox, &oy);
    float x = g->x + ox + g->cdx * (float)col, y = g->y + oy + g->cdy * (float)col, w = g->w, h = g->h;
    float sw = fmaxf(fabsf(g->dx), fabsf(g->cdx)), sh = fmaxf(fabsf(g->dy), fabsf(g->cdy));
    if (w <= 0 && h <= 0) {  // (a frame of no known size, its corner: the cell is one step each way from it)
        *x0 = x, *y0 = y, *x1 = x + (sw > 0 ? sw : sh), *y1 = y + (sh > 0 ? sh : sw);
        return;
    }
    if (sh > h) y -= (sh - h) / 2, h = sh;
    if (sw > w) x -= (sw - w) / 2, w = sw;
    *x0 = x, *y0 = y, *x1 = x + w, *y1 = y + h;
}

int menuptr_hit(float x, float y) {
    Cursor c;
    MenuGeom g;
    if (!pointer_menu(&c) || !menuptr_geometry(&g)) return MENUPTR_NONE;
    if (c.lr == CALL_PAUSE_MENU && y >= 64.0f && y < 97.0f && c.pages > 1) {  // (its page arrows)
        if (x >= 1108.0f && x < 1146.0f) return MENUPTR_PAGE_LEFT;
        if (x >= 1214.0f && x < 1252.0f) return MENUPTR_PAGE_RIGHT;
    }
    int cols = c.cols > 1 && !g.cdx && !g.cdy ? 1 : c.cols;  // (columns whose step isn't known yet: only the first)
    for (int i = 0; i < c.cells; i++) {
        if (cols != c.cols && i % c.cols) continue;
        float x0, y0, x1, y1;
        menuptr_row_rect(&g, cols == c.cols ? i : i / c.cols, cols, &x0, &y0, &x1, &y1);
        if (x >= x0 && x < x1 && y >= y0 && y < y1) return i;
    }
    return MENUPTR_NONE;
}

void menuptr_row_centre(int cell, float *x, float *y) {
    MenuGeom g;
    if (!menuptr_geometry(&g)) return (void)(*x = 640, *y = 360);
    float x0, y0, x1, y1;
    menuptr_row_rect(&g, cell, menuptr_cols(), &x0, &y0, &x1, &y1);
    *x = (x0 + x1) / 2, *y = (y0 + y1) / 2;
}

void menuptr_select(int cell, bool pick_it) {
    Cursor c;
    if (cell < 0 || !pointer_menu(&c)) return;
    if (pick_it && cell == c.row * c.cols + c.col) {
        atomic_store(&g_target, -1), atomic_store(&g_click, false), atomic_store(&g_confirm, true);
        return;
    }
    atomic_store(&g_target, -1);
    atomic_store(&g_target_lr, c.lr);
    atomic_store(&g_click, pick_it);
    atomic_store(&g_target, cell);
}

void menuptr_page_turn(int dir) {
    Cursor c;
    if (!dir || !pointer_menu(&c) || c.pages < 2) return;
    atomic_store(&g_page_lr, c.lr);
    atomic_store(&g_page_dir, dir > 0 ? 1 : -1);
}

bool menuptr_take_confirm(void) { return atomic_exchange(&g_confirm, false); }

void menuptr_init(void) {
    g_probe = getenv("MHGU_MENUPTR_PROBE") != NULL;
    geoms_load();
    fnhook_set(FN_CURSOR_UPDATE, hle_register("menuptr:cursor_update", cursor_update));
    fnhook_set(FN_GRID_UPDATE, hle_register("menuptr:grid_update", grid_update));
    fnhook_set(FN_GRID_UPDATE2, hle_register("menuptr:grid_update2", grid_update2));
}
