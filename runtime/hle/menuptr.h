// The mouse in the game's list menus (runtime/hle/menuptr.c). Any thread.
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum { MENUPTR_NONE = -1, MENUPTR_PAGE_LEFT = -2, MENUPTR_PAGE_RIGHT = -3 };

// Where a menu's rows are, on the 1280x720 screen as the game lays it out: the first row's highlight bar (x, y its top
// left, w x h) and the step to the next row (dx, dy), by the menu's call of the cursor update (lr). The bar's signature
// (kw 0: none, a fixed place): its biggest piece's layer z and half size kw x kh (the HUD matrix's m[14], m[0], m[5]),
// and where the bar's box starts from that piece's top left (ox, oy).
typedef struct {
    uint32_t lr;
    float x, y, w, h, dx, dy;
    float z, kw, kh, ox, oy;
    bool builtin;
} MenuGeom;

void menuptr_init(void);
bool menuptr_open(void);                 // a menu whose rows are known is open
int menuptr_row(void);                   // its selected row on the page, -1 when none is open
int menuptr_rows(void);                  // the rows on its page now (0: none open)
bool menuptr_geometry(MenuGeom *out);    // its geometry (where the renderer last found its bar)
void menuptr_row_rect(const MenuGeom *g, int row, float *x0, float *y0, float *x1, float *y1);  // a row's band
int menuptr_hit(float x, float y);       // a point of the 1280x720 screen: a row of the page, MENUPTR_PAGE_*, NONE
void menuptr_row_centre(int row, float *x, float *y);  // (1280x720)
void menuptr_select(int row, bool pick); // move the selection there (the game's way); pick: then Confirm it
bool menuptr_take_confirm(void);         // a picked row is selected: Confirm it now (once)

// For runtime/menulearn.c (render thread)
// The menu to learn: the open cursor with more than one row that last moved or appeared, its row and page, and the
// presented frame they last changed; false when none is open
bool menuptr_focus(uint32_t *lr, int *row, int *page, uint64_t *since);
bool menuptr_pointer_focus(uint32_t *lr, int *row, uint64_t *since);  // the menu the pointer works on (rows known)
int menuptr_variants(uint32_t lr, MenuGeom *out, int max);  // the geometries known for a call
void menuptr_learned(const MenuGeom *g);  // a geometry seen twice
void menuptr_live(const MenuGeom *g);     // where its bar is this frame (NULL: not found)
