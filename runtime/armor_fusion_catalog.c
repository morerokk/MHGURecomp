#include "armor_fusion_catalog.h"
#include "hle/armorfusion.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { ARMOR_LIMIT = 2048, ITEM_LIMIT = 4096, CATEGORY_LIMIT = 1024, MEMBER_LIMIT = 32768 };
typedef struct { unsigned char rank, slots, min_level, max_level; uint16_t category; } Entry;
typedef struct { uint16_t category, item; unsigned char points; } Member;
static Entry entries[ARMOR_LIMIT][3];
static Member members[MEMBER_LIMIT];
static size_t member_count;
static char armor_names[ARMOR_LIMIT][5][128], item_names[ITEM_LIMIT][128], category_names[CATEGORY_LIMIT][128];
static FusionMaterial recipe_members[FUSION_UI_MAX_MATERIALS];
static unsigned u16(const unsigned char *p) { return p[0] | p[1] << 8; }
static unsigned u32(const unsigned char *p) { return u16(p) | (uint32_t)u16(p + 2) << 16; }

static const Entry *entry_for(const FusionPiece *piece) {
    unsigned level = piece->level;
    for (int i = 0; i < 3; ++i) {
        const Entry *e = &entries[piece->armor_id][i];
        if (e->rank && level >= e->min_level && level <= e->max_level) return e;
    }
    return NULL;
}

static bool classify(FusionPiece *piece) {
    if (!piece || piece->armor_id >= ARMOR_LIMIT || piece->slot < 1 || piece->slot > 5) return false;
    const Entry *entry = entry_for(piece);
    if (!entry || !(entry->slots & (1u << (piece->slot - 1)))) return false;
    piece->rank = entry->rank;
    return true;
}

static bool query(FusionPiece *base, FusionPiece *appearance, FusionRecipe *recipe, FusionUiRequest *request) {
    if (!base || !appearance || !recipe || !request || base->armor_id >= ARMOR_LIMIT ||
        appearance->armor_id >= ARMOR_LIMIT || base->slot < 1 || base->slot > 5 ||
        appearance->slot < 1 || appearance->slot > 5) return false;
    const Entry *b = entry_for(base), *a = entry_for(appearance);
    if (!b || !a || !(b->slots & (1u << (base->slot - 1))) ||
        !(a->slots & (1u << (appearance->slot - 1))) || !a->category) return false;
    base->rank = b->rank; appearance->rank = a->rank;
    const char *bn = armor_names[base->armor_id][base->slot - 1];
    const char *an = armor_names[appearance->armor_id][appearance->slot - 1];
    if (!*bn || !*an || !*category_names[a->category]) return false;
    memset(request, 0, sizeof *request);
    snprintf(request->base_name, sizeof request->base_name, "%s (Lv %u)", bn, base->level);
    snprintf(request->appearance_name, sizeof request->appearance_name, "%s (Lv %u)", an, appearance->level);
    memcpy(request->category_name, category_names[a->category], 128);
    request->rank = a->rank;
    for (size_t i = 0; i < member_count; ++i) {
        const Member *m = members + i;
        if (m->category != a->category) continue;
        if (request->count == FUSION_UI_MAX_MATERIALS || !*item_names[m->item]) return false;
        size_t n = request->count++;
        request->rows[n].item_id = m->item;
        request->rows[n].points = m->points;
        memcpy(request->rows[n].name, item_names[m->item], 128);
        recipe_members[n] = (FusionMaterial){m->item, m->points};
    }
    *recipe = (FusionRecipe){appearance->armor_id, a->category, a->rank, recipe_members, request->count};
    return request->count != 0;
}

bool armor_fusion_catalog_init(void) {
    armorfusion_set_catalog(NULL);
    armorfusion_set_classifier(NULL);
    memset(entries, 0, sizeof entries);
    memset(members, 0, sizeof members);
    memset(armor_names, 0, sizeof armor_names);
    memset(item_names, 0, sizeof item_names);
    memset(category_names, 0, sizeof category_names);
    member_count = 0;
    char path[2048];
#ifdef __ANDROID__
    strcpy(path, "./mhgurecomp");
#else
    if (!plat_exe_path(path, sizeof path)) return false;
#endif
    char *slash = strrchr(path, '/'), *backslash = strrchr(path, '\\');
    if (!slash || (backslash && backslash > slash)) slash = backslash;
    if (!slash || (size_t)(slash - path) + 32 >= sizeof path) return false;
    strcpy(slash + 1, "armor-fusion.afus");
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    unsigned char header[48] = {0}, row[12] = {0};
    bool ok = fread(header, 1, sizeof header, file) == sizeof header &&
        !memcmp(header, "AFUS", 4) && u32(header + 4) == 2;
    unsigned armor_count = ok ? u32(header + 8) : 0;
    member_count = ok ? u32(header + 12) : 0;
    ok = ok && armor_count <= ARMOR_LIMIT * 3 && member_count <= MEMBER_LIMIT;
    unsigned previous = 0;
    for (unsigned i = 0; ok && i < armor_count; ++i) {
        ok = fread(row, 1, 12, file) == 12;
        unsigned id = u16(row), category = u16(row + 6), points = u16(row + 8);
        unsigned key = id * 256u + row[2];
        ok = ok && id && key > previous && id < ARMOR_LIMIT && row[2] && row[3] >= row[2] &&
            row[4] >= 1 && row[4] <= 3 && row[5] && !(row[5] & ~31) && category < CATEGORY_LIMIT &&
            (category ? points == FUSION_POINTS : points == 0) && !u16(row + 10);
        if (ok) {
            Entry *e = entries[id];
            int slot = 0;
            while (slot < 3 && e[slot].rank) ++slot;
            if (slot == 3 || (slot && e[slot - 1].max_level >= row[2])) ok = false;
            else e[slot] = (Entry){row[4], row[5], row[2], row[3], category};
        }
        previous = key;
    }
    previous = 0;
    for (size_t i = 0; ok && i < member_count; ++i) {
        ok = fread(row, 1, 8, file) == 8;
        unsigned category = u16(row), item = u16(row + 2), key = category << 16 | item;
        ok = ok && category && category < CATEGORY_LIMIT && item && item < ITEM_LIMIT && row[4] && key > previous;
        if (ok) members[i] = (Member){category, item, row[4]};
        previous = key;
    }
    ok = ok && fgetc(file) == EOF;
    fclose(file);
    if (!ok) return false;
    strcpy(slash + 1, "armor-fusion.names");
    file = fopen(path, "rb");
    if (!file) return false;
    char line[1024];
    while (fgets(line, sizeof line, file)) {
        char type; unsigned id, slot; int offset = 0;
        if (sscanf(line, "%c\t%u\t%u\t%n", &type, &id, &slot, &offset) != 3 || !offset) { ok = false; break; }
        char *name = line + offset;
        name[strcspn(name, "\r\n")] = 0;
        if (!*name || strlen(name) >= 128) { ok = false; break; }
        char *out = NULL;
        if (type == 'A' && id < ARMOR_LIMIT && slot >= 1 && slot <= 5) out = armor_names[id][slot - 1];
        if (type == 'I' && id < ITEM_LIMIT && !slot) out = item_names[id];
        if (type == 'C' && id < CATEGORY_LIMIT && !slot) out = category_names[id];
        if (!out || *out) { ok = false; break; }
        strcpy(out, name);
    }
    ok = ok && !ferror(file);
    fclose(file);
    if (ok) { armorfusion_set_catalog(query); armorfusion_set_classifier(classify); }
    return ok;
}
