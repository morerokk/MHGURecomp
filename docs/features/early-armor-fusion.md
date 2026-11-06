# Early Armor Fusion

Every hunter forge gains an **Armor Fusion** action from the start. It opens
the game's native fusion/separation screens and material-point picker, including
localized names, item icons, quantities and the final zenny confirmation.
Palico fusion and other Lab features retain their existing progression rules.

Both armor pieces must have the same slot and tier: LR/LR, HR/HR or G/G.
Fusion requires at least ten points from the appearance armor's native material
category, plus the original zenny fee. Quantities stop at the remaining points,
rounded up and limited by ownership; only that rounding can exceed ten points.
Cancellation spends nothing. Separation keeps its native behavior.

## Tiers and materials

Conventional armor rarity 1..3 maps to LR, 4..7 to HR and 8..10 to G.
Original Special Permit series 617..640 are HR at levels 1..10 and G at
11..15; the six G-only families (series 1108..1119) stay G at levels 1..5.
The generator checks the native Special Permit metadata before applying these
exceptions.

Each tier uses the latest native point recipe within its level interval.
Without one, it tries a unique eligible key-material category, then the first
eligible crafting category in native ingredient order. Eight ambiguous series
have verified overrides: Luxury Bath Towel (422), Jaggi Mask X (778), Bistro X
(879/880), and Chaos/Nephilim (1044..1047). Armor with no category uses the
existing LR/HR/G Monster category; ambiguity never silently selects a fallback.

The MHGU 1.4.0 catalog covers 1,282 appearance-capable armor series IDs.
Unused IDs 513/514/737/779 have no armor slots. These counts describe metadata
coverage, not proof that the native validator accepts every pair.

## Implementation

- `runtime/armor_fusion.c` loads the binary catalog and validates deduction
  plans. Category members are immutable slices; missing or invalid data fails
  closed. The pure validator checks tiers, slots, native compatibility, recipe
  identity, distinct items, inventory and points, clearing rejected plans.
- `runtime/hle/armorfusion.c` adds the forge action and borrows its native
  material widgets. Quantities remain scratch state until confirmation.
  Confirmation rereads equipment, inventory, compatibility and the fee before
  charging. The approved deductions replace only the native token consumption;
  equipped handling, appearance mutation, saving and separation remain native.
- Explicit selection, button-release, question and approved phases prevent a
  held input from accepting the fee. Teardown checks ownership before restoring
  suspended widgets and preview lights. Only the active preview supplies light
  to the paper model, avoiding an unsupported two-light shader fallback.
  The guest-call helper in `runtime/rt.h` preserves registers.

`tools/fusion_materials.py` derives the catalog from the builder's own dump.
Only `armor-fusion.afus` is needed beside the executable; desktop archives and
Android assets include it. Extracted tables, generated catalogs and saves stay
outside git. No separate names file is needed.

AFUS v2 records are little endian, sorted by armor ID/level and category/item:

| Record | Layout | Contents |
| --- | --- | --- |
| Header (48 bytes) | `<4sIII32s>` | `AFUS`, version 2, armor/member counts, source-table fingerprint |
| Armor (12 bytes) | `<HBBBBHHH>` | ID, min/max level, tier 1..3, slot mask, category, points, reserved zero |
| Member (8 bytes) | `<HHB3x>` | Category, item, native point value, padding |

Armor intervals cannot overlap. Zero category/points means base-only;
appearance entries require ten points. The loader checks format and bounds,
but does not compare the provenance fingerprint to the loaded game tables.

## Build and verification

```sh
make fusion-catalog GAME=/path/to/your/game
make test-armor-fusion
```

The C suite covers policy, malformed catalogs, immutable lookups, guest-register
preservation and real native hooks against stubbed guest calls. Native cases
include cancellation, held inputs, changed quotes/equipment/inventory, parent
teardown, partial selection, rounding and caller scope. Python checks cover
table parsing, category precedence, tier boundaries, binary output and packaging.
Host tests do not replace in-game transactions.

Linux copied-save checks cover six paid fusion/separation cases: decorated LR
Bulldrome/Maccao, LR/HR/G Rathalos, HR Hero and G Hylian event armor. Exact gear,
material and zenny deltas were checked. Base decorations, stats and skills stay
unchanged; donor decorations return to the item box. All six mixed-rank
directions are grayed out and reject confirmation without payment.
Decorated LR fusion and separation each survive native Save and a cold process
restart with exact equipment, inventory, zenny and hunter-stat comparisons.

Solo and shared Local Play quest checks compare the same equipped base before
and after fusion. A second client with the extension disabled receives the
expected appearance and matching equipment, decorations, stats and skills.
Shared-quest checks also pass after merging upstream a272b61. HD texture/UI/font
mods were loaded with 2560x1440 internal rendering and windowed output.

Windows/Android runtime, WAN/NAT, four-player sessions, full-box separation and
every individual armor appearance/recipe remain outside this test coverage.
