// Native MHGU 1.4.0 fusion UI adapter. The game's confirmation and equipped
// armor handling remain responsible for the equipment mutation. The host
// intercepts confirmation BEFORE the native zenny charge and substitutes the
// selected materials at the native token deduction call site.
#include "../armor_fusion.h"
#include <stdio.h>
#include "../hle.h"
#include <stdlib.h>
#include <string.h>

#define FN_UPDATE       0x860314u
#define FN_CLOSE        0x86549cu
#define FN_VALID        0x340b7cu
#define FN_PAIR         0x340d60u
#define FN_QUOTE        0x341060u
#define FN_CONFIRM      0x74fd94u
#define FN_CONSUME      0x74ff80u
#define FN_COUNT        0x74fe8cu
#define FN_ZENNY_CHECK  0x723180u
#define FN_BOX_RECORD   0x343894u
#define G_EQUIP_BOX     0x1a56fe8u
#define G_PLAYER_DATA   0x1a97f78u
#define FN_SMITH_UPDATE 0x8f1ae8u
#define FN_ALLOCATE     0x20f248u
#define FN_CONSTRUCT    0x85fcecu
#define FN_REGISTER     0x5ea218u
#define G_TASK_SYSTEM   0x2326f48u
#define FN_DELETE       0x85fe10u
#define FN_HIDE_CHILD   0x85cbc4u
#define FN_SHOW_CHILD   0x85cba0u
#define FN_SMITH_CLOSE  0x8f0c70u
#define FN_SMITH_DTOR   0x8f02fcu
#define FN_SMITH_DELETE 0x8f085cu
#define G_INPUT         0x1a8c670u
#define DTI_COORDINATE  0x1a4d230u
#define VT_DTI_COORDINATE 0x1930844u
#define FN_MESSAGE      0x75bc7cu
#define FN_DIALOG_TEXT  0x75bac0u
#define G_FACILITY      0x1a99458u
#define FN_DRAW_VIEW    0xa88238u

enum { COORDINATE_KEEP = (1u << 0) | (1u << 1) | (1u << 4) | (1u << 10) };

static uint32_t active_ui, context_ui;
static enum { IDLE, MATERIALS, RELEASE_CONFIRM, CONFIRM, APPROVED } phase;
static FusionSelection selected[FUSION_MAX_MATERIALS];
static size_t selected_count;
static uint32_t opened_zenny;
static uint32_t coordinate_visibility, common_picker_ui, common_picker_visibility;
static FusionPlan approved_plan;
static uint32_t fusion_label;
static uint32_t fusion_question;
static uint32_t smith_context;
static FusionPiece opened_base, opened_appearance;
static bool trace;
static uint32_t parent_facility;
static uint8_t parent_facility_flags[2];
static bool open_latched;
static uint32_t parent_ui;
static uint32_t visible_children;
static uint16_t preview_views[2];
static bool aborting;

static uint32_t visible_mask(uint32_t ui) {
    uint32_t mask = 0, children = LD32(ui + 0x34), count = LD32(ui + 0x30);
    for (unsigned i = 0; children && i < count && i < 30; ++i) {
        uint32_t child = LD32(children + 4 * i);
        if (child && LD8(child + 0x18)) mask |= 1u << i;
    }
    return mask;
}

static void apply_mask(Cpu *c, uint32_t ui, uint32_t mask, uint32_t fn) {
    for (unsigned i = 0; i < 30; ++i)
        if (mask & (1u << i)) guest_call4(c, fn, ui, i, 0, 0);
}

// Native point-material widgets borrow only the forge's value models; the
// original upgrade/payment controllers are never entered.
enum { MAX_AVAILABLE = 128, CONFIRM_BUTTON = 0x2000, BACK = 0x4000 };
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

