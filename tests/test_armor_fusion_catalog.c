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
static void write_catalog(void) {
    FILE *f = fopen("armor-fusion.afus", "wb"); assert(f);
    fwrite("AFUS", 1, 4, f); word(f, 2, 4); word(f, 3, 4); word(f, 3, 4);
    for (int i = 0; i < 32; ++i) fputc(0, f);
    for (int i = 0; i < 3; ++i) {
        word(f, i ? 149 + i : 100, 2); fputc(1, f); fputc(255, f);
        fputc(1, f); fputc(1, f); word(f, i ? 196 + i : 0, 2);
        word(f, i ? 10 : 0, 2); word(f, 0, 2);
    }
    const unsigned categories[] = {197, 197, 198}, items[] = {1202, 1203, 1300}, points[] = {2, 3, 5};
    for (int i = 0; i < 3; ++i) {
        word(f, categories[i], 2); word(f, items[i], 2); fputc(points[i], f);
        for (int j = 0; j < 3; ++j) fputc(0, f);
    }
    fclose(f);
}

static void check_recipes(void) {
    FusionPiece base = {1, 100, 1, FUSION_RANK_UNKNOWN, 1};
    FusionPiece appearance = {2, 150, 1, FUSION_RANK_UNKNOWN, 1};
    FusionRecipe first, second, repeated;
    assert(classifier && classifier(&base) && base.rank == FUSION_LR);
    assert(query(&base, &appearance, &first));
    assert(base.rank == FUSION_LR && appearance.rank == FUSION_LR);
    assert(first.armor_id == 150 && first.category_id == 197 && first.count == 2);
    assert(first.materials[0].item_id == 1202 && first.materials[0].points == 2);
    assert(first.materials[1].item_id == 1203 && first.materials[1].points == 3);
    appearance.armor_id = 151;
    assert(query(&base, &appearance, &second));
    assert(second.armor_id == 151 && second.category_id == 198 && second.count == 1);
    assert(second.materials[0].item_id == 1300 && second.materials[0].points == 5);
    assert(first.materials[0].item_id == 1202 && first.materials[0].points == 2);
    assert(first.materials[1].item_id == 1203 && first.materials[1].points == 3);
    assert(first.materials != second.materials);
    appearance.armor_id = 150;
    assert(query(&base, &appearance, &repeated));
    assert(repeated.materials == first.materials);
    assert(second.materials[0].item_id == 1300 && second.materials[0].points == 5);
    appearance.slot = 2;
    assert(!query(&base, &appearance, &repeated));
    appearance.slot = 1; appearance.level = 0;
    assert(!query(&base, &appearance, &repeated));
    appearance.level = 1; appearance.armor_id = 100;
    assert(!query(&base, &appearance, &repeated)); // Base-only cannot supply appearance.
    appearance.armor_id = 2048;
    assert(!query(&base, &appearance, &repeated));
    appearance.armor_id = 150;
    assert(!query(NULL, &appearance, &repeated));
    assert(!query(&base, NULL, &repeated));
    assert(!query(&base, &appearance, NULL));
    assert(!classifier(NULL));
    appearance.slot = 6;
    assert(!classifier(&appearance));
}

static void patch_catalog(long offset, unsigned value, int bytes) {
    FILE *f = fopen("armor-fusion.afus", "r+b"); assert(f);
    assert(fseek(f, offset, SEEK_SET) == 0);
    word(f, value, bytes);
    fclose(f);
}

static void check_invalid_catalogs(void) {
    const struct { long offset; unsigned value; int bytes; } invalid[] = {
        {0, 0, 1}, {4, 1, 4}, {8, 6145, 4}, {12, 32769, 4}, // Header limits/version.
        {48, 0, 2}, {48, 2048, 2}, {50, 0, 1}, {51, 0, 1}, // Armor ID/level bounds.
        {52, 0, 1}, {52, 4, 1}, {53, 0, 1}, {53, 32, 1}, // Rank/slot bounds.
        {54, 1024, 2}, {56, 10, 2}, {68, 9, 2}, {70, 1, 2}, // Category/points/reserved.
        {60, 100, 2}, {72, 150, 2}, // Unsorted/duplicate armor intervals.
        {84, 0, 2}, {84, 1024, 2}, {86, 0, 2}, {86, 4096, 2}, {88, 0, 1},
        {94, 1202, 2}, {100, 196, 2}, // Duplicate/unsorted material members.
        {12, 4, 4}, // Truncated member array.
    };
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) {
        write_catalog();
        assert(armor_fusion_catalog_init() && query && classifier);
        patch_catalog(invalid[i].offset, invalid[i].value, invalid[i].bytes);
        assert(!armor_fusion_catalog_init() && !query && !classifier);
    }
    write_catalog();
    assert(armor_fusion_catalog_init());
    patch_catalog(72, 150, 2); patch_catalog(74, 2, 1); // Sorted but overlapping intervals.
    assert(!armor_fusion_catalog_init() && !query && !classifier);
    write_catalog();
    assert(armor_fusion_catalog_init());
    FILE *f = fopen("armor-fusion.afus", "wb"); assert(f);
    fputs("AFUS", f); fclose(f);
    assert(!armor_fusion_catalog_init() && !query && !classifier);
    write_catalog();
    assert(armor_fusion_catalog_init());
    assert(remove("armor-fusion.afus") == 0);
    assert(!armor_fusion_catalog_init() && !query && !classifier);
}

int main(int argc, char **argv) {
    write_catalog();
    remove("armor-fusion.names");
    if (argc > 1 && !strcmp(argv[1], "extra")) {
        FILE *f = fopen("armor-fusion.afus", "ab"); assert(f); fputc(1, f); fclose(f);
        assert(!armor_fusion_catalog_init() && !query && !classifier);
        return 0;
    }
    assert(armor_fusion_catalog_init() && query && classifier);
    check_recipes();
    FILE *f = fopen("armor-fusion.names", "wb"); assert(f);
    fputs("obsolete invalid names metadata\n", f); fclose(f);
    assert(armor_fusion_catalog_init() && query && classifier);
    check_recipes();
    check_invalid_catalogs();
    remove("armor-fusion.names");
    puts("armor fusion catalog checks passed");
}
