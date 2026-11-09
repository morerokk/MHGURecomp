# Game-action rebinding: implementation notes (prototype of M1 and the non-hover part of M2)

Branch `input-impl` (on upstream 0.15.5+2). Written against `docs/input-rework/ACTIONS.md` of the research branch (section 4,
"prototype spec"). Everything here is **off** unless `[Actions] Enable=1` is in the ini: with it off nothing is hooked, no
guest code is touched and the only changes on the input path are two mutex-guarded copies in `hle/input.c`.

## What it is

The engine builds two views of the pad from one word (the pad's held word, in the engine's own bit layout): `f_5de4b0` (mb1, the
gameplay view: player, town avatar, camera) and `f_5de4f8` (mb2, the ~320 menu readers). `f_5de064` calls them through a vtable
slot and derives cur/prev/trig/rel from the result. Both are pure functions of `r1`. The action layer hooks them (`fnhook_set`, as
`itembar.c` / `mousecam.c` do): `r1 = act_view(view, r1, active actions)`, original, done. Chords, combos and key repeat then come
out of the engine's own edge logic. The Control Type options are never written: the dash action reads `LD8(options+0x446E)` and
delivers R or ZR (the game's own dash role) accordingly.

| file | what |
|---|---|
| `runtime/actions.h`, `actions.c` | registry (18 actions), input names, `[Actions*]` parse / format / validate, sampling of the bindings into the active set, `act_view`. Pure C (needs only `config.c`'s key names), host-testable |
| `runtime/hle/input_view.c`, `.h` | the game side: hooks `f_5de064`, `f_5de4b0`, `f_5de4f8`, the player update `f_4a79cc`, the town update `f_48d1f0` and the 15 weapon mappers; context, weapon class, current action, the optional pointer feed |
| `runtime/hle/input.c`, `.h` | the mailbox: the host stages the action set, `input_set` publishes it with the buttons, `GetNpadState` copies it into the set the game latches (so the buttons the engine saw and the actions come from the same host update) |
| `runtime/host_input.c` | sampler glue: physical state callbacks, the Tier 0 sources of each Switch button, wheel queue, control commands `act`, `ptr`, `actstat` |
| `runtime/config.c` | skips the `[Actions*]` sections (they are read by `actions.c`) so they don't log "unknown setting" |
| `tests/test_actions.c`, `make test-actions` | host-only tests (no game files) |

## Actions

mb1 (field and town): `Dodge` (B), `Dash` (the R-role: R, or ZR with the second Control Type), `AttackX` (X), `AttackA` (A),
`ItemUse` (Y), `ChordXA` (X and A in one frame). mb2 (menus, needs `ManageMenus=1`): `MenuConfirm` (A), `MenuCancel` (B),
`MenuUp/Down/Left/Right`, `MenuTabL/R` (L/R), `MenuX`, `MenuY`, `MenuPageL/R` (ZL/ZR). Weapon classes for per-class overrides:
GreatSword SnS Hammer Lance HBG LBG LongSword SwitchAxe Gunlance Bow DualBlades Horn InsectGlaive ChargeBlade Prowler.

## ini

```ini
[Actions]
Enable=1                  ; 0 (default, and no section at all): nothing hooked, bit-for-bit today's behaviour
ManageMenus=1             ; the menu view too
Pointer=0                 ; feed the engine's (dormant) pointer block: experiment, see P1 below
Log=0                     ; one line per frame (env MHGU_ACTION_LOG=1); env MHGU_ACTIONS=0|1 overrides Enable

[Actions.Pad]             ; pad:south east west north (positions), lshoulder rshoulder ltrigger rtrigger lstick rstick
Dodge   = pad:north       ;   dpup dpdown dpleft dpright start back
ItemUse = pad:east | pad:lshoulder+pad:rtrigger     ; up to 3 alternatives, '+' for a chord of up to 3 inputs
AttackA = None            ; None: unbound on purpose (an action not listed keeps what Tier 0 gives it)

[Actions.Kbm]             ; key:<SDL name, spaces and case ignored>  mouse:left right middle x1 x2 wheelup wheeldown
Dodge = key:space
MenuConfirm = mouse:left | key:return
MenuCancel  = mouse:right
MenuUp = mouse:wheelup
MenuDown = mouse:wheeldown

[Actions.Pad.Weapon.Hammer]   ; sparse override for one class, resolution: Weapon.<Class> > base > default
AttackA = pad:north
```

`sw:A`, `sw:Dash` etc. are also accepted: "what the settings' own controls (Tier 0) give that Switch button".
**Defaults are exactly Tier 0**: an action with no entry reads the Switch button Tier 0 gives it (`sw:` sources of the controller
and of `[Keybinds]`, scripted input included), so `Enable=1` with no bindings plays like `Enable=0` (golden test T0 below). A device
list overrides that device's half only: `[Actions.Pad] Dodge = ...` leaves the keyboard's Dodge on `[Keybinds]`' B. A physical
input bound to an action is **claimed** in that view: the default of the other actions of the same view no longer reads it, so a
swap needs no second line (Dodge on north no longer also attacks with X, which north was).
`act_validate` (at startup, in the log) warns about: two actions on one input in one view (the other view is not a conflict), a key
the recomp keeps for itself (Esc, F1, Insert, F7-F12), an input that is also an unmanaged Switch button (it still reaches the game,
for example `ChordXA = pad:lshoulder` also presses L), and notes that Dash / AttackA / Dodge share their game input with
another action (Weapon R-action, interact, weapon B-actions): they are rebound together until the research's split (not done).

## Control commands (control file, `MHGU_CONTROL`)

`act <Name> [frames]` holds the action for that many presented frames (default 8; needs Enable=1), `ptr <x> <y> [0|1]` / `ptr off`
the pointer in the 1280x720 UI space (Pointer=1), `actstat` logs the layer's state (context, weapon class, current action
`act_st:act_no`, dash role, active actions), `wheel <n>` is a wheel (one press per notch for an action bound to the wheel).

## Tests and results (2026-11-09, this machine; vanilla, hidden window, a copy of the save without mod files)

* **Host** (`make test-actions`): 1,835,074 checks, 0 failures. Registry, ini parse / format round trip / unknown names / back-compat
  (no section, Enable=0, other sections ignored), validation, chords and alternatives, per-weapon variants, suppression, injection
  and the **golden property**: for all 65,536 Tier 0 button words x both dash roles x 7 noise words (stick bits, bit 24...),
  `act_view` returns the engine word unchanged in both views. `make test-settings` still passes.
* **T0 golden, in the game** (`Enable=1 ManageMenus=1`, defaults, `Log=1`, scripted presses of A B X Y L R ZL ZR D-pad Plus Minus
  L3 R3 and the stick, in quest 101 with the Hammer and in the village including the Plus menu): the word given to the translators
  equals the one that comes out in every frame, and mb1/mb2 `cur` equals the engine's own translation of it (so the hook is pure,
  P6): quest 2837 frames (381 with input), village and menus 2829 frames (381 with input), 0 mismatches.