static uint32_t child(Cpu *c, unsigned index) { return guest_call4(c, 0x85cb3c, smith, index, 0, 0); }
static void visible(Cpu *c, unsigned index, bool show) {
    // Forge and Coordinate widgets have distinct objects but share native
    // renderer resources (Money, Preview, EquipDetail). Never hide a forge
    // widget already hidden: unconditional hide would close the child's
    // renderer resource while another live facility is displaying it.
    guest_call4(c, show ? 0x85cba0 : 0x85cc0c, smith, index, 0, 0);
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
    guest_call4(c, 0x8f8e48, smith, 0, 0, 0);
    ST32(smith + 4, state);
    guest_call4(c, 0x8fb8e4, smith, LD16(smith + 0x2004), 0, 0);
    guest_call4(c, 0x8fcea8, smith, LD16(smith + 0x2028), 0, 0);
    ST16(smith + 0x2e6a, LD32(smith + 0x8c));
    mark(c);
    // Exact native point-pool help lookup from 0x905960..0x9059b0. Keep
    // the existing game message widget visible instead of a host hint.
    unsigned idx = current();
    bool owned = idx < LD32(smith + 0x88) && LD16(smith + 0x2ad6 + idx * 4) > 0;
    uint32_t sp = c->r[13]; c->r[13] -= 16;
    ST32(c->r[13], 0); ST32(c->r[13] + 4, 0);
    guest_call4(c, 0x85e5bc, 4, 1, 14, owned ? 0x5e : 0x5f);
    c->r[13] = sp;
    uint32_t facility = LD32(0x1a99458);
    uint32_t common = facility ? guest_call4(c, 0x75d2b4, facility, 0, 0, 0) : 0;
    if (common && LD32(common + 0x34) && LD32(common + 0x30) > 4)
        guest_call4(c, 0x85cba0, common, 4, 0, 0);
}

static void materials_cancel(Cpu *c, bool restore) {
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
        apply_mask(c, smith, visibility, FN_SHOW_CHILD);
    }
    smith = 0; quantity = released = false;
}

static bool materials_open(Cpu *c, uint32_t parent, uint16_t category) {
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
    visibility = visible_mask(smith);
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
    guest_call4(c, 0x8fcbcc, smith, 0, 0, 0);
    if (LD32(smith + 0x88) > MAX_AVAILABLE) { materials_cancel(c, true); return false; }
    guest_call4(c, 0x8fa3d8, smith, 0, 0, 0);
    ST8(smith + 0x2e67, FUSION_POINTS);
    guest_call4(c, 0x74edec, smith + 0x1ff4, 8, 1, 0);
    guest_call4(c, 0x74ee34, smith + 0x1ff4, LD32(smith + 0x88), 3, 0);
    guest_call4(c, 0x74ee34, smith + 0x2018, 1, 5, 0);
    for (unsigned i = 0; i < 30; ++i) visible(c, i, false);
    visible(c, 0xe, true); visible(c, 0xf, true); visible(c, 0x10, true);
    quantity = released = false;
    refresh(c);
    LOG("[armor-fusion] native material pool %x category %u available %u required 10\n",
        smith, category, LD32(smith + 0x88));
    return true;
}

static bool material_selection(void) {
    uint32_t n = LD32(smith + 0x84);
    if (!n || n > FUSION_MAX_MATERIALS) return false;
    for (unsigned i = 0; i < n; ++i) {
        uint32_t row = smith + 0x2a54 + 4 * i;
        uint16_t id = LD16(row), qty = LD16(row + 2);
        if (!id || !qty || qty > FUSION_POINTS) return false;
        for (unsigned j = 0; j < i; ++j) if (selected[j].item_id == id) return false;
        selected[i] = (FusionSelection){id, qty};
    }
    selected_count = n;
    return true;
}

static int materials_poll(Cpu *c) {
    selected_count = 0;
    memset(selected, 0, sizeof selected);
    if (!smith) return 2;
    if (LD32(smith + 0x84) > FUSION_MAX_MATERIALS || LD32(smith + 0x88) > MAX_AVAILABLE) {
        materials_cancel(c, true); return 2;
    }
    uint32_t input = LD32(0x1a8c670);
    if (!input) return 0;
    uint32_t pressed = LD32(input + 0x6bfc), held = pressed | LD32(input + 0x6c34);
    if (!released) { if (!(held & (CONFIRM_BUTTON | BACK))) released = true; return 0; }
    ST32(smith + 0x6c, pressed); ST32(smith + 0x70, held);
    if (quantity) {
        uint32_t sp = c->r[13]; c->r[13] -= 16; ST32(c->r[13], smith + 0x320c);
        unsigned result = guest_call4(c, 0x74fa74, smith + 0x3202, LD16(smith + 0x3204), pressed, held);
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
            guest_call4(c, 0x8fd448, smith, 0, 0, 0);
            refresh(c);
            if (LD32(smith + 0x8c) >= FUSION_POINTS) {
                bool valid = material_selection();
                materials_cancel(c, true);
                if (!valid) { memset(selected, 0, sizeof selected); selected_count = 0; return 2; }
                return 1;
            }
            return 0;
        }
        refresh(c);
        return 0;
    }
    if (pressed & BACK) {
        if (LD32(smith + 0x84)) {
            guest_call4(c, 0x8fb720, smith, 0, 0, 0);
            refresh(c); released = false; return 0;
        }
        materials_cancel(c, true); return 2;
    }
    if (pressed & CONFIRM_BUTTON) {
        unsigned idx = current();
        if (idx >= LD32(smith + 0x88) || LD32(smith + 0x8c) >= FUSION_POINTS) return 0;
        uint32_t row = smith + 0x2ad4 + idx * 4;
        uint16_t id = LD16(row), owned = LD16(row + 2);
        uint32_t info = id ? guest_call4(c, 0x394228, LD32(0x1a583a8), id, 0, 0) : 0;
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
    if (guest_call4(c, 0x74ee98, smith + 0x2018, held, 0, 0)) { refresh(c); return 0; }
    if (guest_call4(c, 0x74ee98, smith + 0x1ff4, held, 0, 0)) refresh(c);
    return 0;
}

