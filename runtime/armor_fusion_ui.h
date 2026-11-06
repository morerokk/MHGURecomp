#pragma once
#include "armor_fusion_policy.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { FUSION_UI_MAX_MATERIALS = 256 };
typedef struct {
    uint16_t item_id;
    uint8_t points;
    uint32_t owned;
    char name[128];
} FusionUiMaterial;
typedef struct {
    char base_name[128], appearance_name[128], category_name[128];
    uint32_t zenny;
    enum FusionRank rank;
    size_t count;
    FusionUiMaterial rows[FUSION_UI_MAX_MATERIALS];
} FusionUiRequest;
#ifdef __cplusplus
}
#endif
