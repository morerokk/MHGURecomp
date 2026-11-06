# Early Armor Fusion

Implemented and verified on the native Linux runtime with MHGU 1.4.0.
The automated catalog, policy, settings-regression, and packaging checks pass. A disposable copy of an HR1 character was used for paid fusion,
cancellation, exact inventory and zenny checks, native separation, and a cold
save/load round trip. Windows and Android runtime testing remains outstanding.

## Behavior and scope

The shared forge menu gains an **Armor Fusion** action that opens the native
hunter fusion and separation interface. The hook targets the shared smithy
implementation used by village and hub forges. Bherna and Kokoto were tested
directly; other forge locations use that same implementation.

Hunter fusion requires matching armor tiers: LR/LR, HR/HR, or G/G. The player
chooses materials worth at least 10 points from the appearance armor's native
material category. Native item point values apply. Each item quantity stops at
the quantity needed to cover the remaining points, rounded up and limited by
current ownership. Only that rounding can exceed ten points. The game retains
its native zenny quote and fee.

The adapter keeps the native compatibility validator, equipped-armor handling,
equipment mutation, appearance representation, and separation operation. It
removes only the late category and special-armor fusion progression restrictions
in the new forge-owned hunter fusion context. It does not grant persistent progression flags or unlock other
Lab features. Palico fusion remains outside this feature.

## Armor tiers and recipes