static void preview_lighting(Cpu *c, bool hide) {
    // Each preview owns ambient/directional lights for the shared paper model.
    // Leaving Smith's lights active requests an unsupported two-light shader.
    uint32_t preview = LD32(parent_ui + 0x3210);
    for (unsigned i = 0; preview && i < 2; ++i) {
        uint32_t light = LD32(preview + 8 + 4 * i);
        if (!light) continue;
        if (hide) preview_views[i] = LD16(light + 0xe) & 0x3ff;
        guest_call4(c, FN_DRAW_VIEW, light, hide ? 0 : preview_views[i], 0, 0);
    }
}

static void suspend_parent(Cpu *c, uint32_t ui) {
    parent_ui = ui;
    visible_children = visible_mask(ui);
    apply_mask(c, ui, visible_children, FN_HIDE_CHILD);
    preview_lighting(c, true);
}

static bool parent_live(void) {
    if (!parent_ui || aborting || !parent_facility || LD32(G_FACILITY) != parent_facility ||
        LD32(parent_facility + 0x15c) != parent_ui) return false;
    unsigned state = LD32(parent_ui + 0xc) & 7;
    return state == 1 || state == 2;
}

static void restore_parent(Cpu *c) {
    if (parent_live()) {
        preview_lighting(c, false);
        apply_mask(c, parent_ui, visible_children, FN_SHOW_CHILD);
        ST8(parent_ui + 4, 0); // Native main-menu setup restores its hints/title.
    }
    parent_ui = visible_children = 0;
}

static bool scope(void) {
    // +0x44 mode 0 is the hunter domain. Material interception additionally
    // requires the fusion-specific confirmation/consume return addresses;
    // separation keeps its original native costs and transaction.
    return context_ui && context_ui == active_ui && LD32(context_ui + 0x44) == 0;
}

static FusionPiece decode_piece(uint32_t record, uint32_t box_index) {
    unsigned flags = LD16(record + 4);
    return (FusionPiece){ .box_index = box_index, .armor_id = LD16(record + 6), .slot = flags & 31,
        .level = ((flags >> 5) & 31) + 1 };
}

static bool pieces(Cpu *c, uint32_t ui, FusionPiece *base, FusionPiece *appearance,
                   uint32_t *base_record, uint32_t *appearance_record) {
    uint32_t box = LD32(G_EQUIP_BOX);
    if (!box || LD32(ui + 0x6c) != 0) return false;
    uint32_t base_index = LD32(ui + 0x84), appearance_index = LD32(ui + 0x88);
    if (base_index == appearance_index || base_index >= 2000 || appearance_index >= 2000) return false;
    *base_record = guest_call4(c, FN_BOX_RECORD, box, 0, base_index, 0);
    *appearance_record = guest_call4(c, FN_BOX_RECORD, box, 0, appearance_index, 0);
    if (!*base_record || !*appearance_record) return false;
    *base = decode_piece(*base_record, base_index);
    *appearance = decode_piece(*appearance_record, appearance_index);
    return base->slot >= 1 && base->slot <= 5 && appearance->slot >= 1 && appearance->slot <= 5;
}

static uint32_t inventory(void *context, uint16_t id) {
    return guest_call4(context, FN_COUNT, id, 0, 0, 0);
}

