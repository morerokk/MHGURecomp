// Native MHGU 1.4.0 fusion UI adapter. The game's confirmation and equipped
// armor handling remain responsible for the equipment mutation. The host
// intercepts confirmation BEFORE the native zenny charge and substitutes the
// selected materials at the native token deduction call site.
#include "armorfusion.h"
#include "fusion_materials.h"
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

static ArmorFusionQuery catalog;
static ArmorFusionClassify classify;
static uint32_t active_ui, context_ui;
static bool awaiting, approved, confirming, confirmation_released;
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
static bool aborting;

static uint32_t call4(Cpu *c, uint32_t fn, uint32_t a, uint32_t b, uint32_t d, uint32_t e) {
    uint32_t saved[16];
    memcpy(saved, c->r, sizeof saved);
    c->r[0] = a; c->r[1] = b; c->r[2] = d; c->r[3] = e;
    guest_call(c, fn);
    uint32_t result = c->r[0];
    memcpy(c->r, saved, sizeof saved);
    return result;
}

static void suspend_parent(Cpu *c, uint32_t ui) {
    parent_ui = ui;
    visible_children = 0;
    uint32_t children = LD32(ui + 0x34), count = LD32(ui + 0x30);
    for (unsigned i = 0; children && i < count && i < 30; ++i) {
        uint32_t child = LD32(children + 4 * i);
        if (!child) continue;
        // These children are uGUI nodes, unlike the plain facility object.
        // Native conditional show/hide (0x76df00/0x76e04c) checks +0x18.
        if (LD8(child + 0x18)) {
            visible_children |= 1u << i;
            call4(c, FN_HIDE_CHILD, ui, i, 0, 0);
        }
    }
}

static bool parent_live(void) {
    if (!parent_ui || aborting || !parent_facility || LD32(G_FACILITY) != parent_facility ||
        LD32(parent_facility + 0x15c) != parent_ui) return false;
    unsigned state = LD32(parent_ui + 0xc) & 7;
    return state == 1 || state == 2;
}

static void restore_parent(Cpu *c) {
    if (parent_live()) {
        for (unsigned i = 0; i < 30; ++i)
            if (visible_children & (1u << i)) call4(c, FN_SHOW_CHILD, parent_ui, i, 0, 0);
        ST8(parent_ui + 4, 0); // Native main-menu setup restores its hints/title.
    }
    parent_ui = visible_children = 0;
}

static bool scope(void) {
    // +0x44 mode 0 is the hunter domain. Material interception additionally
    // requires the fusion-specific confirmation/consume return addresses;
    // separation keeps its original native costs and transaction.
    return catalog && context_ui && context_ui == active_ui && LD32(context_ui + 0x44) == 0;
}

static bool pieces(Cpu *c, uint32_t ui, FusionPiece *base, FusionPiece *appearance,
                   uint32_t *base_record, uint32_t *appearance_record) {
    uint32_t box = LD32(G_EQUIP_BOX);
    if (!box || LD32(ui + 0x6c) != 0) return false;
    base->box_index = LD32(ui + 0x84);
    appearance->box_index = LD32(ui + 0x88);
    if (base->box_index == appearance->box_index || base->box_index >= 2000 || appearance->box_index >= 2000)
        return false;
    *base_record = call4(c, FN_BOX_RECORD, box, 0, base->box_index, 0);
    *appearance_record = call4(c, FN_BOX_RECORD, box, 0, appearance->box_index, 0);
    if (!*base_record || !*appearance_record) return false;
    base->slot = LD16(*base_record + 4) & 31;
    appearance->slot = LD16(*appearance_record + 4) & 31;
    base->armor_id = LD16(*base_record + 6);
    appearance->armor_id = LD16(*appearance_record + 6);
    base->level = ((LD16(*base_record + 4) >> 5) & 31) + 1;
    appearance->level = ((LD16(*appearance_record + 4) >> 5) & 31) + 1;
    return base->slot >= 1 && base->slot <= 5 && appearance->slot >= 1 && appearance->slot <= 5;
}

static uint32_t inventory(void *context, uint16_t id) {
    return call4(context, FN_COUNT, id, 0, 0, 0);
}

static uint32_t native_pair(Cpu *c, uint32_t ui, const FusionPiece *base,
                            const FusionPiece *appearance, uint32_t b, uint32_t a) {
    // The fifth argument belongs to our own aligned guest stack frame.
    uint32_t sp = c->r[13];
    c->r[13] -= 16;
    ST32(c->r[13], appearance->box_index);
    uint32_t result = call4(c, FN_PAIR, ui + 0x80, b, a, base->box_index);
    c->r[13] = sp;
    return result;
}

