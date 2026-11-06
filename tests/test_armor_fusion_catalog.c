#include "armor_fusion_catalog.h"
#include "hle/armorfusion.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static ArmorFusionQuery query;
static ArmorFusionClassify classifier;
bool plat_exe_path(char *out, size_t size) { return snprintf(out, size, "./test-program") < (int)size; }
void armorfusion_set_catalog(ArmorFusionQuery value) { query = value; }
void armorfusion_set_classifier(ArmorFusionClassify value) { classifier = value; }
static void word(FILE *f, unsigned value, int bytes) {
    while (bytes--) { fputc(value & 255, f); value >>= 8; }
}
int main(int argc, char **argv) {
    FILE *f = fopen("armor-fusion.afus", "wb"); assert(f);
    fwrite("AFUS", 1, 4, f); word(f, 2, 4); word(f, 2, 4); word(f, 1, 4);
    for (int i = 0; i < 32; ++i) fputc(0, f);
    for (int i = 0; i < 2; ++i) {
        word(f, i ? 150 : 100, 2); fputc(1, f); fputc(255, f);
        fputc(1, f); fputc(1, f); word(f, i ? 197 : 0, 2);
        word(f, i ? 10 : 0, 2); word(f, 0, 2);
    }
    word(f, 197, 2); word(f, 1202, 2); fputc(2, f);
    for (int i = 0; i < 3; ++i) fputc(0, f);
    if (argc > 1 && !strcmp(argv[1], "extra")) fputc(1, f);
    fclose(f);
    f = fopen("armor-fusion.names", "wb"); assert(f);
    fputs("A\t100\t1\tLeather Helm\nA\t150\t1\tRathalos Helm\nI\t1202\t0\tRathalos Scale\nC\t197\t0\tLR Rathalos Materials\n", f);
    fclose(f);
    if (argc > 1) { assert(!armor_fusion_catalog_init() && !query); return 0; }
    assert(armor_fusion_catalog_init() && query);
    FusionPiece base = {1, 100, 1, FUSION_RANK_UNKNOWN, 1};
    FusionPiece appearance = {2, 150, 1, FUSION_RANK_UNKNOWN, 1};
    FusionRecipe recipe; FusionUiRequest request;
    assert(classifier && classifier(&base) && base.rank == FUSION_LR);
    assert(query(&base, &appearance, &recipe, &request));
    assert(base.rank == FUSION_LR && appearance.rank == FUSION_LR);
    assert(recipe.armor_id == 150 && recipe.category_id == 197 && recipe.count == 1);
    assert(recipe.materials[0].item_id == 1202 && recipe.materials[0].points == 2);
    assert(!strcmp(request.rows[0].name, "Rathalos Scale"));
    appearance.slot = 2;
    assert(!query(&base, &appearance, &recipe, &request));
    appearance.slot = 1; appearance.level = 0;
    assert(!query(&base, &appearance, &recipe, &request));
    appearance.level = 1; appearance.armor_id = 100;
    assert(!query(&base, &appearance, &recipe, &request)); // Base-only cannot supply appearance.
    puts("armor fusion catalog checks passed");
}