* **T1 swap** (`Dodge=pad:north`, `ItemUse=pad:east`, `Dash=pad:west`, `AttackX=pad:south`, `AttackA=pad:rshoulder`, `ChordXA=pad:lshoulder`,
  an SDL virtual controller driven by the control file, so real physical buttons): north (Tier 0's X) gives the game B (log: in
  0x1000, out 0x4000, cur 0x80) and the hunter rolls (`is 0001:023`, then `0001:024`); east gives Y (the item bar, nothing in the
  moves log: no item selected in this save); south gives X: from sheathed it draws (`0001:001`); rshoulder gives A: `0010:000`
  (Hammer) / `0010:008` (Bow) / `0010:004` (HBG); lshoulder gives X+A: `0010:001` (Hammer), `0010:026` (HBG).
  `act Dodge 8`, `act Dash 120` inject the same words (8 and 119 frames in the log).
* **T2 dash** (west -> Dash, stick up held): sheathed Hammer `is 0002:002` ... `0002:003` at the exact frames of the hold; drawn
  `0010:007, 017, 008, 012`. With the option byte `+0x446E` set to 1 in memory (peek / poke, restored; nothing on disk) the layer
  delivers ZR (0x800, mb1 cur 0x800) instead of R (0x200) and the hunter does the same move sequence.
