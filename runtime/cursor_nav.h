// The mouse as a menu cursor, without knowing where the game's rows are: the movement of the pointer over the window
// becomes presses of Up / Down / Left / Right in the menu view (MenuUp ... of runtime/actions.h), one per row's height
// moved. Pure logic (host-tested in tests/test_actions.c); the host feeds it in host_input.c.
#pragma once
#include <math.h>
#include <stdint.h>

enum { CNAV_UP, CNAV_DOWN, CNAV_LEFT, CNAV_RIGHT, CNAV_DIRS };

typedef struct {
    float ax, ay;      // movement since the last press, window pixels (y down)
    uint64_t last_ns;  // the last movement fed
} CursorNav;

#define CNAV_IDLE_NS 250000000ull  // a pause this long forgets the movement not yet pressed
#define CNAV_MAX_PRESSES 4         // one feed gives at most this many presses

// Feed a pointer movement (dx, dy window pixels) at now_ns; step = the distance of one row (window pixels). Adds the
// presses it gives to out[CNAV_*]. Horizontal needs twice the distance (tabs and pages are rarer than rows); the axis
// moved more along wins and the other's rest is dropped; a turn back drops what was gathered the other way.
static inline void cursor_nav_feed(CursorNav *n, float dx, float dy, float step, uint64_t now_ns, int out[CNAV_DIRS]) {
    if (step < 1.0f) step = 1.0f;
    if (now_ns - n->last_ns > CNAV_IDLE_NS) n->ax = n->ay = 0;
    n->last_ns = now_ns;
    if (dx * n->ax < 0) n->ax = 0;
    if (dy * n->ay < 0) n->ay = 0;
    n->ax += dx, n->ay += dy;
    float sx = step * 2.0f;
    if (fabsf(n->ay) >= step && fabsf(n->ay) / step >= fabsf(n->ax) / sx) {
        int k = (int)(fabsf(n->ay) / step);
        if (k > CNAV_MAX_PRESSES) k = CNAV_MAX_PRESSES;
        out[n->ay < 0 ? CNAV_UP : CNAV_DOWN] += k;
        n->ay += n->ay < 0 ? k * step : -k * step;
        n->ax = 0;
    } else if (fabsf(n->ax) >= sx) {
        int k = (int)(fabsf(n->ax) / sx);
        if (k > CNAV_MAX_PRESSES) k = CNAV_MAX_PRESSES;
        out[n->ax < 0 ? CNAV_LEFT : CNAV_RIGHT] += k;
        n->ax += n->ax < 0 ? k * sx : -k * sx;
        n->ay = 0;
    }
}
