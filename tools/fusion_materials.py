"""Build the fusion catalog from a player's MHGU 1.4.0 dump.

Normal armor uses MHGU's conventional armor tiers: rarity 1..3 LR, 4..7 HR,
8..10 G. Original Special Permit armor is HR at levels 1..10 and G at 11..15,
matching its native crafting material tiers. The six G-only Deviant families
are G throughout. A quest's LR classification does not define the armor tier.
An upgrade's material rank never defines the armor tier. No copyrighted tables
are embedded; dumps/saves are read-only.
"""
from __future__ import annotations

import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import struct


FORMATS = {
    "armorSeriesData.asd": (bytes.fromhex("00001842"), 127),
    "armorProcessA00.apd": (bytes.fromhex("0000803f"), 38),
    "itemCategoryTypeData.ict": (bytes.fromhex("6666aa41"), 7),
    "itemData.itm": (bytes.fromhex("6666aa41"), 44),
}
FORMATS.update({f"armorCreateA{slot:02}.acrd": (bytes.fromhex("0000803f"), 42)
                for slot in range(5)})
# Rustrazor, Nightcloak, Soulseer, Boltreaver, Elderfrost and Bloodbath pairs.
# Their Special Permit series start at G1; unlike the original twelve Deviants,
# their armor does not cross LR/HR/G tiers as it upgrades.
# https://monsterhunter.fandom.com/wiki/MHGU%3A_Special_Permit_Quests
G_ONLY_SPECIAL_PERMIT = frozenset(range(1108, 1120))
# Native crafting key ingredients can belong to both a broad and a specific
# category. These otherwise ambiguous series use the specific monster category
# (Blangonga Pelt+, Jaggi Shard, Congalala Fur, Diametrical Horn respectively).
SPECIFIC_CRAFT_CATEGORIES = {422: 105, 778: 40, 879: 322, 880: 322,
                             1044: 259, 1045: 259, 1046: 259, 1047: 259}
RANKS = ("LR", "HR", "G")


def records(raw: bytes, name: str) -> list[bytes]:
    version, stride = FORMATS[name]
    if len(raw) < 8 or raw[:4] != version:
        raise ValueError(f"{name}: unsupported or missing version header")
    count, = struct.unpack_from("<I", raw, 4)
    if count > 100_000 or len(raw) != 8 + count * stride:
        raise ValueError(f"{name}: count/record-size mismatch")
    return [raw[i:i + stride] for i in range(8, len(raw), stride)]


def unique_records(rows: list[bytes], name: str) -> dict[int, bytes]:
    result = {}
    for row in rows:
        key, = struct.unpack_from("<I", row)
        if key in result:
            raise ValueError(f"{name}: duplicate ID {key}")
        result[key] = row
    return result


def eligible(category, rank, categories, members):
    return (category in categories and categories[category][6] < len(RANKS)
            and RANKS[categories[category][6]] == rank and bool(members[category]))


def recipe_category(armor_id, rank, rows, crafts, categories, members, maximum_level):
    # Prefer the latest point recipe in the armor's tier, including later
    # upgrades. Earlier recipes can use generic Fang/Claw categories instead.
    matching = [(level, cat) for level, cat in rows if level <= maximum_level
                and eligible(cat, rank, categories, members)]
    choices = {cat for level, cat in matching if level == matching[-1][0]} if matching else set()
    if not choices:
        choices = {cat for flags, cats in crafts if flags & 2 for cat in cats
                   if eligible(cat, rank, categories, members)}
    if not choices:
        # Ticket/cosmetic armor may have no categorized key ingredient. Keep
        # native slot, ingredient and category order for its first fallback.
        primary = next((cat for _, cats in crafts for cat in cats
                        if eligible(cat, rank, categories, members)), None)
        choices = {primary} if primary is not None else set()
    if len(choices) != 1 and armor_id in SPECIFIC_CRAFT_CATEGORIES:
        specific = SPECIFIC_CRAFT_CATEGORIES[armor_id]
        if (not any(specific in cats for _, cats in crafts)
                or not eligible(specific, rank, categories, members)):
            raise ValueError(f"armor {armor_id}: changed specific crafting category")
        choices = {specific}
    if not choices:
        # Preserve native membership/points for award armor's rank category.
        generic = RANKS.index(rank) + 1
        choices = {generic} if eligible(generic, rank, categories, members) else set()
    return next(iter(choices)) if len(choices) == 1 else 0


def armor_ranges(armor_id, row):
    rarity = row[103] + 1
    if 617 <= armor_id <= 640:
        if rarity != 11 or row[105] != 5:
            raise ValueError(f"armor {armor_id}: changed progressive Special Permit metadata")
        # Their quests can start at LR, but level-I armor uses native HR
        # tickets/materials. Quest stars do not determine the armor's tier.
        return [(1, 10, "HR"), (11, 15, "G")]
    if armor_id in G_ONLY_SPECIAL_PERMIT:
        if rarity != 11 or row[105] != 1:
            raise ValueError(f"armor {armor_id}: changed G-only Special Permit metadata")
        return [(1, 5, "G")]
    rank = "LR" if rarity <= 3 else "HR" if rarity <= 7 else "G" if rarity <= 10 else None
    return [(1, 255, rank)] if rank else []