static bool snapshot(Cpu *c, uint32_t ui, FusionPiece *base, FusionPiece *appearance,
                     FusionRecipe *recipe, FusionUiRequest *request, uint32_t *native) {
    uint32_t b, a;
    memset(base, 0, sizeof *base); memset(appearance, 0, sizeof *appearance);
    memset(recipe, 0, sizeof *recipe); memset(request, 0, sizeof *request);
    if (!pieces(c, ui, base, appearance, &b, &a) || !catalog(base, appearance, recipe, request)) return false;
    uint32_t sp = c->r[13];
    c->r[13] -= 16;
    ST32(c->r[13], c->r[13] + 8);
    ST32(c->r[13] + 4, 0); ST32(c->r[13] + 8, 0);
    call4(c, FN_QUOTE, ui + 0x80, b, a, c->r[13] + 4);
    request->zenny = LD32(c->r[13] + 4);
    c->r[13] = sp;
    *native = native_pair(c, ui, base, appearance, b, a);
    return base->rank != FUSION_RANK_UNKNOWN && base->rank == appearance->rank && *native == 1;
}

static void validity(Cpu *c) {
    uint32_t record = c->r[1];
    fnhook_call_original(c, FN_VALID);
    if (scope() && classify && record) {
        FusionPiece piece = { .armor_id = LD16(record + 6), .slot = LD16(record + 4) & 31,
            .level = ((LD16(record + 4) >> 5) & 31) + 1 };
        // Native 0x340bd8..bfc sets bit 0x08 only for the category unlock.
        // With hunter slot 1..5, helper 0x2daeb8 necessarily returns true;
        // therefore bit 0x10 can only come from the special-armor progress
        // flag lookup at 0x340c84..ca0, including the late Deviant unlock.
        // Clear these progression gates only for a catalog-recognized owned
        // hunter piece. Empty/type/sex/already-fused/decorations bits remain.
        if (piece.slot >= 1 && piece.slot <= 5 && classify(&piece) &&
            piece.rank >= FUSION_LR && piece.rank <= FUSION_G) c->r[0] &= ~0x18u;
    }
}

static void pair(Cpu *c) {
    uint32_t b = c->r[1], a = c->r[2];
    fnhook_call_original(c, FN_PAIR);
    if (!scope() || !b || !a) return;
    FusionPiece base = { .armor_id = LD16(b + 6), .slot = LD16(b + 4) & 31,
            .level = ((LD16(b + 4) >> 5) & 31) + 1 },
        appearance = { .armor_id = LD16(a + 6), .slot = LD16(a + 4) & 31,
            .level = ((LD16(a + 4) >> 5) & 31) + 1 };
    FusionRecipe recipe; FusionUiRequest request;
    if (!catalog(&base, &appearance, &recipe, &request) || base.rank == FUSION_RANK_UNKNOWN ||
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
        coordinate_visibility = 0;
        uint32_t children = LD32(active_ui + 0x34), count = LD32(active_ui + 0x30);
        for (unsigned i = 0; children && i < count && i < 30; ++i) {
            uint32_t child = LD32(children + i * 4);
            // Native class bindings from 0x85ff6c: keep the shop header,
            // money, hunter type and left paper/3D-preview frame. Only the
            // equipment detail/list widgets overlap the point-material UI.
            if (child && LD8(child + 0x18)) {
                coordinate_visibility |= 1u << i;
                if (i != 0 && i != 1 && i != 4 && i != 10)
                    call4(c, FN_HIDE_CHILD, active_ui, i, 0, 0);
            }
        }
        // Shared native message/Yes-No controls are owned by the common
        // facility, independently of Coordinate. Preserve both visibility
        // states; the pool uses message4 for native material instructions.
        common_picker_ui = call4(c, 0x75d2b4, parent_facility, 0, 0, 0);
        common_picker_visibility = 0;
        if (common_picker_ui && LD32(common_picker_ui + 0x34) && LD32(common_picker_ui + 0x30) > 5) {
            uint32_t children = LD32(common_picker_ui + 0x34);
            for (unsigned i = 4; i <= 5; ++i) {
                uint32_t child = LD32(children + i * 4);
                if (child && LD8(child + 0x18)) common_picker_visibility |= 1u << i;
            }
            call4(c, FN_HIDE_CHILD, common_picker_ui, 5, 0, 0);
        }
    } else {
        for (unsigned i = 0; i < 30; ++i)
            if (coordinate_visibility & (1u << i)) call4(c, FN_SHOW_CHILD, active_ui, i, 0, 0);
        // Never restore cached GUI addresses after a parent/scene teardown.
        if (parent_live() && common_picker_ui &&
            call4(c, 0x75d2b4, parent_facility, 0, 0, 0) == common_picker_ui) {
            for (unsigned i = 4; i <= 5; ++i)
                call4(c, common_picker_visibility & (1u << i) ? FN_SHOW_CHILD : FN_HIDE_CHILD,
                    common_picker_ui, i, 0, 0);
        }
        coordinate_visibility = common_picker_ui = common_picker_visibility = 0;
    }
}

