from pathlib import Path
import struct
import tempfile
import unittest

from dist import files

from fusion_materials import FORMATS, audit, binary_catalog, records


def table(name, rows):
    return FORMATS[name][0] + struct.pack("<I", len(rows)) + b"".join(rows)


def fixture():
    armor = []
    for armor_id in (150, 151):
        row = bytearray(127)
        struct.pack_into("<IH", row, 0, armor_id, armor_id + 2)
        row[6] = 1
        armor.append(row)
    categories = []
    for cat, rank in ((197, 0), (198, 1)):
        row = bytearray(7)
        struct.pack_into("<I", row, 0, cat)
        row[6] = rank
        categories.append(row)
    item = bytearray(44)
    struct.pack_into("<I", item, 0, 1202)
    item[27] = 2
    struct.pack_into("<H", item, 28, 197)
    upgrade = bytearray(38)
    struct.pack_into("<4H", upgrade, 0, 150, 151, 0, 0)
    struct.pack_into("<H", upgrade, 8, 4)
    struct.pack_into("<HH", upgrade, 22, 197, 10)
    result = {
        "armorSeriesData.asd": table("armorSeriesData.asd", armor),
        "armorProcessA00.apd": table("armorProcessA00.apd", [upgrade]),
        "itemCategoryTypeData.ict": table("itemCategoryTypeData.ict", categories),
        "itemData.itm": table("itemData.itm", [item]),
    }
    for slot in range(5):
        name = f"armorCreateA{slot:02}.acrd"
        result[name] = table(name, [])
    craft = bytearray(42)
    struct.pack_into("<4H", craft, 0, 150, 151, 0, 0)
    struct.pack_into("<HBB", craft, 10, 1202, 3, 2)
    result["armorCreateA00.acrd"] = table("armorCreateA00.acrd", [craft])
    return result