static uint32_t native_pair(Cpu *c, uint32_t ui, const FusionPiece *base,
                            const FusionPiece *appearance, uint32_t b, uint32_t a) {
    // The fifth argument belongs to our own aligned guest stack frame.
    uint32_t sp = c->r[13];
    c->r[13] -= 16;
    ST32(c->r[13], appearance->box_index);
    uint32_t result = guest_call4(c, FN_PAIR, ui + 0x80, b, a, base->box_index);
    c->r[13] = sp;
    return result;
}

static bool snapshot(Cpu *c, uint32_t ui, FusionPiece *base, FusionPiece *appearance,
                     FusionRecipe *recipe, uint32_t *fee, uint32_t *native) {
    uint32_t b, a;
    memset(base, 0, sizeof *base); memset(appearance, 0, sizeof *appearance);
    memset(recipe, 0, sizeof *recipe); *fee = 0;
    if (!pieces(c, ui, base, appearance, &b, &a) || !fusion_recipe(base, appearance, recipe)) return false;
    uint32_t sp = c->r[13];
    c->r[13] -= 16;
    ST32(c->r[13], c->r[13] + 8);
    ST32(c->r[13] + 4, 0); ST32(c->r[13] + 8, 0);
    guest_call4(c, FN_QUOTE, ui + 0x80, b, a, c->r[13] + 4);
    *fee = LD32(c->r[13] + 4);
    c->r[13] = sp;
    *native = native_pair(c, ui, base, appearance, b, a);
    return base->rank != FUSION_RANK_UNKNOWN && base->rank == appearance->rank && *native == 1;
}

static void validity(Cpu *c) {
    uint32_t record = c->r[1];
    fnhook_call_original(c, FN_VALID);
    if (scope() && record) {
        FusionPiece piece = decode_piece(record, 0);
        // Native 0x340bd8..bfc sets bit 0x08 only for the category unlock.
        // With hunter slot 1..5, helper 0x2daeb8 necessarily returns true;
        // therefore bit 0x10 can only come from the special-armor progress
        // flag lookup at 0x340c84..ca0, including the late Deviant unlock.
        // Clear these progression gates only for a catalog-recognized owned
        // hunter piece. Empty/type/sex/already-fused/decorations bits remain.
        if (piece.slot >= 1 && piece.slot <= 5 && fusion_classify(&piece) &&
            piece.rank >= FUSION_LR && piece.rank <= FUSION_G) c->r[0] &= ~0x18u;
    }
}

static void pair(Cpu *c) {
    uint32_t b = c->r[1], a = c->r[2];
    fnhook_call_original(c, FN_PAIR);
    if (!scope() || !b || !a) return;
    FusionPiece base = decode_piece(b, 0), appearance = decode_piece(a, 0);
    FusionRecipe recipe;
    if (!fusion_recipe(&base, &appearance, &recipe) || base.rank == FUSION_RANK_UNKNOWN ||
        base.rank != appearance.rank) c->r[0] |= 0x400;
}

static void quote(Cpu *c) {
    uint32_t output = LD32(c->r[13]);
    fnhook_call_original(c, FN_QUOTE);
    // Native confirmation no longer asks for a fixed fusion token. Its zenny
    // fee is preserved; the separate material chooser shows the actual recipe.
    if (scope() && output) { ST16(output, 0); ST16(output + 2, 0); }
}

static void picker_visibility(Cpu *c, bool hide) {
    if (hide) {
        // Coordinate keeps the header, money, hunter type and preview frame.
        // Smith uses conditional hide (0x85cc0c), not this facility hide.
        coordinate_visibility = visible_mask(active_ui);
        apply_mask(c, active_ui, coordinate_visibility & ~COORDINATE_KEEP, FN_HIDE_CHILD);
        // Shared native message/Yes-No controls are owned by the common
        // facility, independently of Coordinate. Preserve both visibility
        // states; the pool uses message4 for native material instructions.
        common_picker_ui = guest_call4(c, 0x75d2b4, parent_facility, 0, 0, 0);
        common_picker_visibility = common_picker_ui ? visible_mask(common_picker_ui) & 0x30u : 0;
        if (common_picker_ui && LD32(common_picker_ui + 0x34) && LD32(common_picker_ui + 0x30) > 5)
            guest_call4(c, FN_HIDE_CHILD, common_picker_ui, 5, 0, 0);
    } else {
        apply_mask(c, active_ui, coordinate_visibility, FN_SHOW_CHILD);
        // Never restore cached GUI addresses after a parent/scene teardown.
        if (parent_live() && common_picker_ui &&
            guest_call4(c, 0x75d2b4, parent_facility, 0, 0, 0) == common_picker_ui) {
            for (unsigned i = 4; i <= 5; ++i)
                guest_call4(c, common_picker_visibility & (1u << i) ? FN_SHOW_CHILD : FN_HIDE_CHILD,
                    common_picker_ui, i, 0, 0);
        }
        coordinate_visibility = common_picker_ui = common_picker_visibility = 0;
    }
}

