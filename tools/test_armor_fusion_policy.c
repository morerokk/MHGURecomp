#include "../runtime/armor_fusion_policy.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t owned(void *context, uint16_t id) {
    return id == 1202 ? *(unsigned *)context : 0;
}

int main(void) {
    FusionPiece base = {1, 100, 1, FUSION_LR, 1}, appearance = {2, 150, 1, FUSION_LR, 1};
    FusionMaterial material = {1202, 2};
    FusionRecipe recipe = {150, 197, FUSION_LR, &material, 1};
    FusionSelection selection[] = {{1202, 5}, {1202, 1}};
    unsigned inventory = 5;
    FusionPlan plan;
#define PLAN(n) fusion_plan(&base, &appearance, 1, &recipe, selection, n, owned, &inventory, &plan)
    assert(PLAN(1) == FUSION_OK && plan.points == 10 && plan.count == 1);
    assert(plan.deductions[0].quantity == 5 && inventory == 5); // Preview is read-only.
    inventory = 4; // Inventory changed after preview: confirmation must reject.
    assert(PLAN(1) == FUSION_NOT_OWNED && plan.count == 0 && plan.points == 0);
    inventory = 5;
    selection[0].quantity = 4;
    assert(PLAN(1) == FUSION_TOO_FEW_POINTS);
    selection[0].quantity = 5;
    assert(PLAN(2) == FUSION_INVALID_SELECTION); // Duplicate rows cannot double-spend.
    appearance.rank = FUSION_HR;
    assert(PLAN(1) == FUSION_RANK_MISMATCH);
    appearance.rank = FUSION_RANK_UNKNOWN;
    assert(PLAN(1) == FUSION_UNKNOWN_RANK);
    appearance.rank = FUSION_LR;
    recipe.rank = FUSION_HR;
    assert(PLAN(1) == FUSION_INVALID_RECIPE);
    recipe.rank = FUSION_LR;
    recipe.armor_id = 100;
    assert(PLAN(1) == FUSION_INVALID_RECIPE);
    recipe.armor_id = 150;
    appearance.slot = 2;
    assert(PLAN(1) == FUSION_INVALID_PAIR);
    appearance.slot = 1;
    assert(fusion_plan(&base, &appearance, 0x2001, &recipe, selection, 1,
        owned, &inventory, &plan) == FUSION_NATIVE_REJECTED);
    selection[0].item_id = 1203; // Same monster, wrong material rank/category.
    assert(PLAN(1) == FUSION_INVALID_SELECTION);
    selection[0].item_id = 1202;
    selection[0].quantity = 6;
    inventory = 6;
    assert(PLAN(1) == FUSION_OK && plan.points == 12); // Explicit overpayment allowed.
    selection[0].quantity = 65535;
    inventory = 65535;
    assert(PLAN(1) == FUSION_INVALID_SELECTION && !plan.count);
    selection[0].quantity = 6;
    FusionMaterial duplicates[] = {{1202, 2}, {1202, 3}};
    recipe.materials = duplicates;
    recipe.count = 2;
    assert(PLAN(1) == FUSION_INVALID_RECIPE);
    puts("armor fusion policy checks passed");
    return 0;
}