class FusionMaterialsTest(unittest.TestCase):
    def with_generic_category(self, data):
        category = bytearray(7)
        struct.pack_into("<I", category, 0, 1)
        categories = [data["itemCategoryTypeData.ict"][i:i + 7]
                      for i in range(8, len(data["itemCategoryTypeData.ict"]), 7)]
        data["itemCategoryTypeData.ict"] = table("itemCategoryTypeData.ict", categories + [category])
        item = bytearray(44)
        struct.pack_into("<I", item, 0, 1210)
        item[27] = 3
        struct.pack_into("<H", item, 28, 1)
        data["itemData.itm"] = table("itemData.itm", [data["itemData.itm"][8:], item])
        return data

    def test_paired_ids_not_series_display_order(self):
        result = audit(fixture())
        self.assertEqual([a["rank_recipes"][0]["category_id"] for a in result["armor"]], [197, 197])
        self.assertEqual(result["categories"][0]["items"], [{"item_id": 1202, "points": 2}])
        self.assertTrue(all(a["rank_recipes"][0]["rank"] == "LR" for a in result["armor"]))

    def test_reject_truncated_or_extra_bytes(self):
        for name, data in fixture().items():
            for broken in (data[:7], data[:-1], data + b"x"):
                with self.subTest(name=name, size=len(broken)), self.assertRaises(ValueError):
                    records(broken, name)

    def test_reject_unsupported_version(self):
        for name, data in fixture().items():
            with self.subTest(name=name), self.assertRaises(ValueError):
                records(b"BAD!" + data[4:], name)

    def test_reject_unknown_material_category(self):
        data = fixture()
        item = bytearray(data["itemData.itm"])
        struct.pack_into("<H", item, 8 + 28, 999)
        data["itemData.itm"] = item
        with self.assertRaisesRegex(ValueError, "unknown category"):
            audit(data)

    def test_missing_point_recipe_uses_crafting_key_category(self):
        data = fixture()
        data["armorProcessA00.apd"] = table("armorProcessA00.apd", [])
        result = audit(data)
        self.assertTrue(all(a["rank_recipes"][0]["category_id"] == 197 for a in result["armor"]))

    def test_crafting_material_rank_does_not_change_armor_tier(self):
        data = fixture()
        item = bytearray(data["itemData.itm"])
        struct.pack_into("<I", item, 8, 1203)
        struct.pack_into("<H", item, 8 + 28, 198)
        original = data["itemData.itm"][8:]
        data["itemData.itm"] = table("itemData.itm", [original, item[8:]])
        craft = bytearray(data["armorCreateA00.acrd"][8:])
        struct.pack_into("<H", craft, 10, 1203)
        data["armorCreateA00.acrd"] = table("armorCreateA00.acrd", [craft])
        row = audit(data)["armor"][0]
        self.assertEqual(row["rank_recipes"], [{"min_level": 1, "max_level": 255,
                                                "rank": "LR", "category_id": 197}])

    def test_conflicting_categories_stay_unresolved(self):
        data = self.with_generic_category(fixture())
        categories = bytearray(data["itemCategoryTypeData.ict"])
        categories[8 + 7 + 6] = 0
        data["itemCategoryTypeData.ict"] = categories
        item = bytearray(data["itemData.itm"])
        struct.pack_into("<H", item, 8 + 30, 198)
        data["itemData.itm"] = item
        row = data["armorProcessA00.apd"][8:]
        second = bytearray(row)
        struct.pack_into("<H", second, 22, 198)
        data["armorProcessA00.apd"] = table("armorProcessA00.apd", [row, second])
        self.assertTrue(all(a["rank_recipes"][0]["category_id"] == 0 for a in audit(data)["armor"]))

    def test_binary_layout_and_counts(self):
        binary = binary_catalog(audit(fixture()))
        self.assertEqual(binary[:4], b"AFUS")
        self.assertEqual(struct.unpack_from("<III", binary, 4), (2, 2, 1))
        self.assertEqual(struct.unpack_from("<HBBBBHHH", binary, 48), (150, 1, 255, 1, 1, 197, 10, 0))
        self.assertEqual(struct.unpack_from("<HHB", binary, 72), (197, 1202, 2))
        self.assertEqual(len(binary), 80)

    def test_special_permit_tier_is_unresolved(self):
        data = fixture()
        armor = bytearray(data["armorSeriesData.asd"])
        armor[8 + 103] = 10
        data["armorSeriesData.asd"] = armor
        row = audit(data)["armor"][0]
        self.assertEqual(row["rank_recipes"], [])

    def test_no_point_recipe_remains_base_only(self):
        data = fixture()
        data["armorProcessA00.apd"] = table("armorProcessA00.apd", [])
        data["armorCreateA00.acrd"] = table("armorCreateA00.acrd", [])
        binary = binary_catalog(audit(data))
        self.assertEqual(struct.unpack_from("<III", binary, 4), (2, 2, 0))
        self.assertEqual(struct.unpack_from("<HBBBBHHH", binary, 48), (150, 1, 255, 1, 1, 0, 0, 0))

    def test_g_only_special_permit_stays_g(self):
        data = fixture()
        armor = bytearray(data["armorSeriesData.asd"])
        struct.pack_into("<I", armor, 8, 1108)
        armor[8 + 103] = 10
        armor[8 + 105] = 1
        data["armorSeriesData.asd"] = armor
        # No upgrade/craft row for this fixture's replacement ID.
        data["armorProcessA00.apd"] = table("armorProcessA00.apd", [])
        data["armorCreateA00.acrd"] = table("armorCreateA00.acrd", [])
        row = next(a for a in audit(data)["armor"] if a["armor_id"] == 1108)
        self.assertEqual(row["rank_recipes"], [{"min_level": 1, "max_level": 5,
                                                "rank": "G", "category_id": 0}])

    def test_classic_special_permit_ranges_are_disjoint(self):
        for ident, expected in (
            (617, [(1, 10, "HR"), (11, 15, "G")]),
            (625, [(1, 10, "HR"), (11, 15, "G")]),
        ):
            data = fixture()
            armor = bytearray(data["armorSeriesData.asd"])
            struct.pack_into("<I", armor, 8, ident)
            armor[8 + 103], armor[8 + 105] = 10, 5
            data["armorSeriesData.asd"] = armor
            data["armorProcessA00.apd"] = table("armorProcessA00.apd", [])
            data["armorCreateA00.acrd"] = table("armorCreateA00.acrd", [])
            row = next(a for a in audit(data)["armor"] if a["armor_id"] == ident)
            self.assertEqual([(r["min_level"], r["max_level"], r["rank"])
                              for r in row["rank_recipes"]], expected)
            for level in (1, 3, 4, 10, 11, 15):
                matches = [r for r in row["rank_recipes"] if r["min_level"] <= level <= r["max_level"]]
                self.assertEqual(len(matches), 1)

    def test_missing_native_recipe_uses_existing_generic_rank_points(self):
        data = self.with_generic_category(fixture())
        data["armorProcessA00.apd"] = table("armorProcessA00.apd", [])
        data["armorCreateA00.acrd"] = table("armorCreateA00.acrd", [])
        result = audit(data)
        recipe = result["armor"][0]["rank_recipes"][0]
        self.assertEqual(recipe["category_id"], 1)
        generic = next(c for c in result["categories"] if c["category_id"] == 1)
        self.assertEqual(generic["items"], [{"item_id": 1210, "points": 3}])

    def test_generic_fallback_never_overrides_monster_recipe(self):
        result = audit(self.with_generic_category(fixture()))
        recipe = result["armor"][0]["rank_recipes"][0]
        self.assertEqual(recipe["category_id"], 197)

    def test_specific_craft_category_resolves_blangonga_ambiguity(self):
        data = fixture()
        armor = bytearray(data["armorSeriesData.asd"])
        struct.pack_into("<I", armor, 8, 422)
        armor[8 + 103] = 3
        data["armorSeriesData.asd"] = armor
        categories = bytearray(data["itemCategoryTypeData.ict"])
        struct.pack_into("<I", categories, 8, 105)
        struct.pack_into("<I", categories, 15, 102)
        categories[14], categories[21] = 1, 1
        data["itemCategoryTypeData.ict"] = categories
        item = bytearray(data["itemData.itm"])
        struct.pack_into("<HH", item, 8 + 28, 102, 105)
        data["itemData.itm"] = item
        craft = bytearray(data["armorCreateA00.acrd"])
        struct.pack_into("<H", craft, 8, 422)
        data["armorCreateA00.acrd"] = craft
        data["armorProcessA00.apd"] = table("armorProcessA00.apd", [])
        row = next(a for a in audit(data)["armor"] if a["armor_id"] == 422)
        self.assertEqual(row["rank_recipes"][0]["category_id"], 105)


    def test_catalog_is_packaged_without_names(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ("mhgurecomp", "mhgugame.so", "mhgurecomp-updater", "pipelines.list",
                         "armor-fusion.afus"):
                (root / name).write_bytes(b"fixture")
            (root / "licenses").mkdir()
            names = {entry[0] for entry in files(directory, "linux")}
            self.assertIn("armor-fusion.afus", names)
            self.assertNotIn("armor-fusion.names", names)
            (root / "armor-fusion.names").write_bytes(b"old labels")
            self.assertNotIn("armor-fusion.names", {entry[0] for entry in files(directory, "linux")})
            (root / "armor-fusion.afus").unlink()
            with self.assertRaises(SystemExit):
                files(directory, "linux")


if __name__ == "__main__":
    unittest.main()
