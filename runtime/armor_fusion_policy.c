#include "armor_fusion_policy.h"
#include <string.h>

static int valid_rank(enum FusionRank rank) { return rank >= FUSION_LR && rank <= FUSION_G; }

enum FusionResult fusion_plan(const FusionPiece *base, const FusionPiece *appearance,
    uint32_t native_pair_result, const FusionRecipe *recipe,
    const FusionSelection *selection, size_t count,
    FusionOwned owned, void *context, FusionPlan *out) {
    if (!out) return FUSION_INVALID_SELECTION;
    memset(out, 0, sizeof *out);
    if (!base || !appearance || !base->armor_id || !appearance->armor_id ||
        base->slot < 1 || base->slot > 5 || base->slot != appearance->slot ||
        base->box_index == appearance->box_index || base->armor_id == appearance->armor_id)
        return FUSION_INVALID_PAIR;
    if (!valid_rank(base->rank) || !valid_rank(appearance->rank)) return FUSION_UNKNOWN_RANK;
    if (base->rank != appearance->rank) return FUSION_RANK_MISMATCH;
    // The native validator returns 1 for a compatible pair. Its other bits
    // include equipment/decorations restrictions and must not be discarded.
    if (native_pair_result != 1) return FUSION_NATIVE_REJECTED;
    if (!recipe || recipe->armor_id != appearance->armor_id ||
        !recipe->category_id || !recipe->materials || !recipe->count ||
        recipe->rank != appearance->rank) return FUSION_INVALID_RECIPE;
    for (size_t i = 0; i < recipe->count; ++i) {
        if (!recipe->materials[i].item_id || !recipe->materials[i].points)
            return FUSION_INVALID_RECIPE;
        for (size_t j = 0; j < i; ++j)
            if (recipe->materials[j].item_id == recipe->materials[i].item_id)
                return FUSION_INVALID_RECIPE;
    }
    if (!owned || !selection || !count) return FUSION_INVALID_SELECTION;
    if (count > FUSION_MAX_MATERIALS) return FUSION_TOO_MANY_MATERIALS;
    FusionPlan plan = {0};
    for (size_t i = 0; i < count; ++i) {
        const FusionSelection s = selection[i];
        if (!s.item_id || !s.quantity || s.quantity > FUSION_POINTS) return FUSION_INVALID_SELECTION;
        for (size_t j = 0; j < i; ++j)
            if (selection[j].item_id == s.item_id) return FUSION_INVALID_SELECTION;
        unsigned points = 0;
        for (size_t j = 0; j < recipe->count; ++j)
            if (recipe->materials[j].item_id == s.item_id) points = recipe->materials[j].points;
        if (!points) return FUSION_INVALID_SELECTION;
        if (owned(context, s.item_id) < s.quantity) return FUSION_NOT_OWNED;
        plan.points += points * s.quantity;
        plan.deductions[plan.count++] = s;
    }
    if (plan.points < FUSION_POINTS) return FUSION_TOO_FEW_POINTS;
    *out = plan;
    return FUSION_OK;
}