def audit(tables: dict[str, bytes]) -> dict:
    parsed = {name: records(tables[name], name) for name in FORMATS}
    armor = unique_records(parsed["armorSeriesData.asd"], "armor series")
    categories = unique_records(parsed["itemCategoryTypeData.ict"], "categories")
    items = unique_records(parsed["itemData.itm"], "items")
    members = defaultdict(list)
    item_categories = {}
    for item_id, row in items.items():
        points = row[27]
        item_categories[item_id] = sorted(set(struct.unpack_from("<8H", row, 28)) - {0})
        # Eight category IDs share the item's native point value.
        for category in item_categories[item_id]:
            if category not in categories:
                raise ValueError(f"item {item_id}: unknown category {category}")
            if points:
                members[category].append({"item_id": item_id, "points": points})

    crafting = defaultdict(list)
    for slot in range(5):
        for row in parsed[f"armorCreateA{slot:02}.acrd"]:
            armor_ids = set(struct.unpack_from("<4H", row)) - {0}
            materials = []
            for offset in (10, 14, 18, 22):
                item_id, quantity, flags = struct.unpack_from("<HBB", row, offset)
                if not item_id:
                    continue
                if item_id not in items or not quantity:
                    raise ValueError(f"crafting: invalid item/quantity {item_id}/{quantity}")
                materials.append((flags, item_categories[item_id]))
            for armor_id in armor_ids:
                if armor_id not in armor:
                    raise ValueError(f"crafting: unknown armor {armor_id}")
                crafting[armor_id].extend(materials)

    upgrades = defaultdict(list)
    for row in parsed["armorProcessA00.apd"]:
        # These are four armor IDs, NOT one u32 recipe-group ID. The ASD
        # field at +4 is not a foreign key into these rows.
        armor_ids = set(struct.unpack_from("<4H", row)) - {0}
        level, = struct.unpack_from("<H", row, 8)
        category, points = struct.unpack_from("<HH", row, 22)
        if not category:
            continue
        if category not in categories or categories[category][6] not in (0, 1, 2):
            raise ValueError(f"upgrade: unknown category/rank {category}")
        if not points or not members[category]:
            raise ValueError(f"upgrade: empty material recipe {category}")
        for armor_id in armor_ids:
            if armor_id not in armor:
                raise ValueError(f"upgrade: unknown armor {armor_id}")
            upgrades[armor_id].append((level, category))

    result = []
    for armor_id in sorted(set(armor) - {0}):
        rows = sorted(upgrades[armor_id])
        ranges = armor_ranges(armor_id, armor[armor_id])
        slot_mask = sum(1 << slot for slot in range(5) if armor[armor_id][6 + slot])
        rank_recipes = []
        for minimum, maximum, tier in ranges:
            category = recipe_category(armor_id, tier, rows, crafting[armor_id],
                                       categories, members, maximum)
            rank_recipes.append({"min_level": minimum, "max_level": maximum,
                                 "rank": tier, "category_id": category})
        if not slot_mask:
            rank_recipes = []
        result.append({"armor_id": armor_id, "slot_mask": slot_mask, "rank_recipes": rank_recipes})
    return {
        "schema": 2,
        "source_sha256": {name: hashlib.sha256(tables[name]).hexdigest() for name in FORMATS},
        "armor": result,
        "categories": [
            {"category_id": cat, "material_rank": RANKS[categories[cat][6]],
             "items": sorted(values, key=lambda x: x["item_id"])}
            for cat, values in sorted(members.items()) if categories[cat][6] in (0, 1, 2)
        ],
    }


def binary_catalog(result: dict) -> bytes:
    """AFUS v2: 48-byte header, 12-byte armor entries and 8-byte members.

    Header <4sIII32s>: magic, version2, armor-entry count, member count, digest.
    Armor <HBBBBHHH>: ID, min/max display level, rank1..3, slot mask bits0..4,
    category ID, required points, reserved0. Category/points0 means base-only.
    Members <HHB3x>: category ID, item ID, unchanged native point value.
    Entries sorted by (armor ID, minimum level); ranges never overlap.
    """
    ranks = {rank: i + 1 for i, rank in enumerate(RANKS)}
    categories = {row["category_id"]: row for row in result["categories"]}
    entries, used = [], set()
    for row in result["armor"]:
        previous = 0
        for recipe in row["rank_recipes"]:
            rank, category_id = recipe["rank"], recipe["category_id"]
            minimum, maximum = recipe["min_level"], recipe["max_level"]
            if not previous < minimum <= maximum <= 255 or rank not in ranks or not row["slot_mask"]:
                raise ValueError(f"invalid armor level interval {row['armor_id']}")
            previous = maximum
            if category_id:
                if category_id not in categories or categories[category_id]["material_rank"] != rank:
                    raise ValueError(f"armor {row['armor_id']}: invalid recipe category/rank")
                used.add(category_id)
            entries.append(struct.pack("<HBBBBHHH", row["armor_id"], minimum, maximum,
                                       ranks[rank], row["slot_mask"], category_id,
                                       10 if category_id else 0, 0))
    materials = [struct.pack("<HHB3x", category_id, item["item_id"], item["points"])
                 for category_id in sorted(used) for item in categories[category_id]["items"]]
    digest = hashlib.sha256(b"".join(bytes.fromhex(value) for _, value in
                                   sorted(result["source_sha256"].items()))).digest()
    return b"AFUS" + struct.pack("<III", 2, len(entries), len(materials)) + digest + b"".join(entries + materials)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("table_dir", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--binary", type=Path)
    args = parser.parse_args()
    result = audit({name: (args.table_dir / name).read_bytes() for name in FORMATS})
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    if args.binary:
        args.binary.write_bytes(binary_catalog(result))
    ready = sum(any(r["category_id"] for r in a["rank_recipes"]) for a in result["armor"])
    base_only = sum(bool(a["rank_recipes"]) and not any(r["category_id"] for r in a["rank_recipes"])
                    for a in result["armor"])
    print(f"{ready} appearance-capable armor IDs; {base_only} base-only; "
          f"{len(result['armor']) - ready - base_only} unused/unresolved")


if __name__ == "__main__":
    main()