static bool final_question(Cpu *c) {
    uint32_t common = guest_call4(c, 0x75d2b4, parent_facility, 0, 0, 0);
    if (!common || !LD32(common + 0x34) || LD32(common + 0x30) <= 5) return false;
    uint32_t children = LD32(common + 0x34);
    if (!LD32(children + 4 * 4) || !LD32(children + 5 * 4)) return false;
    // Re-open the original MHGU question/Yes-No controls. Formatting flag1
    // invalidates the cached string even when the message ID is unchanged;
    // our scoped getter supplies the literal, freshly quoted fusion fee.
    uint32_t sp = c->r[13]; c->r[13] -= 32;
    ST32(c->r[13], 0); ST32(c->r[13] + 4, 1);
    ST32(c->r[13] + 8, 0); ST32(c->r[13] + 12, 0);
    guest_call4(c, 0x85e5bc, 0x14, 1, 37, 43);
    c->r[13] = sp;
    guest_call4(c, 0x85e794, 2, 0x20, 0, 0);
    ST8(context_ui + 0x62, 0);
    guest_call4(c, FN_SHOW_CHILD, common, 4, 0, 0);
    guest_call4(c, FN_SHOW_CHILD, common, 5, 0, 0);
    return true;
}

static void confirmation(Cpu *c) {
    if (!scope() || c->r[14] != 0x860e7c) { fnhook_call_original(c, FN_CONFIRM); return; }
    if (!parent_live()) {
        materials_cancel(c, false);
        phase = IDLE; c->r[0] = 2; return;
    }
    if (phase == MATERIALS) {
        int result = materials_poll(c);
        c->r[0] = 0;
        if (!result) return;
        phase = IDLE;
        picker_visibility(c, false);
        if (result == 2) { c->r[0] = 2; return; }
        phase = RELEASE_CONFIRM;
        if (!final_question(c)) { phase = IDLE; c->r[0] = 2; }
        return;
    }
    if (phase == RELEASE_CONFIRM) {
        uint32_t input = LD32(G_INPUT);
        uint32_t held = input ? LD32(input + 0x6bfc) | LD32(input + 0x6c34) : 0;
        // Consume the release update itself; only the following update may
        // reach the native Yes/No handler.
        if (!(held & 0x6000)) phase = CONFIRM;
        c->r[0] = 0; return;
    }
    if (phase == CONFIRM) {
        fnhook_call_original(c, FN_CONFIRM);
        if (c->r[0] == 2) { phase = IDLE; return; }
        if (c->r[0] != 1) return;
        phase = IDLE;
        FusionPiece base, appearance; FusionRecipe recipe; uint32_t fee, native;
        if (!snapshot(c, context_ui, &base, &appearance, &recipe, &fee, &native) ||
            base.box_index != opened_base.box_index || base.armor_id != opened_base.armor_id ||
            base.level != opened_base.level ||
            appearance.box_index != opened_appearance.box_index || appearance.armor_id != opened_appearance.armor_id ||
            appearance.level != opened_appearance.level ||
            fee != opened_zenny ||
            !guest_call4(c, FN_ZENNY_CHECK, LD32(G_PLAYER_DATA), fee, 0, 0) ||
            !fusion_plan(&base, &appearance, native, &recipe, selected, selected_count,
                inventory, c, &approved_plan)) { c->r[0] = 2; return; }
        phase = APPROVED;
        ST8(context_ui + 0x62, 0);
        ST32(context_ui + 0x48, fee);
        if (trace) LOG("[armor-fusion] approved base %u/%u Lv%u appearance %u/%u Lv%u: %u points, %zu materials, %uz\n",
            base.slot, base.armor_id, base.level, appearance.slot, appearance.armor_id, appearance.level,
            approved_plan.points, approved_plan.count, fee);
        c->r[0] = 1;
        return;
    }
    // The first arrival in the native cost phase opens material selection.
    // Only the later native Yes/No can authorize payment.
    c->r[0] = 0;
    FusionPiece base, appearance; FusionRecipe recipe; uint32_t fee, native;
    if (snapshot(c, context_ui, &base, &appearance, &recipe, &fee, &native)) {
        opened_base = base; opened_appearance = appearance; opened_zenny = fee;
        snprintf(G2H(fusion_question), 96,
            "Fuse these pieces of armor for %uz?\r\nUse the selected monster materials.", fee);
        picker_visibility(c, true);
        phase = materials_open(c, parent_ui, recipe.category_id) ? MATERIALS : IDLE;
        if (phase == MATERIALS) {
            // Smith and Coordinate Money/Preview objects share renderer
            // resources. Reassert the preserved Coordinate panels after the
            // point UI has finished switching the parent's native widgets.
            apply_mask(c, active_ui, coordinate_visibility & COORDINATE_KEEP, FN_SHOW_CHILD);
        } else picker_visibility(c, false);
        if (trace) LOG("[armor-fusion] native material chooser %s for armor %u Lv%u category %u\n",
            phase == MATERIALS ? "opened" : "rejected", appearance.armor_id, appearance.level, recipe.category_id);
        if (phase != MATERIALS) c->r[0] = 2;
    } else c->r[0] = 2;
}

