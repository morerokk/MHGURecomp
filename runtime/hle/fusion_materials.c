// MHGU 1.4.0 native forge point-material controls. This deliberately does not
// call the forge update/upgrade controllers or their payment/creation helpers.
#include "fusion_materials.h"
#include "../hle.h"
#include <string.h>

enum { MAX_AVAILABLE = 128, CONFIRM = 0x2000, BACK = 0x4000 };
static uint32_t smith;
static bool quantity, released;
static uint32_t visibility;
// Only value models/selectors are copied. No constructed GUI object, allocation
// or ownership link is rewound. The widget bindings already point into smith.
static const struct { uint16_t offset, length; } ranges[] = {
    {0x44, 1}, {0x49, 3}, {0x6c, 8}, {0x84, 12}, {0x1f71, 1},
    {0x1ff4, 0x48}, {0x28d4, 0x30}, {0x2a54, 0x280}, {0x2d50, 0x4c0},
};
static uint8_t saved[0x900];
static uint32_t dirty[4];
static const uint8_t dirty_children[] = {0xa, 0xb, 0xe, 0xf};

static uint32_t call(Cpu *c, uint32_t fn, uint32_t a, uint32_t b, uint32_t d, uint32_t e) {
    uint32_t regs[16]; memcpy(regs, c->r, sizeof regs);
    c->r[0] = a; c->r[1] = b; c->r[2] = d; c->r[3] = e;
    guest_call(c, fn);
    uint32_t result = c->r[0]; memcpy(c->r, regs, sizeof regs);
    return result;
}
static uint32_t child(Cpu *c, unsigned index) { return call(c, 0x85cb3c, smith, index, 0, 0); }
static void visible(Cpu *c, unsigned index, bool show) {
    // Forge and Coordinate widgets have distinct objects but share native
    // renderer resources (Money, Preview, EquipDetail). Never hide a forge
    // widget already hidden: unconditional hide would close the child's
    // renderer resource while another live facility is displaying it.
    call(c, show ? 0x85cba0 : 0x85cc0c, smith, index, 0, 0);
}
static void mark(Cpu *c) {
    for (unsigned i = 2; i < 4; ++i) {
        uint32_t widget = child(c, dirty_children[i]);
        if (widget) ST32(widget + 0x50, LD32(widget + 0x50) | 1);
    }
}
static bool supported(void) {
    const struct { uint32_t address, prologue; } anchors[] = {
        {0x8fcbcc,0xe92d4ff0}, {0x8fa3d8,0xe92d4ff0},
        {0x8f8e48,0xe92d4ff0}, {0x8fb8e4,0xe92d4ff0},
        {0x8fcea8,0xe92d4ff0}, {0x8fd448,0xe92d4bf0},
        {0x8fb720,0xe92d48f0}, {0x74edec,0xe92d4800},
        {0x74ee34,0xe1c011b4}, {0x74ee98,0xe92d4ff0},
        {0x74fa74,0xe92d4010}, {0x394228,0xe590001c},
        {0x85e5bc,0xe92d41f0}, {0x75d2b4,0xe0800101},
        {0x85cb3c,0xe5900034}, {0x85cba0,0xe92d4010}, {0x85cc0c,0xe92d4010},
    };
    for (size_t i = 0; i < sizeof anchors / sizeof anchors[0]; ++i)
        if (LD32(anchors[i].address) != anchors[i].prologue) return false;
    return true;
}
static unsigned current(void) {
    return LD8(smith + 0x2001) + LD8(smith + 0x2002) * LD16(smith + 0x2004);
}
static void refresh(Cpu *c) {
    // This display-only helper has a point-pool branch selected by main7,
    // phase2. In that branch it reads item ownership and writes only the grid
    // model. The synthetic row prevents any fixed-ingredient subtraction.
    uint32_t state = LD32(smith + 4);
    ST32(smith + 4, 0x00021907);
    call(c, 0x8f8e48, smith, 0, 0, 0);
    ST32(smith + 4, state);
    call(c, 0x8fb8e4, smith, LD16(smith + 0x2004), 0, 0);
    call(c, 0x8fcea8, smith, LD16(smith + 0x2028), 0, 0);
    ST16(smith + 0x2e6a, LD32(smith + 0x8c));
    mark(c);
    // Exact native point-pool help lookup from 0x905960..0x9059b0. Keep
    // the existing game message widget visible instead of a host hint.
    unsigned idx = current();
    bool owned = idx < LD32(smith + 0x88) && LD16(smith + 0x2ad6 + idx * 4) > 0;
    uint32_t sp = c->r[13]; c->r[13] -= 16;
    ST32(c->r[13], 0); ST32(c->r[13] + 4, 0);
    call(c, 0x85e5bc, 4, 1, 14, owned ? 0x5e : 0x5f);
    c->r[13] = sp;
    uint32_t facility = LD32(0x1a99458);
    uint32_t common = facility ? call(c, 0x75d2b4, facility, 0, 0, 0) : 0;
    if (common && LD32(common + 0x34) && LD32(common + 0x30) > 4)
        call(c, 0x85cba0, common, 4, 0, 0);
}

