"""Audit native material-point recipes from a player's MHGU 1.4.0 dump.

Normal armor uses MHGU's conventional armor tiers: rarity 1..3 LR, 4..7 HR,
8..10 G. Original Special Permit armor is HR at levels 1..10 and G at 11..15,
matching its native crafting material tiers. The six G-only Deviant families
are G throughout. A quest's LR classification does not define the armor tier.
An upgrade's material rank never defines
the armor tier. No copyrighted tables are embedded; dumps/saves are read-only.
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


def recipe_choices(rank, rows, crafts, categories, members, maximum_level=255):
    matching = [r for r in rows if r["material_rank"] == rank and r["level"] <= maximum_level]
    # The final point recipe within a tier uses the armor monster's own
    # category. Earlier recipes can instead use generic Fang/Claw categories,
    # particularly for Special Permit armor.
    tier_matching = [r for r in matching if r["level"] == matching[-1]["level"]] if matching else []
    choices = sorted({r["category_id"] for r in tier_matching})
    # Reuse a native point recipe of the armor's tier, including later
    # upgrades: Leather S initially uses LR materials but also has an HR
    # recipe. If there is no such recipe, use a uniquely eligible category
    # from the crafting key materials; never select an arbitrary category.
    if not choices and rank:
        key_categories = {cat for craft in crafts for mat in craft["materials"]
                          if mat["flags"] & 2 for cat in mat["category_ids"]
                          if categories[cat][6] in (0, 1, 2) and
                          ("LR", "HR", "G")[categories[cat][6]] == rank and members[cat]}
        choices = sorted(key_categories)
    if not choices and rank:
        # Non-monster/ticket armor frequently has no point upgrade recipe
        # and its ticket has no category. Reuse the first eligible material
        # category in its native crafting recipe (slot order, then native
        # ingredient order); its item point values remain unchanged.
        primary = next((cat for craft in crafts for mat in craft["materials"]
                        for cat in mat["category_ids"]
                        if categories[cat][6] in (0, 1, 2) and
                        ("LR", "HR", "G")[categories[cat][6]] == rank and members[cat]), None)
        choices = [primary] if primary is not None else []
    return choices


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
        for category in sorted(set(struct.unpack_from("<8H", row, 28)) - {0}):
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
                materials.append({"item_id": item_id, "quantity": quantity,
                                  "flags": flags, "category_ids": item_categories[item_id]})
            ranks = {categories[category][6] for mat in materials
                     for category in mat["category_ids"] if categories[category][6] in (0, 1, 2)}
            for armor_id in armor_ids:
                if armor_id not in armor:
                    raise ValueError(f"crafting: unknown armor {armor_id}")
                crafting[armor_id].append({
                    "slot": slot + 1, "materials": materials,
                    "material_rank_lower_bound": ("LR", "HR", "G")[max(ranks)] if ranks else None,
                })

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
            upgrades[armor_id].append({
                "level": level, "category_id": category,
                "material_rank": ("LR", "HR", "G")[categories[category][6]],
                "native_points": points,
            })

    result = []
    for armor_id in sorted(set(armor) - {0}):
        rows = sorted(upgrades[armor_id], key=lambda r: (r["level"], r["category_id"]))
        rarity = armor[armor_id][103] + 1
        rank = "LR" if rarity <= 3 else "HR" if rarity <= 7 else "G" if rarity <= 10 else None
        if armor_id in G_ONLY_SPECIAL_PERMIT:
            if rarity != 11 or armor[armor_id][105] != 1:
                raise ValueError(f"armor {armor_id}: changed G-only Special Permit metadata")
            rank = "G"
        slot_mask = sum(1 << slot for slot in range(5) if armor[armor_id][6 + slot])
        ranges = [(1, 255, rank)] if rank else []
        if 617 <= armor_id <= 640:
            if rarity != 11 or armor[armor_id][105] != 5:
                raise ValueError(f"armor {armor_id}: changed progressive Special Permit metadata")
            # Special Permit quests can start at LR, but their level-I armor
            # key tickets and monster materials already belong to native HR
            # categories. Do not infer armor rank from the quest's star rank.
            ranges = [(1, 10, "HR"), (11, 15, "G")]
        elif armor_id in G_ONLY_SPECIAL_PERMIT:
            ranges = [(1, 5, "G")]
        rank_recipes = []
        for minimum, maximum, tier in ranges:
            tier_choices = recipe_choices(tier, rows, crafting[armor_id], categories, members, maximum)
            source = "native_recipe"
            if len(tier_choices) != 1 and armor_id in SPECIFIC_CRAFT_CATEGORIES:
                specific = SPECIFIC_CRAFT_CATEGORIES[armor_id]
                eligible = {cat for craft in crafting[armor_id] for mat in craft["materials"]
                            for cat in mat["category_ids"]}
                if (specific not in eligible or specific not in categories or not members[specific]
                        or ("LR", "HR", "G")[categories[specific][6]] != tier):
                    raise ValueError(f"armor {armor_id}: changed specific crafting category")
                tier_choices = [specific]
                source = "specific_craft_category"
            if not tier_choices:
                # Award/ticket/cosmetic armor without an associated native
                # point recipe uses the existing rank's Monster category.
                # Preserve its native item membership and point values.
                generic = {"LR": 1, "HR": 2, "G": 3}[tier]
                if (generic in categories and members[generic] and
                        ("LR", "HR", "G")[categories[generic][6]] == tier):
                    tier_choices = [generic]
                    source = "generic_rank_fallback"
            rank_recipes.append({"min_level": minimum, "max_level": maximum,
                                 "rank": tier, "category_id": tier_choices[0] if len(tier_choices) == 1 else 0,
                                 "recipe_source": source})
        if not slot_mask:
            rank_recipes = []
        effective_category = (rank_recipes[0]["category_id"]
                              if len(rank_recipes) == 1 else None)
        result.append({
            "armor_id": armor_id,
            "status": ("candidate" if effective_category else "progressive"
                       if any(r["category_id"] for r in rank_recipes) else "base_only"
                       if rank_recipes else "unresolved"),
            "candidate_category_id": effective_category or None,
            "armor_rank": rank,
            "rank_recipes": rank_recipes,
            "rarity": rarity,
            "slot_mask": slot_mask,
            "rank_source": ("g_only_special_permit" if armor_id in G_ONLY_SPECIAL_PERMIT else
                            "native_special_permit_material_tiers" if 617 <= armor_id <= 640 else
                            "conventional_armor_tier" if rank else "special_permit_unresolved"),
            "fusion_points": 10,
            "upgrade_evidence": rows,
            "crafting_evidence": crafting[armor_id],
        })
    return {
        "schema": 2,
        "runtime_approved": True,
        "source_sha256": {name: hashlib.sha256(tables[name]).hexdigest() for name in FORMATS},
        "armor": result,
        "categories": [
            {"category_id": cat, "material_rank": ("LR", "HR", "G")[categories[cat][6]],
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
    ranks = {"LR": 1, "HR": 2, "G": 3}
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


def gmd_strings(path: Path) -> list[str]:
    raw = path.read_bytes()
    if len(raw) < 41 or raw[:4] != b"GMD\0":
        raise ValueError(f"{path}: invalid GMD header")
    offset = 41 + struct.unpack_from("<I", raw, 36)[0]
    if offset > len(raw) or raw[-1:] != b"\0":
        raise ValueError(f"{path}: invalid string payload")
    return [value.decode("utf-8") for value in raw[offset:-1].split(b"\0")]


def names_catalog(table_dir: Path, result: dict) -> str:
    """Name-only native strings; TSV columns kind, ID, slot, display name."""
    english = table_dir.parent / "eng" / "table"
    armor_names = gmd_strings(english / "armorSeriesData_eng.gmd")
    item_names = gmd_strings(english / "itemData_eng.gmd")
    category_names = gmd_strings(english / "itemCategoryTypeData_eng.gmd")
    lines = []
    def put(kind, ident, slot, value):
        value = " ".join(value.replace("\t", " ").replace("\r", " ").replace("\n", " ").split())
        if value and value != "(None)":
            lines.append(f"{kind}\t{ident}\t{slot}\t{value}\n")
    for row in result["armor"]:
        for slot in range(5):
            if row["slot_mask"] & (1 << slot):
                offset = row["armor_id"] * 10 + slot
                if offset >= len(armor_names):
                    raise ValueError("armor names: missing series/slot")
                put("A", row["armor_id"], slot + 1, armor_names[offset])
    for offset in range(0, len(item_names), 2):
        put("I", offset // 2, 0, item_names[offset])
    for row in result["categories"]:
        ident = row["category_id"]
        if ident >= len(category_names):
            raise ValueError("category names: missing ID")
        put("C", ident, 0, row["material_rank"] + " " + category_names[ident])
    return "".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("table_dir", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--names", type=Path)
    args = parser.parse_args()
    result = audit({name: (args.table_dir / name).read_bytes() for name in FORMATS})
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    if args.binary:
        args.binary.write_bytes(binary_catalog(result))
    if args.names:
        args.names.write_text(names_catalog(args.table_dir, result), encoding="utf-8")
    ready = sum(any(r["category_id"] for r in a["rank_recipes"]) for a in result["armor"])
    base_only = sum(bool(a["rank_recipes"]) and not any(r["category_id"] for r in a["rank_recipes"])
                    for a in result["armor"])
    print(f"{ready} appearance-capable armor IDs; {base_only} base-only; "
          f"{len(result['armor']) - ready - base_only} unused/unresolved")


if __name__ == "__main__":
    main()