static void consume(Cpu *c) {
    if (!scope() || c->r[14] != 0x860eb0) { fnhook_call_original(c, FN_CONSUME); return; }
    // Confirmation and this call execute synchronously on the guest game
    // thread. Every deduction was checked immediately before the native fee.
    if (phase != APPROVED) FATAL("armor fusion: token deduction without approved plan\n");
    for (size_t i = 0; i < approved_plan.count; ++i)
        guest_call4(c, FN_CONSUME, approved_plan.deductions[i].item_id, approved_plan.deductions[i].quantity, 0, 0);
    if (trace) LOG("[armor-fusion] selected materials consumed; continuing native equipment commit\n");
    phase = IDLE;
    c->r[0] = 0;
}

static void update(Cpu *c) {
    static uint32_t trace_ui, trace_state, trace_mode;
    uint32_t ui = c->r[0], previous = context_ui;
    context_ui = ui;
    fnhook_call_original(c, FN_UPDATE);
    if (trace && ui == active_ui && (trace_ui != ui || trace_state != LD32(ui + 4) || trace_mode != LD32(ui + 0x44))) {
        trace_ui = ui; trace_state = LD32(ui + 4); trace_mode = LD32(ui + 0x44);
        LOG("[armor-fusion] native UI %x state %x mode %u\n", ui, trace_state, trace_mode);
    }
    if (ui == active_ui && LD8(ui + 4) == 1) {
        // The first two native options are hunter fusion and separation.
        if (LD8(ui + 0x156) != 2 || LD16(ui + 0x15c) != 2) {
            ST16(ui + 0x158, 0);
            if (LD8(ui + 0x155) >= 2) ST8(ui + 0x155, 1);
        }
        ST8(ui + 0x156, 2);
        ST16(ui + 0x15a, 1);
        ST16(ui + 0x15c, 2);
        ST32(ui + 0x120, 0); ST32(ui + 0x128, 0);
    }
    context_ui = previous;
}

