// Armor fusion catalog and read-only policy; the native adapter owns mutation.
#ifndef ARMOR_FUSION_H
#define ARMOR_FUSION_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum FusionRank { FUSION_RANK_UNKNOWN, FUSION_LR, FUSION_HR, FUSION_G };
enum { FUSION_POINTS = 10, FUSION_MAX_MATERIALS = 10 };

typedef struct {
    uint32_t box_index;
    uint16_t armor_id;
    uint8_t slot;              // Native hunter armor categories 1..5.
    enum FusionRank rank;     // Armor tier, including Special Permit level progression.
    uint8_t level;             // Displayed upgrade level (one based).
} FusionPiece;

typedef struct {
    uint16_t item_id;
    uint8_t points;
} FusionMaterial;

typedef struct {
    uint16_t armor_id;         // Recipe belongs to the appearance armor.
    uint16_t category_id;
    enum FusionRank rank;
    const FusionMaterial *materials;
    size_t count;
} FusionRecipe;

typedef struct { uint16_t item_id, quantity; } FusionSelection;
typedef uint32_t (*FusionOwned)(void *context, uint16_t item_id);
typedef struct {
    FusionSelection deductions[FUSION_MAX_MATERIALS];
    size_t count;
    uint32_t points;
} FusionPlan;

// Validates an explicit player selection, without consuming anything. Failure
// clears the output. Rebuild using current equipment/inventory at confirmation;
// a previously successful plan is not authorization to mutate a changed save.
bool fusion_plan(const FusionPiece *base, const FusionPiece *appearance,
    uint32_t native_pair_result, const FusionRecipe *recipe,
    const FusionSelection *selection, size_t count,
    FusionOwned owned, void *context, FusionPlan *out);

// Catalog values come from verified progression-rank data. Queries fail closed
// without a valid catalog. Material storage is immutable until the next load.
bool fusion_catalog_load(void);
bool fusion_classify(FusionPiece *piece);
bool fusion_recipe(FusionPiece *base, FusionPiece *appearance, FusionRecipe *recipe);
void armorfusion_init(void);
#endif
