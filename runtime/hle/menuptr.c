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

// Built in, measured at 1280x720: the pause menu (it also has its page arrows), and the front end's screens, whose
// highlight doesn't move - the Main Menu and Options bars are recoloured where they are (no HUD piece moves), and
// Character Select's frame is drawn at scale 1, its size unknown to the learner (2026-11-11, test save)
#define CALL_TITLE_MENU    0x87d824u  // the title's Main Menu: New Game, Continue, Options, Download, Data Transfer
#define CALL_TITLE_OPTIONS 0x880798u  // its Options: TV Brightness, Rumble, Manual, Language Settings
#define CALL_CHAR_SELECT   0x5e29a0u  // Character Select: the three save cards
#define CALL_NEW_CHAR      0x87f8a0u  // New Game: Create New Character, Reuse Data (bars as the Main Menu's)
#define CALL_GENDER        0x8874ccu  // character creation's Gender: two cards side by side (the chosen one grows)
// Banner lists whose chosen row is drawn 32 further left (no highlight of its own moves: nothing to learn), measured
// from the HUD pieces and screenshots (2026-11-11, the Soaratorium)
#define CALL_BISTRO        0x931d1cu  // the Bistro: Order a Meal, Use Voucher, Use Gourmet Voucher, Request Rife Roast
#define CALL_QUEST_RANK    0x9839b8u  // the Quest Counter: Training, Low, High, Hunt of the Day
#define CALL_QUEST_STARS   0x984314u  // ... a rank's star levels
#define CALL_QUEST_LIST    0x981f1cu  // ... a level's quests (the list widget's caller: list_widget)
static const MenuGeom builtin[] = {
    {CALL_BISTRO, 868, 51, 364, 45, 0, 48, 0, 0, 0, 0, 0, true},
    {CALL_QUEST_RANK, 740, 142, 540, 40, 0, 50, 0, 0, 0, 0, 0, true},
    {CALL_QUEST_STARS, 740, 142, 540, 40, 0, 50, 0, 0, 0, 0, 0, true},
    {CALL_QUEST_LIST, 740, 202, 540, 37, 0, 42, 0, 0, 0, 0, 0, true},
    // the Palico Intense Training setup: # of times and Items (the learned bar, 36 apart), then Begin Training, a button
    // well below whose highlight is recoloured in place - nothing moves onto it, so its row's place is measured (the
    // button's piece 590,315 210 x 34)
    {.lr = 0x967514u, .x = 428, .y = 170, .w = 372, .h = 36, .dy = 36, .z = 97, .kw = 186, .kh = 18, .builtin = true,
     .offmask = 0x7, .offy = {0, 36, 145}, .cols = 1},
    // the World Map: 2 rows of 4 destinations, picture and name (a linked pair of cursors: cursor_update_as, linked[])
    {.lr = 0x993d04u, .x = 322, .y = 213, .w = 159, .h = 148, .dy = 148, .builtin = true, .cdx = 159, .cols = 4},
    {CALL_PAUSE_MENU, 970, 101, 282, 34, 0, 36, 97, 141, 17, 0, 0, true},
    {CALL_TITLE_MENU, 170, 153, 940, 64, 0, 84, 0, 0, 0, 0, 0, true},
    {CALL_TITLE_OPTIONS, 170, 195, 940, 64, 0, 84, 0, 0, 0, 0, 0, true},
    {CALL_CHAR_SELECT, 55, 146, 565, 98, 0, 116, 0, 0, 0, 0, 0, true},
    {CALL_NEW_CHAR, 170, 278, 940, 64, 0, 84, 0, 0, 0, 0, 0, true},
    {CALL_GENDER, 95, 280, 215, 215, 295, 0, 0, 0, 0, 0, 0, true},
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
static bool steps_differ(const MenuGeom *a, const MenuGeom *b) {
    return (a->dx || a->dy) && (b->dx || b->dy) && (!nearf(a->dx, b->dx) || !nearf(a->dy, b->dy));
}
static MenuGeom *geom_put(const MenuGeom *g) {
    MenuGeom *o = NULL;
    for (int i = 0; i < g_ngeoms && !o; i++)  // (one learned with as many cells first: a menu spaced by its count)
        if (g_geoms[i].lr == g->lr && g_geoms[i].count && g_geoms[i].count == g->count && same_kind(&g_geoms[i], g)) o = &g_geoms[i];
    if (!o) o = geom_kind(g);
    // (the same bar with another row step and another number of cells: a geometry of its own for that count, not a
    // merge - a list with rows not one step apart shows other steps with the same count, and stays one)
    if (o && o->kw && g->count && o->count != g->count && steps_differ(o, g)) o = NULL;
    if (o) {
        if (!o->kw) { *o = *g; return NULL; }
        bool more = false;
        if (!o->dx && !o->dy && (g->dx || g->dy)) o->dx = g->dx, o->dy = g->dy, more = true;
        if (!o->cdx && !o->cdy && (g->cdx || g->cdy)) o->cdx = g->cdx, o->cdy = g->cdy, more = true;
        if (!o->cols && g->cols) o->cols = g->cols, more = true;
        o->x = g->x, o->y = g->y;
        return more ? o : NULL;
    }
    MenuGeom n_g = *g;
    if (!geom_kind(g)) n_g.count = 0;  // (the count is kept only where it splits a menu)
    int n = 0, first = -1;
    for (int i = 0; i < g_ngeoms; i++)
        if (g_geoms[i].lr == g->lr) n++, first = first < 0 ? i : first;
    if (n >= MAX_VARIANTS) { g_geoms[first] = n_g; return &g_geoms[first]; }
    if (g_ngeoms >= MAX_GEOMS) return NULL;
    g_geoms[g_ngeoms] = n_g;
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
static int cursor_cells(uint32_t lr);
void menuptr_learned(const MenuGeom *g0) {
    if (is_builtin(g0->lr)) return;
    MenuGeom kept, gc = *g0, *g = &gc;
    gc.count = cursor_cells(g0->lr);  // (geom_put keeps it only for a menu spaced by its count)
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
        fprintf(f, "%08x %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.2f %.2f %.1f %.1f %.1f %.1f %d %d\n", kept.lr, kept.x, kept.y,
                kept.w, kept.h, kept.dx, kept.dy, kept.z, kept.kw, kept.kh, kept.ox, kept.oy, kept.cdx, kept.cdy, kept.cols,
                kept.count);
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
    // lr x y w h dx dy [z kw kh ox oy [cdx cdy [cols [count]]]]: older lines have no bar signature (a fixed place), no
    // column step, no column count or no cell count
    int k = sscanf(line, "%x %f %f %f %f %f %f %f %f %f %f %f %f %f %d %d", &g.lr, &g.x, &g.y, &g.w, &g.h, &g.dx, &g.dy, &g.z,
                   &g.kw, &g.kh, &g.ox, &g.oy, &g.cdx, &g.cdy, &g.cols, &g.count);
    if (k != 7 && k != 12 && k != 14 && k != 15 && k != 16) return false;
    if (k < 16) g.count = 0;
    if (k < 15) g.cols = 0;
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

// ---- the menu in focus, from the game's layout to the picture (runtime/menulearn.c map_menu): x' = ax x + bx, y' =
// ay y + by on the picture's 1280 x 720
static pthread_mutex_t g_map_lock = PTHREAD_MUTEX_INITIALIZER;
static float g_map[4];
static uint64_t g_map_ns;
void menuptr_set_screen_map(float ax, float bx, float ay, float by) {
    pthread_mutex_lock(&g_map_lock);
    g_map[0] = ax, g_map[1] = bx, g_map[2] = ay, g_map[3] = by, g_map_ns = plat_now_ns();
    pthread_mutex_unlock(&g_map_lock);
}
bool menuptr_screen_map(float m[4]) {
    pthread_mutex_lock(&g_map_lock);
    bool fresh = g_map_ns && plat_now_ns() - g_map_ns < LIVE_NS;
    if (fresh) memcpy(m, g_map, sizeof g_map);
    pthread_mutex_unlock(&g_map_lock);
    return fresh;
}

// ---- the cursors
// (col, cols 0: none; fixed: that many cells, no rows / page fields; masks: the step bits are the update's r2 (back) /
// r3 (on), not Up / Down)
typedef struct { uint32_t fn, row, rows, page, pages, total, col, cols; int fixed; bool masks; } Layout;
static const Layout LIST = {FN_CURSOR_UPDATE, 0xd, 0xe, 0x10, 0x12, 0x14, 0, 0, 0, false};
static const Layout GRID = {FN_GRID_UPDATE, 0x15, 0x17, 0x18, 0x1a, 0x1c, 0x14, 0x16, 0, false};
static const Layout GRID2 = {FN_GRID_UPDATE2, 0x15, 0x17, 0x18, 0x1a, 0x1c, 0x14, 0x16, 0, false};
// A two-way choice (the Yes / No of a prompt, "Create a character with these settings?"): 0x74fbb0(choice, pad bits,
// back mask, on mask) with the choice a byte (0 the first, 1 the second); A picks it (returns 1 / 2), B moves to the
// second and then cancels, the masks (Left 0x80 / Right 0x20 from the prompt's 0x84bd18) move between them
#define FN_CHOICE_UPDATE 0x74fbb0u
static const Layout CHOICE = {FN_CHOICE_UPDATE, 0, 0, 0, 0, 0, 0, 0, 2, true};
static int page_of(uint32_t cur, const Layout *L) { return L->fixed ? 0 : LD16(cur + L->page); }
static int pages_of(uint32_t cur, const Layout *L) { return L->fixed ? 1 : LD16(cur + L->pages); }

#define MAX_CURSORS 16
// A cursor is open while its update runs. Some menus run it only when a button is pressed (character creation's grids):
// those count as open while the renderer sees their highlight where their geometry puts it (bar_ns, menulearn.c), and for
// learning for LEARN_NS after it ran; a cursor that ran within FRESH_NS of its last run is the same opening of its menu.
#define LEARN_NS  3000000000ull
#define FRESH_NS  3000000000ull
#define RECENT_NS 30000000000ull  // (the renderer looks for the highlights of menus seen this lately)
typedef struct {
    uint32_t lr;
    uint64_t seen_ns, appeared_ns, moved_ns, since;  // since: presented frame its cell / page last changed (or it appeared)
    uint64_t bar_ns;                                 // when the renderer last saw its highlight where it should be
    int row, col, cells, cols, page, pages;          // cells: on its page now
    bool known;                                      // its cells' places are known
} Cursor;
static Cursor g_cursors[MAX_CURSORS];
static pthread_mutex_t g_cur_lock = PTHREAD_MUTEX_INITIALIZER;
static int cursor_cells(uint32_t lr) {  // (its cells now; 0: not seen)
    int n = 0;
    pthread_mutex_lock(&g_cur_lock);
    for (int i = 0; i < MAX_CURSORS; i++)
        if (g_cursors[i].lr == lr && g_cursors[i].seen_ns) n = g_cursors[i].cells;
    pthread_mutex_unlock(&g_cur_lock);
    return n;
}
static atomic_uint g_target_lr;    // the menu a target is for
static atomic_int g_target = -1;   // the cell asked for (-1 none)
static atomic_bool g_click;        // ... and picked: Confirm once it is selected
static atomic_bool g_confirm;      // a picked cell is selected now: the host presses Confirm
static atomic_uint g_page_lr;      // a page to turn: the menu, and which way (> 0 next)
static atomic_int g_page_dir;
static atomic_ullong g_asked_ns;   // when the last jump or page was asked for
static _Atomic float g_target_gx = -1.0f, g_target_gy = -1.0f;  // where the pointer was (layout), -1: not by the pointer
static atomic_ullong g_nudge_ns;   // when the host last nudged (pressed Down for a quiet cursor)
#define ASK_NS 400000000ull
static int g_probe = -1;

// (g_cur_lock held) the open cursor that last moved or appeared, of those whose cells are known (for the pointer: open,
// or its highlight seen; RECENT: seen lately at all, for the renderer to look for) or of those with more than one cell
// (for learning: a section bar's one row never moves)
enum { PICK_POINTER, PICK_LEARN, PICK_RECENT };
static Cursor *pick(int how) {
    uint64_t now = plat_now_ns(), best_t = 0;
    Cursor *best = NULL;
    for (int i = 0; i < MAX_CURSORS; i++) {
        Cursor *c = &g_cursors[i];
        if (!c->lr) continue;
        bool open = how == PICK_POINTER ? now - c->seen_ns < OPEN_NS || now - c->bar_ns < OPEN_NS
                    : how == PICK_LEARN ? now - c->seen_ns < LEARN_NS
                                        : now - c->seen_ns < RECENT_NS;
        if (!open || (how == PICK_LEARN ? c->cells < 2 : !c->known)) continue;
        // (one known only from its frame on screen, never run, comes last: a small frame's key piece can sit where
        // another screen happens to have a piece too - the Face grid's corner on the Style screen)
        uint64_t t = !c->seen_ns ? 0 : c->moved_ns > c->appeared_ns ? c->moved_ns : c->appeared_ns;
        if (!best || t >= best_t) best = c, best_t = t;
    }
    // A screen in front that isn't known - a running cursor of several cells that appeared well after the known one (a
    // prompt over a menu) - takes the mouse back to what it was: the pointer would only miss it, and its click is no
    // button (a right click would be Cancel, while the default keyboard and mouse have Confirm on the right button).
    // (Cursors that appear together - a list and the read-only list beside it - are not in front of each other.)
    if (best && how == PICK_POINTER)
        for (int i = 0; i < MAX_CURSORS; i++) {
            const Cursor *c = &g_cursors[i];
            if (c->lr && !c->known && c->cells >= 2 && now - c->seen_ns < OPEN_NS && c->appeared_ns > best->appeared_ns + 100000000ull)
                return NULL;
        }
    return best;
}

static bool running(uint32_t lr) {  // its update ran just now
    bool r = false;
    uint64_t now = plat_now_ns();
    pthread_mutex_lock(&g_cur_lock);
    for (int i = 0; i < MAX_CURSORS; i++)
        if (g_cursors[i].lr == lr) r = now - g_cursors[i].seen_ns < 50000000ull;
    pthread_mutex_unlock(&g_cur_lock);
    return r;
}
static bool same_frame(uint32_t a, uint32_t b) {  // the same highlight (layer and sizes)
    MenuGeom x, y;
    if (menuptr_variants(a, &x, 1) < 1 || menuptr_variants(b, &y, 1) < 1 || !x.kw || !y.kw) return false;
    return nearf(x.z, y.z) && fabsf(x.kw - y.kw) < 0.01f && fabsf(x.kh - y.kh) < 0.01f;
}
// (render thread) Of menus with the same frame (look-alikes: Style's grid and character creation's menu), the frame on
// screen is the one's whose cursor ran last: true when another with g's frame ran after g's did (or g's never did)
bool menuptr_frame_taken(const MenuGeom *g) {
    struct { uint32_t lr; uint64_t seen; } o[MAX_CURSORS];
    int n = 0;
    uint64_t own = 0;
    pthread_mutex_lock(&g_cur_lock);
    for (int i = 0; i < MAX_CURSORS; i++) {
        if (!g_cursors[i].lr || !g_cursors[i].seen_ns) continue;
        if (g_cursors[i].lr == g->lr) own = g_cursors[i].seen_ns;
        else o[n].lr = g_cursors[i].lr, o[n].seen = g_cursors[i].seen_ns, n++;
    }
    pthread_mutex_unlock(&g_cur_lock);
    for (int i = 0; i < n; i++) {
        MenuGeom x;
        if (o[i].seen > own && menuptr_variants(o[i].lr, &x, 1) > 0 && x.kw && nearf(x.z, g->z) && fabsf(x.kw - g->kw) < 0.01f &&
            fabsf(x.kh - g->kh) < 0.01f)
            return true;
    }
    return false;
}

void menuptr_bar_seen(uint32_t lr) {
    uint64_t now = plat_now_ns();
    pthread_mutex_lock(&g_cur_lock);
    for (int i = 0; i < MAX_CURSORS; i++)
        if (g_cursors[i].lr == lr) g_cursors[i].bar_ns = now;
    pthread_mutex_unlock(&g_cur_lock);
}

// (render thread) A menu of a small-scale frame shows it exactly on its cell (row, col): open there. A cursor not seen
// yet gets an entry (its cells unknown until it runs: as many as 16 rows of its columns; a cell beyond its real ones
// is given up by the update); a quiet one (not run lately) takes the cell shown.
void menuptr_bar_at(const MenuGeom *g, int row, int col) {
    if (g->cols < 1) return;
    uint64_t now = plat_now_ns();
    pthread_mutex_lock(&g_cur_lock);
    Cursor *e = NULL, *oldest = &g_cursors[0];
    for (int i = 0; i < MAX_CURSORS && !e; i++) {
        uint64_t t = g_cursors[i].seen_ns > g_cursors[i].bar_ns ? g_cursors[i].seen_ns : g_cursors[i].bar_ns;
        uint64_t to = oldest->seen_ns > oldest->bar_ns ? oldest->seen_ns : oldest->bar_ns;
        if (g_cursors[i].lr == g->lr) e = &g_cursors[i];
        else if (t < to) oldest = &g_cursors[i];
    }
    if (!e) {
        e = oldest;
        memset(e, 0, sizeof *e);
        e->lr = g->lr, e->cols = g->cols, e->cells = g->cols * 16, e->pages = 1, e->row = row, e->col = col;
        e->appeared_ns = now, e->since = g_present_count;
    }
    bool quiet = now - e->seen_ns > 50000000ull;
    // (seen again after a while is not appearing anew: only its cursor running says that - a stray piece where the frame
    // would be, on another screen, would otherwise make it the newest menu and take the pointer)
    // (the cell shown, for a quiet cursor - not a move: that would make a menu behind a prompt newer than the prompt,
    // and take the pointer from it)
    if (quiet && (e->row != row || e->col != col)) e->row = row, e->col = col, e->since = g_present_count;
    // (quiet, its cells may be another screen's: one call runs three screens of 15, 10 and 7 items, its look-alike one of
    // 2 - the last run's count left the rest of a bigger screen dead. Unknown until it runs: any cell is offered, and the
    // update gives up one beyond the page's last)
    if (quiet) e->cells = e->cols * 16;
    if (!e->cols) e->cols = g->cols;
    e->bar_ns = now, e->known = true;
    pthread_mutex_unlock(&g_cur_lock);
}

int menuptr_small_geoms(MenuGeom *out, int max) {
    int n = 0;
    pthread_mutex_lock(&g_geom_lock);
    for (int i = 0; i < g_ngeoms && n < max; i++) {
        const MenuGeom *o = &g_geoms[i];
        if (o->kw && o->cols > 0 && fabsf(4.0f * o->kw * o->kh) < 100.0f) out[n++] = *o;
    }
    pthread_mutex_unlock(&g_geom_lock);
    return n;
}

// Two calls whose menus look the same (one geometry each: the same bar, place, steps and columns): character creation's
// grids are run from two calls, three screens each, so a frame on screen is both of theirs; the update that runs takes
// a jump meant for the other
static bool running(uint32_t lr);
static bool same_frame(uint32_t a, uint32_t b);
static bool same_layout(uint32_t a, uint32_t b);
// A jump for menu A while B's update runs instead: B looks like A (the same frame) and A isn't running - the Style
// screen's grid and character creation's menu share frame, place and column step, so the frame on either screen is both
// of theirs. The jump becomes B's, its cell found again from where the pointer was, by B's own geometry.
static void retarget(uint32_t lr, int cols, int cells) {
    uint32_t a = atomic_load(&g_target_lr);
    if (running(a) || !same_frame(a, lr)) return;
    float gx = atomic_load(&g_target_gx), gy = atomic_load(&g_target_gy);
    if (gx < 0) {  // (not by the pointer - the wheel: the same cell, when the two are laid out alike)
        if (same_layout(a, lr)) atomic_store(&g_target_lr, lr);
        return;
    }
    MenuGeom g;
    if (menuptr_variants(lr, &g, 1) < 1) return;
    for (int i = 0; i < cells; i++) {
        float x0, y0, x1, y1;
        menuptr_row_rect(&g, i, cols, &x0, &y0, &x1, &y1);
        if (gx >= x0 && gx < x1 && gy >= y0 && gy < y1) {
            atomic_store(&g_target, i), atomic_store(&g_target_lr, lr);
            return;
        }
    }
    atomic_store(&g_target, -1), atomic_store(&g_click, false);  // (over none of B's cells)
}
static bool same_layout(uint32_t a, uint32_t b) {
    MenuGeom x, y;
    if (menuptr_variants(a, &x, 1) < 1 || menuptr_variants(b, &y, 1) < 1 || !x.kw || !y.kw) return false;
    return nearf(x.z, y.z) && fabsf(x.kw - y.kw) < 0.01f && fabsf(x.kh - y.kh) < 0.01f && nearf(x.x, y.x) && nearf(x.y, y.y) &&
           nearf(x.dx, y.dx) && nearf(x.dy, y.dy) && nearf(x.cdx, y.cdx) && nearf(x.cdy, y.cdy) && x.cols == y.cols;
}

static int cols_of(uint32_t cur, const Layout *L) {
    int n = L->cols ? LD8(cur + L->cols) : 1;
    return n < 1 ? 1 : n;
}
static int cells_now(uint32_t cur, const Layout *L) {  // cells on the cursor's page
    if (L->fixed) return L->fixed;
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
    int row = LD8(cur + L->row), col = L->col ? LD8(cur + L->col) : 0, cnt = L->fixed ? L->fixed : LD8(cur + L->rows);
    int page = page_of(cur, L);
    int k = -1;
    for (int i = 0; i < 32 && k < 0; i++)
        if (last[i].lr == lr) k = i;
    if (k >= 0 && last[k].cur == cur && last[k].row == row && last[k].col == col && last[k].cnt == cnt && last[k].page == page)
        return;
    if (k < 0) k = next++ % 32, last[k].lr = lr;
    LOG("[menuptr] %s %08x from %08x row %d -> %d of %d, column %d -> %d of %d, page %d / %u, %u items, bits %08x%s\n",
        L->col ? "grid" : L->fixed ? "choice" : "cursor", cur, lr, row0, row, cnt, col0, col, cols_of(cur, L), page,
        pages_of(cur, L), L->fixed ? L->fixed : LD16(cur + L->total), bits, known(lr) ? " (known)" : "");
    last[k].cur = cur, last[k].row = row, last[k].col = col, last[k].cnt = cnt, last[k].page = page;
}

// A list widget that runs the list cursor itself: the menus built on it - the Quest Counter's quest list, and 0x8e54b4's -
// are told apart by the widget's caller, which stands for the call. It has two cursors: its list (widget+4, from
// 0x74c4ec: the caller's address) and a second one (widget+0x28, from 0x74c514: the quest's details pages, L / R), which
// gets the caller's address with the top bit set - one id for both made the pointer flip between them every frame
#define FN_LIST_WIDGET 0x74c130u
#define CALL_WIDGET_A  0x74c4ecu
#define CALL_WIDGET_B  0x74c514u
#define WIDGET_SECOND  0x80000000u
static uint32_t g_widget_lr;  // (game thread: the caller of the widget running now)
static void list_widget(Cpu *c) {
    uint32_t saved = g_widget_lr;
    g_widget_lr = c->r[14] & ~1u;
    fnhook_call_original(c, FN_LIST_WIDGET);
    g_widget_lr = saved;
}

// Two list cursors that make one grid: one picks the row (Up / Down), the other the column (Left / Right) - the World Map's
// 2 x 4 destinations. The row cursor's call is the menu (its cells are rows x columns, its column the other cursor's);
// the column cursor only reports its column and takes the column of a jump, and is never a menu of its own.
#define CALL_WORLD_ROWS 0x993d04u
#define CALL_WORLD_COLS 0x993d2cu
typedef struct { uint32_t row_lr, col_lr; int cols; } Linked;
static const Linked linked[] = {{CALL_WORLD_ROWS, CALL_WORLD_COLS, 4}};
#define NLINKED (sizeof linked / sizeof *linked)
static _Atomic int g_link_col[NLINKED];
static int linked_by(uint32_t lr, bool col) {
    for (unsigned i = 0; i < NLINKED; i++)
        if ((col ? linked[i].col_lr : linked[i].row_lr) == lr) return (int)i;
    return -1;
}

static _Atomic uint64_t g_any_cursor_ns;  // when any cursor of the four classes last ran (a menu is up)
bool menuptr_cursor_ran(uint64_t within_ns) {
    uint64_t t = atomic_load(&g_any_cursor_ns);
    return t && plat_now_ns() - t < within_ns;
}
static void cursor_update_as(Cpu *c, const Layout *L) {
    uint32_t cur = c->r[0], lr = c->r[14] & ~1u;
    atomic_store(&g_any_cursor_ns, plat_now_ns());
    if ((lr == CALL_WIDGET_A || lr == CALL_WIDGET_B) && g_widget_lr) lr = g_widget_lr | (lr == CALL_WIDGET_B ? WIDGET_SECOND : 0);
    // (the host's nudge - a short Down that makes a quiet cursor run - never moves anything itself: a jump puts its own
    // step in, and with none, or for another menu than the one that runs, it would walk the selection down)
    if (plat_now_ns() - atomic_load(&g_nudge_ns) < 150000000ull) c->r[1] &= ~BIT_DOWN;
    int lc = L->col ? -1 : linked_by(lr, true);
    if (lc >= 0) {  // (a linked pair's column cursor: the column of a jump for its row cursor's menu, as one step)
        const Linked *k = &linked[lc];
        int col0 = LD8(cur + L->row), target = atomic_load(&g_target);
        if (atomic_load(&g_target_lr) == k->row_lr && target >= 0) {
            int tc = target % k->cols;
            c->r[1] &= ~(BIT_UP | BIT_DOWN | BIT_LEFT | BIT_RIGHT);
            if (tc != col0) {
                int from = tc > 0 ? tc - 1 : 1;
                ST8(cur + L->row, (uint8_t)from);
                c->r[1] |= tc > from ? BIT_RIGHT : BIT_LEFT;
            }
        }
        uint32_t bits = c->r[1];
        fnhook_call_original(c, L->fn);
        if (g_probe > 0) probe(c, cur, bits, lr, col0, 0, L);
        atomic_store(&g_link_col[lc], LD8(cur + L->row));
        return;
    }
    int lk = L->col ? -1 : linked_by(lr, false);
    int row0 = LD8(cur + L->row), col0 = L->col ? LD8(cur + L->col) : 0, cols = cols_of(cur, L), cells = cells_now(cur, L);
    if (lk >= 0) col0 = atomic_load(&g_link_col[lk]), cols = linked[lk].cols, cells *= cols;
    if (atomic_load(&g_page_lr) == lr && atomic_load(&g_page_dir)) {  // (the wheel: a page)
        int dir = atomic_exchange(&g_page_dir, 0);
        atomic_store(&g_target, -1), atomic_store(&g_click, false);
        c->r[1] &= ~(BIT_UP | BIT_DOWN | BIT_LEFT | BIT_RIGHT);  // (a nudge's Down: only the page)
        if (L->col) c->r[1] |= call1(c, dir > 0 ? FN_GRID_NEXT : FN_GRID_PREV, cur);
        else c->r[1] |= dir > 0 ? BIT_RIGHT : BIT_LEFT;
    } else if (atomic_load(&g_target) >= 0 && atomic_load(&g_target_lr) != lr) {
        retarget(lr, cols, cells);  // (a look-alike runs instead of the menu the jump was for)
    }
    if (atomic_load(&g_target_lr) == lr && !atomic_load(&g_page_dir)) {
        static int steps;  // (updates spent on this target: a cell that can't be reached is given up)
        static int last_target = -1;
        int target = atomic_load(&g_target);
        // (a jump: the steps the pad gives meanwhile - the host's nudge for a cursor that only runs on a press - are left
        // out, also when the target is given up: a cell beyond the page's last, offered while its count was unknown)
        uint32_t back = L->masks ? c->r[2] : BIT_UP, on = L->masks ? c->r[3] : BIT_DOWN;  // (a choice: its caller's masks)
        if (target >= 0) c->r[1] &= ~(BIT_UP | BIT_DOWN | BIT_LEFT | BIT_RIGHT | back | on);
        if (target != last_target) steps = 0, last_target = target;
        if (target >= cells || (target >= 0 && ++steps > 4)) {
            atomic_store(&g_target, -1), atomic_store(&g_click, false);
            target = last_target = -1;
        }
        // (the cursor is put beside the target and makes one ordinary step onto it: in a grid the row first, its column
        // left as it is, then the column on the next update - writing both at once let the Style screen's grid take its
        // column back)
        int tr = target >= 0 ? target / cols : 0, tc = target >= 0 ? target % cols : 0, rows = (cells + cols - 1) / cols;
        if (target >= 0 && tr != row0 && rows > 1) {
            MenuGeom g;
            bool across = !L->col && !L->masks && menuptr_geometry(&g) && g.dy == 0 && g.dx != 0;  // (items side by side)
            int from = tr > 0 ? tr - 1 : 1;
            ST8(cur + L->row, (uint8_t)from);
            c->r[1] |= tr > from ? (across ? BIT_RIGHT : on) : (across ? BIT_LEFT : back);
        } else if (target >= 0 && tc != col0 && cols > 1 && lk < 0) {  // (a linked pair: the column cursor's, above)
            int from = tc > 0 ? tc - 1 : 1;
            ST8(cur + L->col, (uint8_t)from);
            c->r[1] |= tc > from ? BIT_RIGHT : BIT_LEFT;
        }
    }
    uint32_t bits = c->r[1];
    fnhook_call_original(c, L->fn);
    if (g_probe > 0) probe(c, cur, bits, lr, row0, col0, L);
    uint64_t now = plat_now_ns();
    int row = LD8(cur + L->row), col = L->col ? LD8(cur + L->col) : 0, page = page_of(cur, L);
    if (lk >= 0) col = atomic_load(&g_link_col[lk]);
    bool is_known = known(lr);
    pthread_mutex_lock(&g_cur_lock);
    Cursor *e = NULL, *oldest = &g_cursors[0];
    for (int i = 0; i < MAX_CURSORS && !e; i++) {
        if (g_cursors[i].lr == lr) e = &g_cursors[i];
        else if (g_cursors[i].seen_ns < oldest->seen_ns) oldest = &g_cursors[i];
    }
    bool fresh = !e || now - e->seen_ns >= FRESH_NS;
    if (!e) e = oldest, e->lr = lr;
    if (fresh) e->appeared_ns = now, e->moved_ns = 0, e->since = g_present_count;
    else if (e->row != row || e->col != col || e->page != page) e->moved_ns = now, e->since = g_present_count;
    e->seen_ns = now, e->row = row, e->col = col, e->page = page, e->pages = pages_of(cur, L), e->known = is_known;
    e->cols = cols_of(cur, L), e->cells = cells_now(cur, L);
    if (lk >= 0) e->cols = linked[lk].cols, e->cells *= linked[lk].cols;
    pthread_mutex_unlock(&g_cur_lock);
    if (atomic_load(&g_target_lr) == lr && atomic_load(&g_target) == row * e->cols + col) {
        atomic_store(&g_target, -1);
        if (atomic_exchange(&g_click, false)) atomic_store(&g_confirm, true);
    }
}
static void cursor_update(Cpu *c) { cursor_update_as(c, &LIST); }
static void grid_update(Cpu *c) { cursor_update_as(c, &GRID); }
static void grid_update2(Cpu *c) { cursor_update_as(c, &GRID2); }
static void choice_update(Cpu *c) { cursor_update_as(c, &CHOICE); }

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
    Cursor *c = pick(PICK_POINTER);
    if (c) *out = *c;
    pthread_mutex_unlock(&g_cur_lock);
    return c != NULL;
}

bool menuptr_focus(uint32_t *lr, int *row, int *col, int *page, uint64_t *since, int *cols) {
    pthread_mutex_lock(&g_cur_lock);
    Cursor *c = pick(PICK_LEARN);
    if (c) *lr = c->lr, *row = c->row, *col = c->col, *page = c->page, *since = c->since, *cols = c->cols;
    pthread_mutex_unlock(&g_cur_lock);
    return c != NULL;
}
bool menuptr_pointer_focus(uint32_t *lr, int *row, int *col, uint64_t *since) {
    pthread_mutex_lock(&g_cur_lock);
    Cursor *c = pick(PICK_RECENT);  // (open or not: its highlight on screen is what makes it open, for a quiet cursor)
    if (c) *lr = c->lr, *row = c->row, *col = c->col, *since = c->since;
    pthread_mutex_unlock(&g_cur_lock);
    return c != NULL;
}

bool menuptr_slider_open(void);
bool menuptr_open(void) {
    Cursor c;
    return pointer_menu(&c) || menuptr_slider_open();  // (Skin Tone's slider: a pointer screen too)
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
uint64_t menuptr_menu_id(void) {
    Cursor c;
    return pointer_menu(&c) ? c.appeared_ns ^ c.lr : 0;
}

bool menuptr_geometry(MenuGeom *out) {
    Cursor c;
    if (!pointer_menu(&c)) return false;
    pthread_mutex_lock(&g_live_lock);
    bool live = g_live_ns && g_live.lr == c.lr && plat_now_ns() - g_live_ns < LIVE_NS && (!g_live.count || g_live.count == c.cells);
    if (live) *out = g_live;
    pthread_mutex_unlock(&g_live_lock);
    if (live) return true;
    MenuGeom v[MAX_VARIANTS];
    int n = menuptr_variants(c.lr, v, MAX_VARIANTS), pick = -1;
    // (one learned with as many cells as it has now - a menu spaced by its count -, else the last learned for any count,
    // else the last learned)
    for (int i = 0; i < n; i++)
        if (v[i].count && v[i].count == c.cells) pick = i;
    for (int i = n - 1; i >= 0 && pick < 0; i--)
        if (!v[i].count) pick = i;
    if (n) *out = v[pick >= 0 ? pick : n - 1];
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
    if (w <= 0 && h <= 0 && sh <= 0 && sw > 0 && fabsf(g->cdx) <= 0 && fabsf(g->cdy) <= 0) {
        // (a frame of no known size in a list laid out across - a prompt's Yes / No: its frame sits on the middle of the
        // option there, so the cell is a step wide about it, and as tall)
        *x0 = x - sw / 2, *x1 = x + sw / 2, *y0 = y - sw / 2, *y1 = y + sw / 2;
        return;
    }
    if (w <= 0 && h <= 0) {  // (a frame of no known size, its corner: the cell is one step each way from it; a list's row
                             // runs on to the middle of the screen from near the left edge, as far past the middle as it
                             // starts before it from further in - a list in the middle: the World Map's destinations,
                             // 384 to 896 -, or to the edge when it starts right of the middle)
        *x0 = x, *y0 = y, *y1 = y + (sh > 0 ? sh : sw);
        *x1 = sw > 0 ? x + sw : sh > 0 ? (x >= 640.0f ? 1280.0f : x >= 200.0f ? 1280.0f - x : 640.0f) : x + sh;
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
    static const struct { uint32_t lr; float y0, y1, lx0, lx1, rx0, rx1; } arrows[] = {  // (page arrows, 1280x720)
        {CALL_PAUSE_MENU, 64, 97, 1108, 1146, 1214, 1252},
        {CALL_QUEST_LIST, 146, 184, 1144, 1182, 1236, 1274},
    };
    for (unsigned i = 0; i < sizeof arrows / sizeof *arrows; i++)
        if (c.lr == arrows[i].lr && y >= arrows[i].y0 && y < arrows[i].y1 && c.pages > 1) {
            if (x >= arrows[i].lx0 && x < arrows[i].lx1) return MENUPTR_PAGE_LEFT;
            if (x >= arrows[i].rx0 && x < arrows[i].rx1) return MENUPTR_PAGE_RIGHT;
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

static void select_cell(int cell, bool pick_it);
void menuptr_select_at(int cell, bool pick, float gx, float gy) {
    atomic_store(&g_target_gx, gx), atomic_store(&g_target_gy, gy);
    select_cell(cell, pick);
}
void menuptr_select(int cell, bool pick) {  // (not by the pointer: the wheel)
    atomic_store(&g_target_gx, -1.0f), atomic_store(&g_target_gy, -1.0f);
    select_cell(cell, pick);
}
void menuptr_nudged(void) { atomic_store(&g_nudge_ns, plat_now_ns()); }
static void select_cell(int cell, bool pick_it) {
    Cursor c;
    if (cell < 0 || !pointer_menu(&c)) return;
    if (pick_it && cell == c.row * c.cols + c.col) {
        atomic_store(&g_target, -1), atomic_store(&g_click, false), atomic_store(&g_confirm, true);
        return;
    }
    atomic_store(&g_target, -1);
    atomic_store(&g_target_lr, c.lr);
    atomic_store(&g_click, pick_it);
    atomic_store(&g_asked_ns, plat_now_ns());
    atomic_store(&g_target, cell);
}

void menuptr_page_turn(int dir) {
    Cursor c;
    if (!dir || !pointer_menu(&c) || c.pages < 2) return;
    atomic_store(&g_page_lr, c.lr);
    atomic_store(&g_asked_ns, plat_now_ns());
    atomic_store(&g_page_dir, dir > 0 ? 1 : -1);
}

bool menuptr_take_confirm(void) { return atomic_exchange(&g_confirm, false); }

// ---- character creation's Skin Tone slider: a gradient of 184 tones (1..184, 0xb8) with a marker. Its update
// 0x8babe4(slider, ..., pad bits r2), every frame from 0x8ba920: Left (0x80) / Right (0x20) step the tone by 1 (10 with
// L / R held); the tone is read from its GUI object's parameter 0x10 (+0x24 the GUI: 0x745a78 gets, 0x745a40 sets it, the
// value a float in s0) and kept at +0x84 too. The marker is at x 143 + 2 * tone at 1280x720. The pointer sets the tone
// the jump's way: the parameter one tone off, then one step - the game moves the marker and colours the hunter itself.
#define FN_SLIDER_UPDATE 0x8babe4u
#define FN_GUI_SET       0x745a40u
#define SLIDER_GUI   0x24u
#define SLIDER_TONE  0x84u
#define SLIDER_MAX   184
static atomic_ullong g_slider_ns;      // when its update last ran
static atomic_int g_slider_tone;
static atomic_int g_slider_target = -1;
static atomic_bool g_slider_pick;      // Confirm once the target tone is set (B on this screen drops the change)

static void slider_update(Cpu *c) {
    uint32_t s = c->r[0];
    int target = atomic_load(&g_slider_target), tone = (int)LD32(s + SLIDER_TONE);
    if (target >= 1 && target <= SLIDER_MAX && target != tone) {
        uint32_t gui = LD32(s + SLIDER_GUI);
        int from = target > 1 ? target - 1 : 2;
        if (gui >= 0x08000000u && !(gui & 3)) {
            uint32_t saved[16], s0 = c->v.s[0];
            memcpy(saved, c->r, sizeof saved);
            c->r[0] = gui, c->r[1] = 0x10u, c->v.f[0] = (float)from;
            guest_call(c, FN_GUI_SET);
            memcpy(c->r, saved, sizeof saved);
            c->v.s[0] = s0;
            c->r[2] = (c->r[2] & ~(BIT_LEFT | BIT_RIGHT | BIT_UP | BIT_DOWN)) | (target > from ? BIT_RIGHT : BIT_LEFT);
        }
    }
    fnhook_call_original(c, FN_SLIDER_UPDATE);
    tone = (int)LD32(s + SLIDER_TONE);
    atomic_store(&g_slider_tone, tone);
    atomic_store(&g_slider_ns, plat_now_ns());
    if (tone == atomic_load(&g_slider_target)) {
        atomic_store(&g_slider_target, -1);
        if (atomic_exchange(&g_slider_pick, false)) atomic_store(&g_confirm, true);  // (a double click: then Confirm)
    }
}

bool menuptr_slider_open(void) {
    uint64_t seen = atomic_load(&g_slider_ns);
    return seen && plat_now_ns() - seen < OPEN_NS;
}
bool menuptr_slider_hit(float x, float y) { return x >= 130.0f && x < 540.0f && y >= 268.0f && y < 392.0f; }
bool menuptr_slider_panel(float x, float y) { return x >= 20.0f && x < 666.0f && y >= 214.0f && y < 558.0f; }
void menuptr_slider_set(float x, bool pick) {
    int t = (int)lroundf((x - 143.0f) / 2.0f);
    t = t < 1 ? 1 : t > SLIDER_MAX ? SLIDER_MAX : t;
    if (pick && t == atomic_load(&g_slider_tone)) return (void)atomic_store(&g_confirm, true);
    atomic_store(&g_slider_pick, pick);
    atomic_store(&g_slider_target, t);
}
void menuptr_slider_step(int d) {
    int t = atomic_load(&g_slider_tone) + d;
    atomic_store(&g_slider_target, t < 1 ? 1 : t > SLIDER_MAX ? SLIDER_MAX : t);
}

// A jump or a page waits for a cursor whose update has not run lately: one that only runs on a press (character
// creation's grids). The host then presses Down briefly, so that the menu runs it (the hook gives it its own bits instead).
bool menuptr_needs_nudge(void) {
    // (a jump or page not done in ASK_NS is given up: its menu stopped - a message over it - and nudging on would hold
    // Down there for good, which kept the message's own Confirm from getting through)
    if ((atomic_load(&g_target) >= 0 || atomic_load(&g_page_dir)) && plat_now_ns() - atomic_load(&g_asked_ns) > ASK_NS) {
        atomic_store(&g_target, -1), atomic_store(&g_click, false), atomic_store(&g_page_dir, 0);
        return false;
    }
    uint32_t lr = 0;
    if (atomic_load(&g_page_dir)) lr = atomic_load(&g_page_lr);
    else if (atomic_load(&g_target) >= 0) lr = atomic_load(&g_target_lr);
    if (!lr) return false;
    bool quiet = false;
    uint64_t now = plat_now_ns();
    pthread_mutex_lock(&g_cur_lock);
    for (int i = 0; i < MAX_CURSORS; i++)
        if (g_cursors[i].lr == lr) quiet = now - g_cursors[i].seen_ns > 50000000ull && now - g_cursors[i].bar_ns < OPEN_NS;
    pthread_mutex_unlock(&g_cur_lock);
    return quiet;
}

void menuptr_init(void) {
    g_probe = getenv("MHGU_MENUPTR_PROBE") != NULL;
    geoms_load();
    fnhook_set(FN_CURSOR_UPDATE, hle_register("menuptr:cursor_update", cursor_update));
    fnhook_set(FN_GRID_UPDATE, hle_register("menuptr:grid_update", grid_update));
    fnhook_set(FN_GRID_UPDATE2, hle_register("menuptr:grid_update2", grid_update2));
    fnhook_set(FN_SLIDER_UPDATE, hle_register("menuptr:slider_update", slider_update));
    fnhook_set(FN_CHOICE_UPDATE, hle_register("menuptr:choice_update", choice_update));
    fnhook_set(FN_LIST_WIDGET, hle_register("menuptr:list_widget", list_widget));
}