For conventional armor, the catalog maps native armor rarity 1..3 to LR,
4..7 to HR, and 8..10 to G. These bands follow the armor lists' tier groupings
in [MHGU Armor](https://monsterhunterwiki.org/wiki/MHGU/Armor) and
[MHGU G Rank Armor](https://monsterhunterwiki.org/wiki/MHGU/Armor/G_Rank).
The catalog uses these conventional bands with explicit Special Permit
exceptions.

Special Permit armor has separate level intervals:

| Armor family | Displayed armor level | Fusion tier |
| --- | --- | --- |
| Original twelve Deviants, paired series IDs 617..640 | 1..10 | HR |
| Original twelve Deviants, paired series IDs 617..640 | 11..15 | G |
| Rustrazor, Nightcloak, Soulseer, Boltreaver, Elderfrost, Bloodbath; paired series IDs 1108..1119 | 1..5 | G |

The original Special Permit armor's native key materials and upgrade recipes
establish its HR and G intervals. The catalog checks the expected native
Special Permit metadata before applying those intervals. A quest's LR rating
does not make the resulting armor LR. The
[Special Permit quest list](https://monsterhunter.fandom.com/wiki/MHGU:_Special_Permit_Quests)
also distinguishes the six families that begin at G1.

For a tier, the generator prefers the final native point recipe within that
tier and level interval. This avoids generic early Fang/Claw categories when a
later recipe uses the armor monster's category. If no point recipe applies,
the generator tries the native crafting key materials, then the first eligible
same-tier crafting-material category in native slot/ingredient order.

Eight series with ambiguous crafting categories use explicit monster mappings
checked against their native crafting ingredients:

| Appearance armor | Native material category |
| --- | --- |
| Luxury Bath Towel, ID 422 | HR Blangonga, category 105 |
| Jaggi Mask X, ID 778 | G Jaggi, category 40 |
| Bistro X, IDs 879/880 | G Congalala, category 322 |
| Chaos/Nephilim, IDs 1044..1047 | Native G S. Magala, category 259 |

The native G S. Magala category accepts both Chaotic Gore and Shagaru
materials. Chaos/Nephilim reuse that existing shared category and its point
values.

The remaining 30 award or cosmetic series have no associated monster category.
They use the game's existing LR, HR, or G Monster category, IDs 1, 2, and 3,
with its native item membership and point values. This fallback applies only
when no native recipe/category applies; it does not replace an existing monster
recipe.

The current MHGU 1.4.0 audit covers 1,282 appearance-capable armor series IDs,
including the 24 progressive Special Permit IDs, with zero base-only IDs.
The four excluded IDs, 513/514/737/779, have no armor slots (`slot_mask == 0`)
and remain unused. These counts describe catalog coverage, not distinct armor
pieces or proof that the native validator accepts each pair. AFUS retains the
base-only representation for incomplete or unsupported input data.

## Implementation and catalog format

- `tools/fusion_materials.py` reads the builder's own native armor, crafting,
  category, and item tables. It writes the audit JSON and binary catalog.
  Source tables and generated game-data artifacts stay outside git.
- `runtime/armor_fusion_catalog.c` loads `armor-fusion.afus` beside the
  executable on desktop and from the unpacked app folder on Android. It looks
  up an armor's tier by series ID, slot, and displayed level, then supplies
  the appearance recipe. Material categories index immutable slices built
  while loading the sorted records; queries do not scan or copy the whole
  material table. Missing or unsupported lookups fail closed.
- `runtime/armor_fusion_policy.c` checks matching known tiers, slot compatibility,
  the native validator result, recipe ownership, distinct selected items,
  current inventory, and the 10-point minimum. It builds a read-only deduction
  plan and clears the plan on failure.
- `runtime/hle/fusion_materials.c` adapts the parent forge's native material-point
  widgets. Selection changes only scratch quantities until the final native
  Yes/No confirmation. No host renderer or overlay input capture is used.
- `runtime/hle/armorfusion.c` adds the forge action, opens the native coordinate
  UI, and intercepts hunter confirmation before the zenny charge. On acceptance
  it rereads both pieces, recalculates the quote and recipe, and rechecks current
  inventory and native compatibility. It substitutes the approved material
  deductions at the native token-consumption call, then lets the native equipped
  handling and equipment commit continue. Closing the child UI restores the
  suspended forge's facility flags when the scene still matches.
- Confirmation uses explicit material-selection, button-release, final-question
  and approved-payment phases. Approval is consumed by the synchronous native
  transaction. Both adapters share `runtime/hle/guest_call.h` to preserve the
  caller's sixteen general registers while returning the guest result.

AFUS version 2 uses little-endian records:

| Record | Layout | Meaning |
| --- | --- | --- |
| Header, 48 bytes | `<4sIII32s>` | `AFUS`, version 2, armor-entry count, member count, SHA-256 fingerprint of the source-table hashes |
| Armor entry, 12 bytes | `<HBBBBHHH>` | Armor ID, min/max displayed level, tier 1..3, slot bitmask, category ID, required points, reserved zero |
| Category member, 8 bytes | `<HHB3x>` | Category ID, item ID, unchanged native point value, padding |

Armor entries sort by armor ID and minimum level; intervals do not overlap.
An entry with category and required points both zero is base-only. Appearance
entries require ten points. Category members sort by category and item ID.
Armor slot bits correspond to slots 1..5. The header fingerprint records build
provenance; the runtime loader currently validates format and bounds rather
than comparing that fingerprint with the loaded game's tables.

The Makefile regenerates the catalog if it is missing or the source tables
change. Desktop release archives include it. Android packages it as an asset
and unpacks it into the app folder at startup. The retired English names file
is no longer required or packaged; the game supplies the native UI's labels.

## Native material picker

The point picker reuses the native forge widgets that normal weapon upgrades use:
`cUIOShopSmithCategory` (child 14), `cUIOShopSmithCategoryList` (child 15),
the item grid (child 16), and quantity popup (child 17). The game renders item
icons, localized names, native point values, owned counts, selected provisions
and the required-point indicator. No host presentation buffer is needed.

The forge is suspended beneath the native fusion task. A synthetic recipe row
contains the appearance category and ten required points, with no fixed crafting
ingredients. The adapter invokes only selection helpers, never the ordinary
forge update or its crafting/upgrade commit states. Selected material pairs are
converted back to the existing fusion validation plan before the native fusion
fee and equipment commit.

The native available pool has 128 item slots and the fusion policy permits ten
distinct selected materials. Quantity controls stop at the remaining point
requirement, allowing the native rounding overpayment for a material worth more
than the remaining points. Selection and undo affect scratch quantities only.
Cancellation restores the parent forge's touched state, models and visibility;
parent teardown discards that snapshot instead of writing into destroyed UI.

Money and preview widgets in the forge and fusion screens share native renderer
resources. Hide only currently visible forge widgets through the native
conditional-hide helper; hiding an already hidden alias can close the active
fusion screen's resource. Capture all original fusion visibility and restore it
after the picker. Preserve the preview frame, funds and armor-type header while
selecting materials. The common message and Yes/No controls have their own
visibility snapshot and are explicitly shown for the final fee confirmation.

## Native anchors, MHGU 1.4.0

Addresses are guest virtual addresses with main loaded at `0x200000`.

| Address | Role |
| --- | --- |
| `0x8f1ae8` | Shared smithy update; the adapter adds a fifth main-menu row and uses the native five-row list layout |
| `0x20f248` → `0x85fcec` → `0x5ea218` | Original Lab allocation, facility construction and native task registration; the forge remains suspended underneath |
| `0x860314`, `0x86549c` | Coordinate UI update and close |
| `0x340b7c` | Equipment validity; catalog-recognized hunter armor clears only category/special progression bits `0x18` in the forge-owned context |
| `0x340d60` | Pair validator; result `1` means compatible |
| `0x341060` | Native quote; the adapter retains zenny and removes the fixed fusion-token quote in its scope |
| `0x74fd94`, caller return `0x860e7c` | Confirmation interception before payment |
| `0x75bac0`, caller return `0x842a48`, tuple `{1,37,43}` | Fusion-cost question text; the unrelated Equip now question remains native |
| `0x723180`, `0x723150` | Zenny availability check and native charge |
| `0x74fe8c`, `0x74ff80`, caller return `0x860eb0` | Native item count and consumption; the adapter replaces this token deduction with the approved material plan |
| `0x348f00` | Native equipment commit, retained through the original confirmation flow |
| `0x8fcbcc`, `0x8fa3d8` | Native category pool and requirement display, using a synthetic zero-fixed-material recipe |
| `0x8fb8e4`, `0x8fcea8` | Native available and selected-material display refresh |
| `0x8fd448`, `0x8fb720` | Scratch selection add and undo; never inventory consumption |
| `0x74fa74` | Native quantity controls; button-event output points at forge `+0x320c` |
| `0x85cc0c` | Conditional child hide, preserving shared renderer resources held by another UI |

The adapter checks selected guest function prologues before installing hooks.
The forge path does not call the equipment commit as a standalone transaction.

## Verification

From the repository root:

```sh
make test-armor-fusion
make fusion-catalog GAME=/path/to/your/game
python3 tools/fusion_materials.py /path/to/your/game/romfs/nativeNX/table \
  --output /tmp/mhgu-fusion-audit.json --binary /tmp/armor-fusion.afus
```

The 15 Python metadata tests and the C checks exercise policy failures,
native rejection, changed inventory, malformed tables, paired series IDs,
conventional and Special Permit tiers, AFUS layout, and base-only entries.
The distribution check covers the generated catalog. Existing settings and
settings-panel checks remain separate from the native fusion UI.

The native adapter host test enters through the real registered hooks and runs
the real deduction policy against stubbed guest functions and a guest-memory
fixture. It covers cancellation, held-button release, stale quotes/equipment/
inventory, scope guards, teardown, single-refresh partial selections and native
point rounding. The guest-call test checks register preservation, including
nested calls. These host tests do not replace interactive game verification.

For a runtime smoke test, keep the executable and generated catalog
together, use a disposable copy of the save, and point only the game-data path
at the original dump:

```sh
XDG_DATA_HOME=/tmp/mhgu-fusion-user MHGU_DATA_DIR=/path/to/your/game \
  MHGU_FUSION_TRACE=1 ./build/mhgurecomp
```

`MHGU_GAME_DIR` changes game-directory/save-path selection and is unsuitable for
this isolated XDG-save setup. Copy the intended save into the runtime's
`MHGURecomp/save/save_0` directory under the chosen XDG data root before testing.

Verified in the native Linux game on a copied HR1 save:

- The five-row Bherna and Kokoto forge menus open the native hunter
  fusion/separation menu. Returning and reopening leaves the smithy usable.
- The fusion question names monster materials, while the native Equip now
  question is left unchanged.
- Equipped Bulldrome Helm Lv5 accepts a Jaggi Helm Lv1 appearance using
  five Jaggi Scales worth two native points each and the native 2,000z fee.
  Only those five scales and 2,000z are deducted.
- The 44-byte base armor record changes only its native appearance field;
  armor identity, upgrade level, and every other byte remain unchanged. The
  native operation consumes the appearance piece into that representation.
- Cancelling after editing quantities leaves inventory and both armor records
  unchanged. The picker clamps quantities to current ownership and waits for
  ten points before showing the native fee Yes/No confirmation.
- Saving through the game and restarting the executable preserves the exact
  paid inventory and equipment records, including the visible appearance.
- Native separation restores both original armor records byte for byte and
  does not consume or refund crafting materials.
- A second fusion accepts five Jaggi Hides instead, confirming that material
  selection controls the actual deduction rather than a fixed hidden recipe.
- The native picker displays Jaggi material icons, owned counts and values with
  a ten-point requirement. Cancelling quantity editing preserves all items,
  equipment and zenny. Holding Confirm across the quantity-to-confirmation
  transition does not authorize payment.
- The native final Yes/No displays the actual 2,000z fee. Choosing No spends
  nothing; choosing Yes after selecting one then four more Jaggi Scales
  merges them into exactly one five-scale deduction.
- Returning to normal weapon upgrading restores its own recipe: Petrified
  Blade Lv1 to Lv2 again shows Ore materials worth four points, with the
  original owned quantities, rather than the fusion category or requirement.
- A decorated Bulldrome Braces Lv5 accepts a Bherna Braces Lv1 appearance
  using two Kelbi Horns worth five points each and the native 2,000z fee.
  The base's 44-byte record changes only its appearance field. Its Attack
  Jewel 1, derived skill points, active skills, attack, defense, health and
  resistances remain unchanged. Saving and restarting preserves the fused
  decorated arms and the paid inventory.
- Two Linux clients join a private direct Local Play hub. The observer runs
  the same runtime with the early-fusion hooks disabled by omitting the catalog.
  It sees the host's Jaggi Helm and Bherna Braces appearances while
  keeping its own unfused equipment. All seven received equipment records
  match the host's stable fields, decorations and derived skill points;
  received stats and active skills match too. Guest pointers are compared by
  their contents, rather than by their addresses in the two processes.

These live checks used the feature build before the catalog and confirmation
cleanup. Post-cleanup checks cover the catalog, deduction policy, preserved
guest-call registers, native control transitions and the full Linux build;
the interactive forge transactions above have not been repeated after cleanup.

A post-cleanup native Linux smoke test used a separate executable folder with
only the binary catalog and a copied save. Bherna's forge opens the fusion
menu and the localized material picker; a partial selection updates the points,
undoing it restores the pool, and holding Confirm across the transition leaves
the native 2,000z question open. Cancelling returns to a usable forge, and the
fusion menu opens again. No payment was confirmed in this smoke test.

The copied character has LR equipment. HR/G and Special Permit tier boundaries,
mixed-tier rejection, and changed-inventory rejection are covered by metadata
and policy tests, rather than native transactions with high-rank equipment.
The decorated appearance donor's jewel return and a full item box have not
been exercised directly. Shared-quest multiplayer, cross-platform multiplayer,
Windows/Android runtime, and every individual forge location remain untested.
