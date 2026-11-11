// The mouse in the game's list menus: the row under the pointer is selected, a click picks it.
//
// The game's menus have no pointer input (docs/GAME-ACTIONS.md, P1), but most list menus move a shared cursor object
// (docs/research/title-menu.md): update 0x74ee98(cursor, pad bits) moves it by the menu word's Up (0x10) / Down (0x40)
// bits and returns nonzero when it moved. The cursor keeps +0xd the row (u8, on the page: the real one, unlike the pause
// menu's own copy at +0xe4, which writing does not move), +0xe the rows a page has, +0x10 the page (u16), +0x12 the
// pages, +0x14 the items in all. The recompiled code calls it from 408 places; a menu is told apart by its call's
// return address (a hunt's pause menu: 0x86f8ec, 13 items, 7 a page, greyed-out rows not skipped). A screen in
// sections runs two at once: its list, and a cursor of one row a page whose page is the section (a tab bar).
//
// For another row the cursor's row is set to the one beside the target and the update gets one step bit, so the game
// makes a single ordinary step onto it (its highlight, sound and help line follow): a jump in one update.
//
// Where a menu's rows are: geometries by call (built in, and learned by runtime/menulearn.c from the HUD: the highlight
// bar that moves when the row changes; kept in menus_learned.txt beside the settings). A geometry carries its bar's
// signature (layer and size), so the renderer finds the bar every frame and the rows go where it is (a popup that opens
// beside the row it came from); a call shared by several screens has a geometry for each, told apart the same way.
// The host side (host_input.c) frees the mouse while such a menu is open, maps the pointer to a row and presses Confirm
// once a clicked row is selected.
//
// MHGU_MENUPTR_PROBE=1 logs every cursor of the class that changes, with its call.
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../config.h"
#include "../hle.h"
#include "menuptr.h"

#define FN_CURSOR_UPDATE 0x74ee98u  // (cursor, pad bits) -> nonzero when it moved
#define CALL_PAUSE_MENU  0x86f8ecu  // a hunt's pause menu's call of it (return address)
#define CUR_ROW   0xdu   // u8: the row, on the page
#define CUR_ROWS  0xeu   // u8: rows a page has
#define CUR_PAGE  0x10u  // u16
#define CUR_PAGES 0x12u  // u16
#define CUR_TOTAL 0x14u  // u16: items in all
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
// The same bar and steps (the place may differ: a popup)
static bool same_kind(const MenuGeom *a, const MenuGeom *b) {
    if (!a->kw != !b->kw) return false;
    if (!a->kw) return true;  // (no signature: one geometry a call, the last one wins)
    return nearf(a->z, b->z) && fabsf(a->kw - b->kw) < 0.01f && fabsf(a->kh - b->kh) < 0.01f && nearf(a->dx, b->dx) &&
           nearf(a->dy, b->dy) && nearf(a->w, b->w) && nearf(a->h, b->h);
}

