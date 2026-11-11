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
#define MENUPTR_ROWS 16
typedef struct {
    uint32_t lr;
    float x, y, w, h, dx, dy;
    float z, kw, kh, ox, oy;
    bool builtin;
    uint32_t offmask;
    float offx[MENUPTR_ROWS], offy[MENUPTR_ROWS];
    float cdx, cdy;
} MenuGeom;
void menuptr_row_offset(const MenuGeom *g, int row, float *x, float *y);  // a row's bar from the first row's

// A cell is row * columns + column, on the menu's page; a list has one column.
void menuptr_init(void);
bool menuptr_open(void);                 // a menu whose cells are known is open
int menuptr_row(void);                   // its selected cell, -1 when none is open
int menuptr_rows(void);                  // the cells on its page now (0: none open)
int menuptr_cols(void);                  // its columns (1: a list)
int menuptr_pages(void);                 // its pages
bool menuptr_geometry(MenuGeom *out);    // its geometry (where the renderer last found its bar)
void menuptr_row_rect(const MenuGeom *g, int cell, int cols, float *x0, float *y0, float *x1, float *y1);  // a cell's box
int menuptr_hit(float x, float y);       // a point of the 1280x720 screen: a cell of the page, MENUPTR_PAGE_*, NONE
void menuptr_row_centre(int cell, float *x, float *y);  // (1280x720)
void menuptr_select(int cell, bool pick); // move the selection there (the game's way); pick: then Confirm it
bool menuptr_take_confirm(void);         // a picked cell is selected: Confirm it now (once)
void menuptr_page_turn(int dir);         // the next (> 0) or previous page, the game's way (its own paging buttons)

// For runtime/menulearn.c (render thread)
// The menu to learn: the open cursor with more than one cell that last moved or appeared, its row, column and page, and
// the presented frame they last changed; false when none is open
bool menuptr_focus(uint32_t *lr, int *row, int *col, int *page, uint64_t *since);
bool menuptr_pointer_focus(uint32_t *lr, int *row, int *col, uint64_t *since);  // the menu the pointer works on
int menuptr_variants(uint32_t lr, MenuGeom *out, int max);  // the geometries known for a call
// A step seen twice (a row's: dx / dy set; a column's: cdx / cdy set, dx and dy 0): kept with the call's geometry for
// that bar, or a new geometry
void menuptr_learned(const MenuGeom *g);
void menuptr_live(const MenuGeom *g);     // where its bar is this frame (NULL: not found)
// The bar of the menu the pointer works on is at (x, y) (its box's top left, less its column's steps) on its row: rows
// found away from where its geometry says are kept (g: the geometry the bar was found by)
void menuptr_row_seen(const MenuGeom *g, int row, float x, float y);