void fusion_materials_cancel(Cpu *c, bool restore) {
    if (!smith) return;
    if (restore) {
        for (unsigned i = 0; i < 30; ++i) visible(c, i, false);
        size_t cursor = 0;
        for (size_t i = 0; i < sizeof ranges / sizeof ranges[0]; ++i) {
            memcpy(G2H(smith + ranges[i].offset), saved + cursor, ranges[i].length);
            cursor += ranges[i].length;
        }
        for (unsigned i = 0; i < 4; ++i) {
            uint32_t widget = child(c, dirty_children[i]);
            if (widget) ST32(widget + 0x50, dirty[i] | 1); // redraw restored value model
        }
        for (unsigned i = 0; i < 30; ++i)
            if (visibility & (1u << i)) visible(c, i, true);
    }
    smith = 0; quantity = released = false;
}

bool fusion_materials_open(Cpu *c, uint32_t parent, uint16_t category) {
    if (smith || !parent || !supported() || category < 1 || category > 337 ||
        !LD32(parent + 0x34) || LD32(parent + 0x30) < 30) return false;
    uint32_t item = LD32(0x1a583a8);
    if (!item || !LD32(item + category * 8 + 0x414) ||
        LD32(item + category * 8 + 0x418) > MAX_AVAILABLE) return false;
    smith = parent;
    // Fail before touching parent state if any required resource is absent or
    // its native model binding is incompatible with the supported forge.
    const unsigned required[] = {0xa, 0xb, 0xe, 0xf, 0x10, 0x11};
    for (unsigned i = 0; i < sizeof required / sizeof required[0]; ++i) if (!child(c, required[i])) { smith = 0; return false; }
    if (LD32(child(c, 0xe) + 0x4c) != smith + 0x2e20 ||
        LD32(child(c, 0xf) + 0x4c) != smith + 0x2e20 ||
        LD32(child(c, 0x10) + 0x4c) != smith + 0x1ff4 ||
        LD32(child(c, 0x10) + 0x50) != smith + 0x2d50 ||
        LD32(child(c, 0x11) + 0x50) != smith + 0x3200) { smith = 0; return false; }
    visibility = 0;
    for (unsigned i = 0; i < 30; ++i) {
        uint32_t widget = child(c, i);
        if (widget && LD8(widget + 0x18)) visibility |= 1u << i;
    }
    size_t cursor = 0;
    for (size_t i = 0; i < sizeof ranges / sizeof ranges[0]; ++i) {
        memcpy(saved + cursor, G2H(smith + ranges[i].offset), ranges[i].length);
        cursor += ranges[i].length;
    }
    for (unsigned i = 0; i < 4; ++i) {
        uint32_t widget = child(c, dirty_children[i]);
        dirty[i] = widget ? LD32(widget + 0x50) : 0;
    }
    memset(G2H(smith + 0x28d4), 0, 0x30);
    ST8(smith + 0x1f71, 0); ST8(smith + 0x44, 0); ST8(smith + 0x49, 0);
    ST16(smith + 0x28da, category); ST8(smith + 0x28de, FUSION_POINTS);
    call(c, 0x8fcbcc, smith, 0, 0, 0);
    if (LD32(smith + 0x88) > MAX_AVAILABLE) { fusion_materials_cancel(c, true); return false; }
    call(c, 0x8fa3d8, smith, 0, 0, 0);
    ST8(smith + 0x2e67, FUSION_POINTS);
    call(c, 0x74edec, smith + 0x1ff4, 8, 1, 0);
    call(c, 0x74ee34, smith + 0x1ff4, LD32(smith + 0x88), 3, 0);
    call(c, 0x74ee34, smith + 0x2018, 1, 5, 0);
    for (unsigned i = 0; i < 30; ++i) visible(c, i, false);
    visible(c, 0xe, true); visible(c, 0xf, true); visible(c, 0x10, true);
    quantity = released = false;
    refresh(c);
    LOG("[armor-fusion] native material pool %x category %u available %u required 10\n",
        smith, category, LD32(smith + 0x88));
    return true;
}

