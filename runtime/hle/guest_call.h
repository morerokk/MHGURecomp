// Scoped guest calls used by native fusion adapters. guest_call itself saves
// only LR; keep every caller register while returning the guest's r0 separately.
#pragma once
#include "../rt.h"

static inline uint32_t guest_call4(Cpu *c, uint32_t fn, uint32_t a, uint32_t b,
                                  uint32_t d, uint32_t e) {
    uint32_t saved[16];
    memcpy(saved, c->r, sizeof saved);
    c->r[0] = a; c->r[1] = b; c->r[2] = d; c->r[3] = e;
    guest_call(c, fn);
    uint32_t result = c->r[0];
    memcpy(c->r, saved, sizeof saved);
    return result;
}
