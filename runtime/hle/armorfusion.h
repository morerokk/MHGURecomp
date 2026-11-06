#pragma once
#include "../armor_fusion_policy.h"
#include <stdbool.h>

// Catalog values must come from independently verified progression-rank data.
// Query fills ranks and the appearance recipe; immutable material storage
// remains valid until catalog reinitialization. It must not mutate guest state.
typedef bool (*ArmorFusionQuery)(FusionPiece *base, FusionPiece *appearance,
    FusionRecipe *recipe);
typedef bool (*ArmorFusionClassify)(FusionPiece *piece);
void armorfusion_set_catalog(ArmorFusionQuery query);
void armorfusion_set_classifier(ArmorFusionClassify classify);
void armorfusion_init(void);