static bool selection(FusionSelection *out, size_t capacity, size_t *count) {
    uint32_t n = LD32(smith + 0x84);
    if (!out || !count || !n || n > FUSION_MAX_MATERIALS || n > capacity) return false;
    for (unsigned i = 0; i < n; ++i) {
        uint32_t row = smith + 0x2a54 + 4 * i;
        uint16_t id = LD16(row), qty = LD16(row + 2);
        if (!id || !qty || qty > FUSION_POINTS) return false;
        for (unsigned j = 0; j < i; ++j) if (out[j].item_id == id) return false;
        out[i] = (FusionSelection){id, qty};
    }
    *count = n;
    return true;
}

int fusion_materials_poll(Cpu *c, FusionSelection *out, size_t capacity, size_t *count) {
    if (count) *count = 0;
    if (out) memset(out, 0, sizeof *out * capacity);
    if (!smith) return 2;
    if (LD32(smith + 0x84) > FUSION_MAX_MATERIALS || LD32(smith + 0x88) > MAX_AVAILABLE) {
        fusion_materials_cancel(c, true); return 2;
    }
    uint32_t input = LD32(0x1a8c670);
    if (!input) return 0;
    uint32_t pressed = LD32(input + 0x6bfc), held = pressed | LD32(input + 0x6c34);
    if (!released) { if (!(held & (CONFIRM | BACK))) released = true; return 0; }
    ST32(smith + 0x6c, pressed); ST32(smith + 0x70, held);
    if (quantity) {
        uint32_t sp = c->r[13]; c->r[13] -= 16; ST32(c->r[13], smith + 0x320c);
        unsigned result = call(c, 0x74fa74, smith + 0x3202, LD16(smith + 0x3204), pressed, held);
        c->r[13] = sp;
        if (!result) return 0;
        quantity = false; released = false;
        visible(c, 0x11, false); visible(c, 0xf, true);
        if (result == 1) {
            unsigned idx = current(), n = LD32(smith + 0x84), qty = LD16(smith + 0x3202);
            if (idx >= LD32(smith + 0x88) || n > FUSION_MAX_MATERIALS || !qty || qty > 10) return 0;
            uint32_t row = smith + 0x2ad4 + idx * 4; uint16_t id = LD16(row);
            if (!id || qty > LD16(row + 2)) return 0;
            unsigned merged = qty; bool exists = false;
            for (unsigned i = 0; i < n; ++i) if (LD16(smith + 0x2a54 + i * 4) == id) {
                exists = true; merged += LD16(smith + 0x2a56 + i * 4);
            }
            if (merged > 10 || (!exists && n >= FUSION_MAX_MATERIALS)) return 0;
            call(c, 0x8fd448, smith, 0, 0, 0);
            refresh(c);
            if (LD32(smith + 0x8c) >= FUSION_POINTS) {
                bool valid = selection(out, capacity, count);
                fusion_materials_cancel(c, true);
                if (!valid) { if (out) memset(out, 0, sizeof *out * capacity); if (count) *count = 0; return 2; }
                return 1;
            }
        }
        refresh(c);
        return 0;
    }
    if (pressed & BACK) {
        if (LD32(smith + 0x84)) {
            call(c, 0x8fb720, smith, 0, 0, 0);
            refresh(c); released = false; return 0;
        }
        fusion_materials_cancel(c, true); return 2;
    }
    if (pressed & CONFIRM) {
        unsigned idx = current();
        if (idx >= LD32(smith + 0x88) || LD32(smith + 0x8c) >= FUSION_POINTS) return 0;
        uint32_t row = smith + 0x2ad4 + idx * 4;
        uint16_t id = LD16(row), owned = LD16(row + 2);
        uint32_t info = id ? call(c, 0x394228, LD32(0x1a583a8), id, 0, 0) : 0;
        unsigned points = info ? LD8(info + 0x24) : 0;
        if (!points || !owned) return 0;
        unsigned max = (FUSION_POINTS - LD32(smith + 0x8c) + points - 1) / points;
        if (max > owned) max = owned;
        for (unsigned i = 0; i < LD32(smith + 0x84); ++i)
            if (LD16(smith + 0x2a54 + i * 4) == id) {
                unsigned previous = LD16(smith + 0x2a56 + i * 4);
                if (previous >= 10) return 0;
                if (max > 10 - previous) max = 10 - previous;
            }
        if (!max) return 0;
        ST32(smith + 0x320c, 0); ST32(smith + 0x3200, 0); ST16(smith + 0x3202, 1);
        ST16(smith + 0x3204, max); ST32(smith + 0x3208, points);
        visible(c, 0xf, false); visible(c, 0x11, true);
        quantity = true; released = false; return 0;
    }
    // Native list paging uses L/R while its grid uses directional navigation.
    if (call(c, 0x74ee98, smith + 0x2018, held, 0, 0)) { refresh(c); return 0; }
    if (call(c, 0x74ee98, smith + 0x1ff4, held, 0, 0)) refresh(c);
    return 0;
}