// (the lock held) a geometry like g for its call, else NULL
static MenuGeom *geom_kind(const MenuGeom *g) {
    for (int i = 0; i < g_ngeoms; i++)
        if (g_geoms[i].lr == g->lr && same_kind(&g_geoms[i], g)) return &g_geoms[i];
    return NULL;
}
// (the lock held) keep g: in place of one like it, else added (a call's oldest goes when it has too many)
static bool geom_put(const MenuGeom *g) {
    MenuGeom *o = geom_kind(g);
    if (o) { *o = *g; return false; }
    int n = 0, first = -1;
    for (int i = 0; i < g_ngeoms; i++)
        if (g_geoms[i].lr == g->lr) n++, first = first < 0 ? i : first;
    if (n >= MAX_VARIANTS) { g_geoms[first] = *g; return true; }
    if (g_ngeoms < MAX_GEOMS) g_geoms[g_ngeoms++] = *g;
    return true;
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

// (render thread, runtime/menulearn.c) a geometry seen twice: kept, and written to menus_learned.txt when it is new
void menuptr_learned(const MenuGeom *g) {
    if (is_builtin(g->lr)) return;
    pthread_mutex_lock(&g_geom_lock);
    bool added = geom_put(g);
    pthread_mutex_unlock(&g_geom_lock);
    if (!added) return;  // (a bar known already, maybe somewhere else: the renderer follows it)
    LOG("[menuptr] learned menu %08x: first row %.0f,%.0f %.0fx%.0f, next row %+.0f,%+.0f (bar: layer %.0f, %.1f x %.1f)\n",
        g->lr, g->x, g->y, g->w, g->h, g->dx, g->dy, g->z, g->kw, g->kh);
    FILE *f = g_learned_path[0] ? fopen(g_learned_path, "a") : NULL;
    if (f) {
        fprintf(f, "%08x %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.2f %.2f %.1f %.1f\n", g->lr, g->x, g->y, g->w, g->h, g->dx, g->dy,
                g->z, g->kw, g->kh, g->ox, g->oy);
        fclose(f);
    }
}

static void geoms_load(void) {
    g_no_builtin = getenv("MHGU_MENUPTR_NO_BUILTIN") != NULL;
    for (unsigned i = 0; i < sizeof builtin / sizeof *builtin && !g_no_builtin; i++) geom_put(&builtin[i]);
    char ini[1100];
    config_path(ini, sizeof ini);
    char *slash = strrchr(ini, '/'), *bslash = strrchr(ini, '\\');
    if (bslash > slash) slash = bslash;
    snprintf(g_learned_path, sizeof g_learned_path, "%.*smenus_learned.txt", slash ? (int)(slash - ini + 1) : 0, ini);
    FILE *f = fopen(g_learned_path, "r");
    if (!f) return;
    char line[256];
    int n = 0;
    while (fgets(line, sizeof line, f)) {  // (later lines win: a menu learned again)
        MenuGeom g = {0};
        int k = sscanf(line, "%x %f %f %f %f %f %f %f %f %f %f %f", &g.lr, &g.x, &g.y, &g.w, &g.h, &g.dx, &g.dy, &g.z, &g.kw,
                       &g.kh, &g.ox, &g.oy);
        if (k != 7 && k != 12) continue;
        if (k == 7) g.z = g.kw = g.kh = g.ox = g.oy = 0;  // (an older line: no bar signature, a fixed place)
        if (!is_builtin(g.lr)) geom_put(&g), n++;
    }
    fclose(f);
    LOG("[menuptr] %d learned menu line(s) from %s\n", n, g_learned_path);
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

// ---- the cursors seen
#define MAX_CURSORS 16
typedef struct {
    uint32_t lr;
    uint64_t seen_ns, appeared_ns, moved_ns, since;  // since: presented frame its row / page last changed (or it appeared)
    int row, rows, page, pages;                      // rows: on its page now
    bool known;                                      // its rows' places are known
} Cursor;
static Cursor g_cursors[MAX_CURSORS];
static pthread_mutex_t g_cur_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_uint g_target_lr;    // the menu a target is for
static atomic_int g_target = -1;   // the row asked for (-1 none)
static atomic_bool g_click;        // ... and picked: Confirm once it is selected
static atomic_bool g_confirm;      // a picked row is selected now: the host presses Confirm
static int g_probe = -1;

// (g_cur_lock held) the open cursor that last moved or appeared, of those whose rows are known (for the pointer) or of
// those with more than one row (for learning: a section bar's one row never moves)
static Cursor *pick(bool for_pointer) {
    uint64_t now = plat_now_ns(), best_t = 0;
    Cursor *best = NULL;
    for (int i = 0; i < MAX_CURSORS; i++) {
        Cursor *c = &g_cursors[i];
        if (!c->lr || now - c->seen_ns >= OPEN_NS) continue;
        if (for_pointer ? !c->known : c->rows < 2) continue;
        uint64_t t = c->moved_ns > c->appeared_ns ? c->moved_ns : c->appeared_ns;
        if (!best || t >= best_t) best = c, best_t = t;
    }
    return best;
}

static int rows_on_page(int rows, int page, int total) {
    int left = total - page * rows;
    return left < 0 ? 0 : left < rows ? left : rows;
}

// The two cursor classes: the list cursor above, and a grid cursor (update 0x7530d4(cursor, pad bits), reached through
// its vtable via 0x7530b8, which runs it when +0x1e is 1): +0x14 the column, +0x15 the row, +0x16 / +0x17 columns /
// rows a page, +0x18 / +0x1a page / pages, +0x1c items in all; Up / Down move the row (wrapping), Left / Right the
// column. Item Sets' list is one (one column; the read-only list beside it is a list cursor that doesn't move). Grids of
// more than one column are not done: their cursors are seen, but count as no rows.
#define FN_GRID_UPDATE 0x7530d4u
typedef struct { uint32_t fn, row, rows, page, pages, total, cols; } Layout;
static const Layout LIST = {FN_CURSOR_UPDATE, CUR_ROW, CUR_ROWS, CUR_PAGE, CUR_PAGES, CUR_TOTAL, 0};
static const Layout GRID = {FN_GRID_UPDATE, 0x15, 0x17, 0x18, 0x1a, 0x1c, 0x16};
static int rows_now(uint32_t cur, const Layout *L) {  // rows on the cursor's page (0: a grid of several columns)
    if (L->cols && LD8(cur + L->cols) != 1) return 0;
    return rows_on_page(LD8(cur + L->rows), LD16(cur + L->page), LD16(cur + L->total));
}

static void probe(Cpu *c, uint32_t cur, uint32_t bits, uint32_t lr, uint8_t idx0, const Layout *L) {
    static struct { uint32_t cur, lr; uint8_t idx, cnt; uint16_t page; } last[32];  // (per call: menus run several at once)
    static int next;
    uint8_t idx = LD8(cur + L->row), cnt = LD8(cur + L->rows);
    uint16_t page = LD16(cur + L->page);
    int k = -1;
    for (int i = 0; i < 32 && k < 0; i++)
        if (last[i].lr == lr) k = i;
    if (k >= 0 && last[k].cur == cur && last[k].idx == idx && last[k].cnt == cnt && last[k].page == page) return;
    if (k < 0) k = next++ % 32, last[k].lr = lr;
    LOG("[menuptr] %s %08x from %08x row %u -> %u of %u%s, page %u / %u, %u items, bits %08x%s\n", L->cols ? "grid" : "cursor",
        cur, lr, idx0, idx, cnt, L->cols && LD8(cur + L->cols) != 1 ? " (columns)" : "", page, LD16(cur + L->pages),
        LD16(cur + L->total), bits, known(lr) ? " (known)" : "");
    last[k].cur = cur, last[k].idx = idx, last[k].cnt = cnt, last[k].page = page;
}

static void cursor_update_as(Cpu *c, const Layout *L) {
    uint32_t cur = c->r[0], lr = c->r[14] & ~1u;
    uint8_t row0 = LD8(cur + L->row);
    int on_page = rows_now(cur, L);
    if (atomic_load(&g_target_lr) == lr) {
        static int steps;  // (updates spent on this target: a row that can't be reached is given up)
        static int last_target = -1;
        int target = atomic_load(&g_target);
        if (target != last_target) steps = 0, last_target = target;
        if (target >= on_page || (target >= 0 && ++steps > 4)) {
            atomic_store(&g_target, -1), atomic_store(&g_click, false);
            target = last_target = -1;
        }
        // (a jump: the cursor is put on the row beside the target and makes one ordinary step onto it)
        if (target >= 0 && target != row0 && on_page > 1) {
            MenuGeom g;
            bool across = menuptr_geometry(&g) && g.dy == 0 && g.dx != 0;  // (a row of items side by side)
            int from = target > 0 ? target - 1 : 1;
            ST8(cur + L->row, (uint8_t)from);
            across &= !L->cols;  // (a grid's one column steps by row)
            c->r[1] |= target > from ? (across ? BIT_RIGHT : BIT_DOWN) : (across ? BIT_LEFT : BIT_UP);
        }
    }
    uint32_t bits = c->r[1];
    fnhook_call_original(c, L->fn);
    if (g_probe > 0) probe(c, cur, bits, lr, row0, L);
    uint64_t now = plat_now_ns();
    int row = LD8(cur + L->row), page = LD16(cur + L->page);
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
    else if (e->row != row || e->page != page) e->moved_ns = now, e->since = g_present_count;
    e->seen_ns = now, e->row = row, e->page = page, e->pages = LD16(cur + L->pages), e->known = is_known;
    e->rows = rows_now(cur, L);
    pthread_mutex_unlock(&g_cur_lock);
    if (atomic_load(&g_target_lr) == lr && atomic_load(&g_target) == row) {
        atomic_store(&g_target, -1);
        if (atomic_exchange(&g_click, false)) atomic_store(&g_confirm, true);
    }
}
static void cursor_update(Cpu *c) { cursor_update_as(c, &LIST); }
static void grid_update(Cpu *c) { cursor_update_as(c, &GRID); }

// The menu the pointer works on: lr, row, rows on its page, pages; false when none is open
static bool pointer_menu(uint32_t *lr, int *row, int *rows, int *pages) {
    pthread_mutex_lock(&g_cur_lock);
    Cursor *c = pick(true);
    if (c) *lr = c->lr, *row = c->row, *rows = c->rows, *pages = c->pages;
    pthread_mutex_unlock(&g_cur_lock);
    return c != NULL;
}

bool menuptr_focus(uint32_t *lr, int *row, int *page, uint64_t *since) {
    pthread_mutex_lock(&g_cur_lock);
    Cursor *c = pick(false);
    if (c) *lr = c->lr, *row = c->row, *page = c->page, *since = c->since;
    pthread_mutex_unlock(&g_cur_lock);
    return c != NULL;
}
bool menuptr_pointer_focus(uint32_t *lr, int *row, uint64_t *since) {
    pthread_mutex_lock(&g_cur_lock);
    Cursor *c = pick(true);
    if (c) *lr = c->lr, *row = c->row, *since = c->since;
    pthread_mutex_unlock(&g_cur_lock);
    return c != NULL;
}

bool menuptr_open(void) {
    uint32_t lr;
    int row, rows, pages;
    return pointer_menu(&lr, &row, &rows, &pages);
}
int menuptr_row(void) {
    uint32_t lr;
    int row, rows, pages;
    return pointer_menu(&lr, &row, &rows, &pages) ? row : -1;
}
int menuptr_rows(void) {
    uint32_t lr;
    int row, rows, pages;
    return pointer_menu(&lr, &row, &rows, &pages) ? rows : 0;
}

bool menuptr_geometry(MenuGeom *out) {
    uint32_t lr;
    int row, rows, pages;
    if (!pointer_menu(&lr, &row, &rows, &pages)) return false;
    pthread_mutex_lock(&g_live_lock);
    bool live = g_live_ns && g_live.lr == lr && plat_now_ns() - g_live_ns < LIVE_NS;
    if (live) *out = g_live;
    pthread_mutex_unlock(&g_live_lock);
    if (live) return true;
    MenuGeom v[MAX_VARIANTS];
    int n = menuptr_variants(lr, v, MAX_VARIANTS);
    if (n) *out = v[n - 1];  // (the last learned)
    return n > 0;
}

void menuptr_row_rect(const MenuGeom *g, int row, float *x0, float *y0, float *x1, float *y1) {
    // (the band a row has: the highlight bar's size, grown to the step between rows along the list)
    float x = g->x + g->dx * (float)row, y = g->y + g->dy * (float)row, w = g->w, h = g->h;
    if (fabsf(g->dy) > h) y -= (fabsf(g->dy) - h) / 2, h = fabsf(g->dy);
    if (fabsf(g->dx) > w) x -= (fabsf(g->dx) - w) / 2, w = fabsf(g->dx);
    *x0 = x, *y0 = y, *x1 = x + w, *y1 = y + h;
}

int menuptr_hit(float x, float y) {
    uint32_t lr;
    int row, rows, pages;
    MenuGeom g;
    if (!pointer_menu(&lr, &row, &rows, &pages) || !menuptr_geometry(&g)) return MENUPTR_NONE;
    if (lr == CALL_PAUSE_MENU && y >= 64.0f && y < 97.0f && pages > 1) {  // (its page arrows)
        if (x >= 1108.0f && x < 1146.0f) return MENUPTR_PAGE_LEFT;
        if (x >= 1214.0f && x < 1252.0f) return MENUPTR_PAGE_RIGHT;
    }
    for (int i = 0; i < rows; i++) {
        float x0, y0, x1, y1;
        menuptr_row_rect(&g, i, &x0, &y0, &x1, &y1);
        if (x >= x0 && x < x1 && y >= y0 && y < y1) return i;
    }
    return MENUPTR_NONE;
}

void menuptr_row_centre(int row, float *x, float *y) {
    MenuGeom g;
    if (!menuptr_geometry(&g)) return (void)(*x = 640, *y = 360);
    float x0, y0, x1, y1;
    menuptr_row_rect(&g, row, &x0, &y0, &x1, &y1);
    *x = (x0 + x1) / 2, *y = (y0 + y1) / 2;
}

void menuptr_select(int row, bool pick_it) {
    uint32_t lr;
    int cur_row, rows, pages;
    if (row < 0 || !pointer_menu(&lr, &cur_row, &rows, &pages)) return;
    if (pick_it && row == cur_row) {
        atomic_store(&g_target, -1), atomic_store(&g_click, false), atomic_store(&g_confirm, true);
        return;
    }
    atomic_store(&g_target, -1);
    atomic_store(&g_target_lr, lr);
    atomic_store(&g_click, pick_it);
    atomic_store(&g_target, row);
}

bool menuptr_take_confirm(void) { return atomic_exchange(&g_confirm, false); }

void menuptr_init(void) {
    g_probe = getenv("MHGU_MENUPTR_PROBE") != NULL;
    geoms_load();
    fnhook_set(FN_CURSOR_UPDATE, hle_register("menuptr:cursor_update", cursor_update));
    fnhook_set(FN_GRID_UPDATE, hle_register("menuptr:grid_update", grid_update));
}