static bool final_question(Cpu *c) {
    uint32_t common = call4(c, 0x75d2b4, parent_facility, 0, 0, 0);
    if (!common || !LD32(common + 0x34) || LD32(common + 0x30) <= 5) return false;
    uint32_t children = LD32(common + 0x34);
    if (!LD32(children + 4 * 4) || !LD32(children + 5 * 4)) return false;
    // Re-open the original MHGU question/Yes-No controls. Formatting flag1
    // invalidates the cached string even when the message ID is unchanged;
    // our scoped getter supplies the literal, freshly quoted fusion fee.
    uint32_t sp = c->r[13]; c->r[13] -= 32;
    ST32(c->r[13], 0); ST32(c->r[13] + 4, 1);
    ST32(c->r[13] + 8, 0); ST32(c->r[13] + 12, 0);
    call4(c, 0x85e5bc, 0x14, 1, 37, 43);
    c->r[13] = sp;
    call4(c, 0x85e794, 2, 0x20, 0, 0);
    ST8(context_ui + 0x62, 0);
    call4(c, FN_SHOW_CHILD, common, 4, 0, 0);
    call4(c, FN_SHOW_CHILD, common, 5, 0, 0);
    return true;
}

static void confirmation(Cpu *c) {
    if (!scope() || c->r[14] != 0x860e7c) { fnhook_call_original(c, FN_CONFIRM); return; }
    if (!parent_live()) {
        fusion_materials_cancel(c, false);
        awaiting = confirming = false; c->r[0] = 2; return;
    }
    if (awaiting) {
        int result = fusion_materials_poll(c, selected, FUSION_MAX_MATERIALS, &selected_count);
        c->r[0] = 0;
        if (!result) return;
        awaiting = false;
        picker_visibility(c, false);
        if (result == 2) { c->r[0] = 2; return; }
        confirming = true; confirmation_released = false;
        if (!final_question(c)) { confirming = false; c->r[0] = 2; }
        return;
    }
    if (confirming) {
        uint32_t input = LD32(G_INPUT);
        uint32_t held = input ? LD32(input + 0x6bfc) | LD32(input + 0x6c34) : 0;
        if (!confirmation_released) {
            if (!(held & 0x6000)) confirmation_released = true;
            c->r[0] = 0; return;
        }
        fnhook_call_original(c, FN_CONFIRM);
        if (c->r[0] == 2) { confirming = false; return; }
        if (c->r[0] != 1) return;
        confirming = false;
        FusionPiece base, appearance; FusionRecipe recipe; FusionUiRequest request; uint32_t native;
        if (!snapshot(c, context_ui, &base, &appearance, &recipe, &request, &native) ||
            base.box_index != opened_base.box_index || base.armor_id != opened_base.armor_id ||
            base.level != opened_base.level ||
            appearance.box_index != opened_appearance.box_index || appearance.armor_id != opened_appearance.armor_id ||
            appearance.level != opened_appearance.level ||
            request.zenny != opened_zenny ||
            !call4(c, FN_ZENNY_CHECK, LD32(G_PLAYER_DATA), request.zenny, 0, 0) ||
            fusion_plan(&base, &appearance, native, &recipe, selected, selected_count,
                inventory, c, &approved_plan) != FUSION_OK) { c->r[0] = 2; return; }
        approved = true;
        ST8(context_ui + 0x62, 0);
        ST32(context_ui + 0x48, request.zenny);
        if (trace) LOG("[armor-fusion] approved base %u/%u Lv%u appearance %u/%u Lv%u: %u points, %zu materials, %uz\n",
            base.slot, base.armor_id, base.level, appearance.slot, appearance.armor_id, appearance.level,
            approved_plan.points, approved_plan.count, request.zenny);
        c->r[0] = 1;
        return;
    }
    // The first arrival in the native cost phase opens material selection.
    // Only the later native Yes/No can authorize payment.
    c->r[0] = 0;
    FusionPiece base, appearance; FusionRecipe recipe; FusionUiRequest request; uint32_t native;
    if (snapshot(c, context_ui, &base, &appearance, &recipe, &request, &native)) {
        opened_base = base; opened_appearance = appearance; opened_zenny = request.zenny;
        snprintf(G2H(fusion_question), 96,
            "Fuse these pieces of armor for %uz?\r\nUse the selected monster materials.", request.zenny);
        picker_visibility(c, true);
        awaiting = fusion_materials_open(c, parent_ui, recipe.category_id);
        if (awaiting) {
            // Smith and Coordinate Money/Preview objects share renderer
            // resources. Reassert the preserved Coordinate panels after the
            // point UI has finished switching the parent's native widgets.
            const unsigned keep[] = {0, 1, 4, 10};
            for (unsigned i = 0; i < sizeof keep / sizeof keep[0]; ++i)
                if (coordinate_visibility & (1u << keep[i]))
                    call4(c, FN_SHOW_CHILD, active_ui, keep[i], 0, 0);
        } else picker_visibility(c, false);
        if (trace) LOG("[armor-fusion] native material chooser %s for armor %u Lv%u category %u\n",
            awaiting ? "opened" : "rejected", appearance.armor_id, appearance.level, recipe.category_id);
        if (!awaiting) c->r[0] = 2;
    } else c->r[0] = 2;
}