static void smith_update(Cpu *c) {
    uint32_t ui = c->r[0];
    uint32_t input = LD32(G_INPUT);
    bool confirm = input && (LD32(input + 0x6bfc) & 0x2000);
    if (!confirm) open_latched = false;
    if (active_ui) {
        // The native task system owns the child facility's entire lifecycle.
        // Only the suspended forge's update waits for that child to close.
        return;
    }
    unsigned row = LD8(ui + 0x1f71) + LD8(ui + 0x1f72) * LD16(ui + 0x1f74);
    if (LD8(ui + 4) == 1 && row == 4 && confirm) {
        if (open_latched) return;
        open_latched = true;
        if (LD32(DTI_COORDINATE) != VT_DTI_COORDINATE || LD32(VT_DTI_COORDINATE + 8) != 0x20f210u) {
            LOG("[armor-fusion] native coordinate DTI does not match; refusing factory call\n");
            return;
        }
        parent_facility = LD32(G_FACILITY);
        if (parent_facility) {
            parent_facility_flags[0] = LD8(parent_facility + 0x198);
            parent_facility_flags[1] = LD8(parent_facility + 0x199);
        }
        suspend_parent(c, ui);
        uint32_t tasks = LD32(G_TASK_SYSTEM);
        active_ui = tasks ? guest_call4(c, FN_ALLOCATE, 0x200, 0x10, 0, 0) : 0;
        if (active_ui) {
            guest_call4(c, FN_CONSTRUCT, active_ui, 0, 0, 0);
            // Exact original Lab chain at 0x75e7e0..0x75e830: task order
            // 0x18, two zero setup words, immediate-registration flag true.
            // Do not overwrite cFacility +0x15c: it still owns the parent.
            uint32_t sp = c->r[13];
            c->r[13] -= 16;
            ST32(c->r[13], 1); ST32(c->r[13] + 4, 0); ST32(c->r[13] + 8, 0);
            uint32_t registered = guest_call4(c, FN_REGISTER, tasks, active_ui, 0x18, c->r[13] + 4);
            c->r[13] = sp;
            if (!registered) {
                uint32_t failed = active_ui;
                guest_call4(c, FN_CLOSE, failed, 0, 0, 0);
                guest_call4(c, FN_DELETE, failed, 0, 0, 0);
            }
        } else restore_parent(c);
        if (trace) LOG("[armor-fusion] forge %x opened native fusion UI %x\n", ui, active_ui);
        return;
    }
    smith_context = ui;
    fnhook_call_original(c, FN_SMITH_UPDATE);
    smith_context = 0;
    if (LD8(ui + 4) == 1) {
        if (trace && LD16(ui + 0x1f78) != 5) LOG("[armor-fusion] added forge menu row at %x\n", ui);
        if (LD8(ui + 0x1f72) != 5 || LD16(ui + 0x1f78) != 5) ST16(ui + 0x1f74, 0);
        ST8(ui + 0x1f72, 5);
        ST16(ui + 0x1f76, 1);
        ST16(ui + 0x1f78, 5);
        // The native list model has layouts for 4/7/5/2 rows (types 7/8/9/10).
        // Use its existing five-row layout alongside the five-row selector.
        ST16(ui + 0x2d14, 9);
        ST32(ui + 0x2d38, 1);
        ST32(ui + 0x2d3c, fusion_label);
    }
}

static void dialog_text(Cpu *c) {
    // 0x85e5bc -> 0x8429a4 -> 0x75bac0(category, group, index).
    // The normal cost branch in 0x8650f0 resolves table 0x183e554 row 30:
    // {group 37, message 43, format flags 6}. Live native message-widget
    // +0x54/+0x56/+0x58 confirms {category 1, group 37, message 43}; its
    // GMD string is "Offer %2d Fusion Material(s) and %dz...".
    // Message 45 is the unrelated "Equip now?" and must remain untouched.
    // GUI lookup may run after the facility update, so use the owned active
    // hunter-fusion UI and the exact native dialog lookup caller.
    if (active_ui && LD8(active_ui + 4) == 2 && LD32(active_ui + 0x44) == 0 &&
        c->r[14] == 0x842a48 && c->r[1] == 1 && c->r[2] == 37 && c->r[3] == 43) {
        static uint32_t traced_ui;
        if (trace && traced_ui != active_ui) {
            traced_ui = active_ui;
            LOG("[armor-fusion] replaced native cost question 1/37/43 for UI %x\n", active_ui);
        }
        c->r[0] = fusion_question;
        return;
    }
    fnhook_call_original(c, FN_DIALOG_TEXT);
}

static void message(Cpu *c) {
    // The shared smithy help table continues after its four main entries.
    // Index four is {62,0}: a valid forge hint, but unrelated to fusion. Replace
    // precisely that lookup while the new main-menu row is highlighted.
    if (smith_context && LD8(smith_context + 4) == 1 && c->r[1] == 14 && c->r[2] == 62 &&
        LD8(smith_context + 0x1f71) == 4 && LD16(smith_context + 0x1f74) == 0) {
        c->r[0] = fusion_label;
        return;
    }
    fnhook_call_original(c, FN_MESSAGE);
}

