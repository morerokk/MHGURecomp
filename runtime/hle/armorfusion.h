#pragma once
#include "../armor_fusion_ui.h"
#include "../cpu.h"

// Catalog values must come from independently verified progression-rank data.
// Query fills ranks, names and the appearance recipe; material storage must
// remain valid until the next query. It must not mutate guest state.
typedef bool (*ArmorFusionQuery)(FusionPiece *base, FusionPiece *appearance,
    FusionRecipe *recipe, FusionUiRequest *request);
typedef bool (*ArmorFusionClassify)(FusionPiece *piece);
void armorfusion_set_catalog(ArmorFusionQuery query);
void armorfusion_set_classifier(ArmorFusionClassify classify);
void armorfusion_init(void);