static void consume(Cpu *c) {
    if (!scope() || c->r[14] != 0x860eb0) { fnhook_call_original(c, FN_CONSUME); return; }
    // Confirmation and this call execute synchronously on the guest game
    // thread. Every deduction was checked immediately before the native fee.
    if (!approved) FATAL("armor fusion: token deduction without approved plan\n");
    for (size_t i = 0; i < approved_plan.count; ++i)
        call4(c, FN_CONSUME, approved_plan.deductions[i].item_id, approved_plan.deductions[i].quantity, 0, 0);
    if (trace) LOG("[armor-fusion] selected materials consumed; continuing native equipment commit\n");
    approved = false;
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
    if (!catalog) { fnhook_call_original(c, FN_SMITH_UPDATE); return; }
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
        active_ui = tasks ? call4(c, FN_ALLOCATE, 0x200, 0x10, 0, 0) : 0;
        if (active_ui) {
            call4(c, FN_CONSTRUCT, active_ui, 0, 0, 0);
            // Exact original Lab chain at 0x75e7e0..0x75e830: task order
            // 0x18, two zero setup words, immediate-registration flag true.
            // Do not overwrite cFacility +0x15c: it still owns the parent.
            uint32_t sp = c->r[13];
            c->r[13] -= 16;
            ST32(c->r[13], 1); ST32(c->r[13] + 4, 0); ST32(c->r[13] + 8, 0);
            uint32_t registered = call4(c, FN_REGISTER, tasks, active_ui, 0x18, c->r[13] + 4);
            c->r[13] = sp;
            if (!registered) {
                uint32_t failed = active_ui;
                call4(c, FN_CLOSE, failed, 0, 0, 0);
                call4(c, FN_DELETE, failed, 0, 0, 0);
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
    if (catalog && active_ui && LD8(active_ui + 4) == 2 && LD32(active_ui + 0x44) == 0 &&
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
        fusion_materials_cancel(c, restore);
        // A normal native close can interrupt the pool before poll returns.
        // Restore its shared controls while both owners are still live;
        // teardown/scene changes must only discard cached guest addresses.
        if (restore && (coordinate_visibility || common_picker_ui)) picker_visibility(c, false);
        active_ui = 0; awaiting = approved = confirming = false;
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
    if (active_ui) call4(c, FN_CLOSE, active_ui, 0, 0, 0);
    aborting = false;
}

static void smith_close(Cpu *c) { abort_child(c); fnhook_call_original(c, FN_SMITH_CLOSE); }
static void smith_dtor(Cpu *c) { abort_child(c); fnhook_call_original(c, FN_SMITH_DTOR); }
static void smith_delete(Cpu *c) { abort_child(c); fnhook_call_original(c, FN_SMITH_DELETE); }

void armorfusion_set_catalog(ArmorFusionQuery query) { catalog = query; }
void armorfusion_set_classifier(ArmorFusionClassify classifier) { classify = classifier; }

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