* **T3 attacks**: Hammer, HBG (class 4) and Bow (class 9, weapon swapped in the town of the save copy with the research tree's
  `itweapon`, quest 404, `MHGU_DUMMY=1`): Dodge, X (draw), A and X+A start game actions as listed; the class was identified by the
  mapper hooks (`weapon 4 (HBG)`, `weapon 9 (Bow)`). Only that the game's action changed is verified, not which move it is.
* **T4 menus** (`ManageMenus=1`, Kbm `MenuUp/Down = mouse:wheelup/down`, `MenuConfirm = mouse:left`, `MenuCancel = mouse:right`):
  Plus menu in the village: `wheel -3` = three separate Down presses, the cursor goes Item Pouch -> Quest Info; Confirm on Item Pouch
  opens the pouch, `wheel -2` moves two rows, Cancel closes it and then the menu. Screenshots were compared by eye. `MenuTabR`
  (R) did not change the pouch page (the pouch's pages are not on R); the bit is delivered (log).
* **Enable=0**: a run without `[Actions]` logs nothing from the layer, `act` answers "[Actions] Enable is 0", and the controller's
  south button (Switch B) rolls as before.

## P1 (does any widget react to the pointer?): no, in every screen tried. Hover is not implemented.

With `Pointer=1` the feed works (P2 confirmed: `sPad+0x68DC/E0` = 640,360 -> the mb2 pointer block `+0x6C18` X/Y/prev/older follow,
and with `ptr x y 1` the pressed flag (`+0x6C30`) is set, `changed` too). The held-word bit 24 is stripped by `f_5de064`, no pad
bit reaches the game. But: with the pointer over the rows of the Plus menu (rows 4 and 7), pressed on a row, at (0,0), (1279,719),
(640,360), the selection never moved, nor did a press confirm anything; the same in the Item Pouch. With the research tree's
`itfn` on all 20 reader functions of the block (`772358 80314c 803224 805090 80bbb0 80bc60 81092c 8109d0 8517a0 8517d8 8529ac 8529ec
8560c4 8560fc 858f00 858fe4 8590bc 8b5e98 8b6d0c 8b9604`) **none was ever entered** in: Plus menu (and its second page), Item Pouch,
Combine, Combo List, Quest Info (disabled in the village), Palicoes, Hunter's Notes, Friends, Chat, and the village hot keys X (world
map) and Y (hunter search) (those two opened or not was not checked by screenshot). Only `f_5de9cc` runs, every frame whatever
the pointer does. So the vanilla list controllers and these screens do not use the pointer block: `Pointer` stays an opt-in
experiment (default 0), there is no real-mouse feed, no cursor and no hover. Screens not probed: title, quest board, counter,
smithy, equipment box, item box, shop, map, options: hover there is unknown (the readers could be live in one of them).

## Differences from the spec (and why)

* The per-frame latch is in the **first `f_5de064` call of the frame** (mb1), not `f_5ddf1c`: the engine polls the pad inside
  `f_5ddf1c` before `f_5de064`, so latching there sees the word that was polled.
* The set the game latches is the one copied when it **read the pad** (`GetNpadState`), published together with the buttons: so the
  action word and the engine word cannot come from different host updates (that was the T0 risk).