static void close_ui(Cpu *c) {
    bool own = c->r[0] == active_ui;
    if (own) {
        if (trace) LOG("[armor-fusion] native menu %x closed at caller %x, state %x, flags %x\n",
            active_ui, c->r[14], LD32(active_ui + 4), LD32(active_ui + 0xc));
        bool restore = parent_live();
        materials_cancel(c, restore);
        // A normal native close can interrupt the pool before poll returns.
        // Restore its shared controls while both owners are still live;
        // teardown/scene changes must only discard cached guest addresses.
        if (restore && (coordinate_visibility || common_picker_ui)) picker_visibility(c, false);
        active_ui = 0; phase = IDLE;
        coordinate_visibility = common_picker_ui = common_picker_visibility = 0; selected_count = 0;
    }
    fnhook_call_original(c, FN_CLOSE);
    // uUIFacility closes its global menu flag. Restore the suspended forge's
    // flags when returning to that parent, provided the scene is unchanged.
    if (own && parent_live()) {
        ST8(parent_facility + 0x198, parent_facility_flags[0]);
        ST8(parent_facility + 0x199, parent_facility_flags[1]);
    }
    if (own) restore_parent(c);
    if (own) parent_facility = 0;
}

static void abort_child(Cpu *c) {
    if (parent_ui != c->r[0]) return;
    aborting = true;
    if (active_ui) guest_call4(c, FN_CLOSE, active_ui, 0, 0, 0);
    aborting = false;
}

static void smith_close(Cpu *c) { abort_child(c); fnhook_call_original(c, FN_SMITH_CLOSE); }
static void smith_dtor(Cpu *c) { abort_child(c); fnhook_call_original(c, FN_SMITH_DTOR); }
static void smith_delete(Cpu *c) { abort_child(c); fnhook_call_original(c, FN_SMITH_DELETE); }

void armorfusion_init(void) {
    trace = getenv("MHGU_FUSION_TRACE") != NULL;
    const struct { uint32_t addr, word; } anchors[] = {
        {FN_SMITH_UPDATE, 0xe92d41f0}, {FN_UPDATE, 0xe92d4830},
        {FN_VALID, 0xe92d4070}, {FN_PAIR, 0xe92d4ff0},
        {FN_QUOTE, 0xe92d41f0}, {FN_CONSUME, 0xe92d4070},
        {FN_CONFIRM, 0xe92d4800},
        {FN_CONSTRUCT, 0xe92d4010}, {FN_REGISTER, 0xe92d4010},
        {FN_ALLOCATE, 0xe92d4070}, {FN_DELETE, 0xe92d4010},
        {FN_SMITH_CLOSE, 0xe92d4800}, {FN_SMITH_DTOR, 0xe92d4830}, {FN_SMITH_DELETE, 0xe92d4800},
        {FN_DIALOG_TEXT, 0xe92d4070},
        {0x75d2b4, 0xe0800101}, {0x85e5bc, 0xe92d41f0}, {0x85e794, 0xe92d4830},
        {FN_DRAW_VIEW, 0xe590200cu},
    };
    for (size_t i = 0; i < sizeof anchors / sizeof anchors[0]; ++i) {
        if (LD32(anchors[i].addr) != anchors[i].word) {
            LOG("[armor-fusion] unsupported or patched guest function %x; hooks not installed\n", anchors[i].addr);
            return;
        }
    }
    fusion_label = hle_alloc(32);
    memcpy(G2H(fusion_label), "Armor Fusion", sizeof "Armor Fusion");
    fusion_question = hle_alloc(96);
    memcpy(G2H(fusion_question), "Choose monster materials to fuse these armor pieces?",
        sizeof "Choose monster materials to fuse these armor pieces?");
    fnhook_set(FN_SMITH_UPDATE, hle_register("armorfusion:smith", smith_update));
    fnhook_set(FN_UPDATE, hle_register("armorfusion:update", update));
    fnhook_set(FN_CLOSE, hle_register("armorfusion:close", close_ui));
    fnhook_set(FN_VALID, hle_register("armorfusion:valid", validity));
    fnhook_set(FN_PAIR, hle_register("armorfusion:pair", pair));
    fnhook_set(FN_QUOTE, hle_register("armorfusion:quote", quote));
    fnhook_set(FN_CONFIRM, hle_register("armorfusion:confirm", confirmation));
    fnhook_set(FN_CONSUME, hle_register("armorfusion:consume", consume));
    fnhook_set(FN_MESSAGE, hle_register("armorfusion:message", message));
    fnhook_set(FN_DIALOG_TEXT, hle_register("armorfusion:dialog_text", dialog_text));
    fnhook_set(FN_SMITH_CLOSE, hle_register("armorfusion:smith_close", smith_close));
    fnhook_set(FN_SMITH_DTOR, hle_register("armorfusion:smith_dtor", smith_dtor));
    fnhook_set(FN_SMITH_DELETE, hle_register("armorfusion:smith_delete", smith_delete));
}
