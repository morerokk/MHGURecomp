// The mouse in the game's list and grid menus (runtime/hle/menuptr.c). Any thread.
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum { MENUPTR_NONE = -1, MENUPTR_PAGE_LEFT = -2, MENUPTR_PAGE_RIGHT = -3 };

// Where a menu's cells are, on the 1280x720 screen as the game lays it out: the first cell's highlight bar (x, y its top
// left, w x h), the step to the next row (dx, dy) and to the next column (cdx, cdy: 0 for a list), by the menu's call
// of the cursor update (lr). The bar's signature (kw 0: none, a fixed place): its biggest piece's layer z and half size
// kw x kh (the HUD matrix's m[14], m[0], m[5]), and where the bar's box starts from that piece's top left (ox, oy).
// Rows not one step apart (headers between groups: the Multiplayer menu): offmask's rows (of the first MENUPTR_ROWS) are
// at offx / offy from the first row's bar instead; the others are a whole number of steps from the nearest such row.
// cols: its cursor's columns (0: not known; kept for a frame of a small scale, whose menu the renderer finds by it).
#define MENUPTR_ROWS 16
typedef struct {
    uint32_t lr;
    float x, y, w, h, dx, dy;
    float z, kw, kh, ox, oy;
    bool builtin;
    uint32_t offmask;
    float offx[MENUPTR_ROWS], offy[MENUPTR_ROWS];
    float cdx, cdy;
    int cols;
} MenuGeom;
void menuptr_row_offset(const MenuGeom *g, int row, float *x, float *y);  // a row's bar from the first row's

// A cell is row * columns + column, on the menu's page; a list has one column.
void menuptr_init(void);
bool menuptr_open(void);                 // a menu whose cells are known is open
int menuptr_row(void);                   // its selected cell, -1 when none is open
int menuptr_rows(void);                  // the cells on its page now (0: none open)
int menuptr_cols(void);                  // its columns (1: a list)
int menuptr_pages(void);                 // its pages
uint64_t menuptr_menu_id(void);          // which menu it is, and since when (another value: another menu, or opened again)
bool menuptr_geometry(MenuGeom *out);    // its geometry (where the renderer last found its bar)
void menuptr_row_rect(const MenuGeom *g, int cell, int cols, float *x0, float *y0, float *x1, float *y1);  // a cell's box
int menuptr_hit(float x, float y);       // a point of the 1280x720 screen: a cell of the page, MENUPTR_PAGE_*, NONE
void menuptr_row_centre(int cell, float *x, float *y);  // (1280x720)
void menuptr_select(int cell, bool pick); // move the selection there (the game's way); pick: then Confirm it
void menuptr_select_at(int cell, bool pick, float x, float y);  // the same, from the pointer at (x, y) of the layout
void menuptr_nudged(void);               // the host pressed Down to wake a quiet cursor: no update takes it as a step
bool menuptr_frame_taken(const MenuGeom *g);  // another menu with g's frame ran lately (render thread: menulearn.c)
bool menuptr_take_confirm(void);         // a picked cell is selected: Confirm it now (once)
bool menuptr_needs_nudge(void);          // a jump or page waits for a cursor that runs only on a press: press Down briefly
// Character creation's Skin Tone slider (open: its update ran just now; menuptr_open counts it): a point of the 1280x720
// layout on its gradient, or on its panel; the tone at x (pick: then Confirm, as B drops the change there); a step of
// the tone (the wheel)
bool menuptr_slider_open(void);
bool menuptr_slider_hit(float x, float y);
bool menuptr_slider_panel(float x, float y);
void menuptr_slider_set(float x, bool pick);
void menuptr_slider_step(int d);
void menuptr_page_turn(int dir);         // the next (> 0) or previous page, the game's way (its own paging buttons)
// Where the renderer put the menu in focus: from its layout's 1280x720 to the picture's (its whole width and height),
// x' = m[0] x + m[1], y' = m[2] y + m[3]; false when not found lately (runtime/menulearn.c sets it every frame)
bool menuptr_screen_map(float m[4]);
void menuptr_set_screen_map(float ax, float bx, float ay, float by);

// For runtime/menulearn.c (render thread)
// The menu to learn: the open cursor with more than one cell that last moved or appeared, its row, column and page, and
// the presented frame they last changed; false when none is open
bool menuptr_focus(uint32_t *lr, int *row, int *col, int *page, uint64_t *since, int *cols);
// The menu whose highlight to look for: of the known ones seen lately, the one that last moved or appeared
bool menuptr_pointer_focus(uint32_t *lr, int *row, int *col, uint64_t *since);
void menuptr_bar_seen(uint32_t lr);       // its highlight is on screen where its geometry puts it: the menu is open
// The highlight of a menu of a small-scale frame is on screen exactly on one of its cells (row, col): it is open there,
// even if its cursor has not run yet (one that runs only on a press). The geometries to look for: menuptr_small_geoms.
void menuptr_bar_at(const MenuGeom *g, int row, int col);
int menuptr_small_geoms(MenuGeom *out, int max);
int menuptr_variants(uint32_t lr, MenuGeom *out, int max);  // the geometries known for a call
// A step seen twice (a row's: dx / dy set; a column's: cdx / cdy set, dx and dy 0): kept with the call's geometry for
// that bar, or a new geometry
void menuptr_learned(const MenuGeom *g);
void menuptr_live(const MenuGeom *g);     // where its bar is this frame (NULL: not found)
// The bar of the menu the pointer works on is at (x, y) (its box's top left, less its column's steps) on its row: rows
// found away from where its geometry says are kept (g: the geometry the bar was found by)
void menuptr_row_seen(const MenuGeom *g, int row, float x, float y);