* The sPad address is `LD32(LD32(0x1a3cbfc))` (the GOT entry points at the global that holds it), the option object `LD32(0x1a97f78)`.
* Defaults are `sw:` tokens (Tier 0 sources evaluated at sampling time) instead of a generated explicit profile: the same result
  follows `FaceButtons`, `[Keybinds]` and scripted input live, which a generated file would not.
* No `ctx` gating: the mb1 view applies in every context (the context is detected and logged: `FIELD` while `f_4a79cc` ran in the
  last frames, `TOWN` for `f_48d1f0`, else `MENU`), mb2 whenever `ManageMenus=1`. Gates (drawn / sheathed), the virtual bit 0x8000 with
  the `f_85f93c` getter override (`SplitDashWeaponR`, parsed and ignored with a log line), intent edits and the mapper wrapper
  (L3-L5) are **not** done: Dash/AttackA/Dodge are still the same input as the weapon R-action / interact / weapon B-action.
* The player's drawn/sheathed state is only logged (`act_st:act_no` at `P+0x2506/0x250A`, as `movelog.c` names them).

## Not verified / known limits

* Not run: title menu, item box, smithy, counter, quest board, equipment box, shop with the menu view (only the Plus menu and the
  pouch); multiplayer; the Prowler; the Lance / Switch Axe / Insect Glaive / Long Sword / Gunlance etc. (3 classes tried); a real
  gamepad and real keyboard / mouse (only the SDL virtual controller, the control file and `ptr`/`wheel`); `FaceButtons = Label` mode
  with a real Nintendo / PlayStation pad (the Tier 0 sources follow the same code but it was not run); more than one controller.
* The mouse: the first click in the window still **captures** the mouse and is not a button press (existing behaviour); clicks then
  press `mouse:*` bindings, the wheel only while captured. Menus with a free pointer are not done (no hover anyway).
* The wheel does not move menus while the item bar handler ran in the last 200 ms (wheel ownership as in the spec, not tested live).
* `MHGU_INPUT` / control-file button presses count as the `sw:` sources of the controller half only.
* An unmanaged button bound as an input (`pad:lshoulder` for `ChordXA`) still reaches the game as its own Switch button (warned at
  startup); keyboard keys that are also `[Keybinds]` of an unmanaged button are not warned about.
* Hooked functions that the interpreter runs (cheats, `interpwatch`) bypass hooks. A mod that hooks one of the same functions before
  this layer starts stops the layer (log line); mods that hook later chain behind it (Forge keeps a hook already there).
* Performance was not measured: the added cost is three extra hooked entries and ~20 ns of table work per frame, plus the
  15 mapper hooks and the player/town update hooks (one `f_47bcd0` guest call each per frame).
* The settings UI (docs/SETTINGS_UI.md) was not started; this is ini-only. Keyboard glyphs in prompts, M3 (mouse aim audit) and the
  "stick-equivalent" actions were not started either.

## How to try

1. Build as usual (`make`), then add to `mhgurecomp.ini` (any of the examples above; menus + mouse wheel preset):
   ```ini
   [Actions]
   Enable=1
   ManageMenus=1
   [Actions.Kbm]
   MenuConfirm = mouse:left | key:return
   MenuCancel  = mouse:right
   MenuUp      = mouse:wheelup | key:up
   MenuDown    = mouse:wheeldown | key:down
   ```
   (click once in the window so that the mouse is captured; Esc releases it.) The log (`log` / stderr) shows `[actions] on ...`,
   the warnings, and `[actions] translators hooked`.
2. A swap on the controller: `[Actions.Pad]` `Dodge = pad:north`, `AttackX = pad:south` (and so on; no two actions on one button).
3. `Log=1` then a script: `MHGU_CONTROL=<file>` with `act Dodge 8`, `actstat`, `wheel -3`; or `python`-free checks in the log:
   per frame `f<n> <ctx> w<weapon> mb1 in <engine word> out <word given to the game> cur <mb1 translation> | mb2 ...`.
4. Host tests: `make test-actions test-settings`.
