# Game-action rebinding: implementation notes (M1, the non-hover part of M2, and the Game actions screen)

Branch `input-impl` (on upstream 0.15.5+2). Written against `docs/input-rework/ACTIONS.md` of the research branch (section 4,
"prototype spec"). Everything here is **off** unless `[Actions] Enable=1` is in the ini (or it is turned on in the settings
menu, Controls > Game actions): with it off nothing is hooked, no guest code is touched and the only changes on the input
path are two mutex-guarded copies in `hle/input.c`.

## What it is

The engine builds two views of the pad from one word (the pad's held word, in the engine's own bit layout): `f_5de4b0` (mb1, the
gameplay view: player, town avatar, camera) and `f_5de4f8` (mb2, the ~320 menu readers). `f_5de064` calls them through a vtable
slot and derives cur/prev/trig/rel from the result. Both are pure functions of `r1`. The action layer hooks them (`fnhook_set`, as
`itembar.c` / `mousecam.c` do): `r1 = act_view(view, r1, active actions)`, original, done. Chords, combos and key repeat then come
out of the engine's own edge logic. The Control Type options are never written: the dash action reads `LD8(options+0x446E)` and
delivers R or ZR (the game's own dash role) accordingly.

| file | what |
|---|---|
| `runtime/actions.h`, `actions.c` | registry (50 actions), input names, `[Actions*]` parse / format / save / validate, sampling of the bindings into the active set, `act_view`. Pure C (needs only `config.c`'s key names), host-testable |
| `runtime/hle/input_view.c`, `.h` | the game side: hooks `f_5de064`, `f_5de4b0`, `f_5de4f8`, the player update `f_4a79cc`, the town update `f_48d1f0` and the 15 weapon mappers; context (hunt with the weapon out / away, town), weapon class, current action, the optional pointer feed. `input_view_apply` hooks the first time the layer is on |
| `runtime/settings_panel.cpp` | Controls > Game actions: the bindings as a draft, press-to-bind for the controller and for keyboard / mouse / wheel, saved with the other settings (`act_save`) and in use at once |
| `runtime/hle/input.c`, `.h` | the mailbox: the host stages the action set, `input_set` publishes it with the buttons, `GetNpadState` copies it into the set the game latches (so the buttons the engine saw and the actions come from the same host update) |
| `runtime/host_input.c` | sampler glue: physical state callbacks, the Tier 0 sources of each Switch button, wheel queue, the controller press for the settings menu's capture, control commands `act`, `ptr`, `actstat` |
| `runtime/config.c`, `config_store.c` | skip the `[Actions*]` sections (they are read and written by `actions.c`): no "unknown setting", and saving the settings keeps their lines as they are |
| `tests/test_actions.c`, `make test-actions` | host-only tests (no game files) |

## Actions

Every one of the pad's 16 buttons is an action in each of three contexts (50 actions; `act_order` lists them per context):

* **Hunt** (the gameplay view while a hunter plays): `AttackX` (X), `AttackA` (A, weapon out), `ChordXA` (X and A in one
  frame, no default), `Dodge` (B), `ItemUse` (Y), `Interact` (A, weapon away), `Dash` (the R-role: R, or ZR with the second
  Control Type), `FieldZR` (the other one of R / ZR), `FieldL`, `FieldZL`, `CrossUp/Down/Left/Right` (D-pad), `FieldL3`,
  `FieldR3`, `Pause` (Plus), `Map` (Minus).
* **Town / Hub** (the gameplay view while the town avatar plays): `TownInteract` (A), `TownCancel` (B), `TownX`, `TownY`,
  `TownRun` (the R-role), `TownZR`, `TownL`, `TownZL`, `TownUp/Down/Left/Right`, `TownL3`, `TownR3`, `TownPlus`, `TownMinus`.
* **Menus** (the menu view, needs `ManageMenus=1`): `MenuConfirm` (A; also `Confirm`), `MenuCancel` (B; also `Cancel`),
  `MenuX`, `MenuY`, `MenuUp/Down/Left/Right`, `MenuTabL/R` (L/R), `MenuPageL/R` (ZL/ZR), `MenuPlus`, `MenuMinus`, `MenuL3`,
  `MenuR3`.

**The principle: rebinding an action never changes a different feature** unless the user merged them, and only the Simple
layout merges (its rows write one binding to several actions explicitly: Confirm = `MenuConfirm` + `TownInteract`, and so on).
So a profile edited in Advanced never moves anything it did not name: the town's actions, the menus', the hunt's, the item
bar's are separate bindings with separate defaults (the game's own button, Tier 0). An earlier version let the town's actions
follow the hunt's and the menus' (`TownCancel` followed `MenuCancel`, then `Dodge`; `TownInteract` followed `MenuConfirm`;
`MenuPlus` followed `Pause`): rebinding Dodge moved the town's B and `Confirm = pad:south` alone moved the town's talk. Those
chains are gone (2026-11-11). What is left follows only **inside one context and one feature**, where the two are the same
game input until the user splits them: `Interact` follows `AttackA` (the hunt's A button), `Crouch` follows `Dodge` (its B
button), `WeaponR` follows `Dash` (the R-role getter). An action with no binding of its own uses the binding of the one it
follows that has one (per device), else its default (Tier 0). An ini that relied on `Confirm = pad:south` also moving the
town's talk needs `TownInteract = pad:south` (the Simple screen writes both).

**The hunt's A button** is one of two actions by the hunter's state, read from `player+0x114C` (u8: 1 weapon out, 0 away;
found by comparing memory snapshots of sheathed / drawn idle, walk, roll and attacks, see Tests): weapon out, A is `AttackA`;
weapon away, A is `Interact` (gather, carve, talk, open). While `Interact` follows `AttackA` they are one button, as in the
game. Once split, `AttackA` with the weapon away presses A only together with `AttackX` or `Dash` (the draw attacks that
need both), or always with `[Actions] SheathedAttackA=1`. The Prowler (no weapon to put away) and a screen that reads the
gameplay view with no hunter get both.

**One input for Interact and other hunt actions** (the quest box with Interact, Dodge and the like on one button). With the
weapon away an input bound to `Interact` and to another hunt action (`Dodge`, `AttackX`, ...) presses A and B (or X) together,
and the game answers the other one: A + B is a roll, so the box never opened. The game does not say beforehand whether
something can be interacted with (the research's flags `P+0x2BF4` / `P+0x1060` do not change near the box: snapshots at the
box and 30 m away, and its Supply Box prompt), so the press is **tried as Interact first** (`act_gate`, game side
`tr_view`; only when the hunter stands still: if the left stick is pushed as the press begins, the roll wins at once and Interact is not tried, so only the standing crouch waits): only A for `[Actions] SharedInteractFrames` updates (default 1, 1..30); if the hunter's action changes in that
time (a common action, not a weapon move: the box opens, `act 1:0x70` came 3-4 updates after the press) it was an
interaction and the other actions stay off for as long as the input is held; if not, they get the press (a roll 1 update
late), and a press that ends sooner still gets a 3-update press of them (a tap is a roll, late). Sharing is decided from the
bindings (`act_interact_share`: the same input, or a chord that holds it, either device, per weapon class): inputs that are not
shared, and the whole of a hunt with the weapon out, are not delayed. Cost: that shared roll starts ~1 update later than a
roll on its own button; the setting trades that against a slow interaction (an NPC that takes longer than that to turn
would be rolled over). `AttackA` is not a contender (with the weapon away it is not delivered).
**Dodge no longer uses this**: with Dodge and Interact on one input the state decides (see "State-scoped actions" above: Dodge
only while the stick is pushed, weapon away), so nothing waits; the gate is kept for Interact sharing with actions that have
no state to tell them apart (`AttackX` ...). At a chest 1 update of waiting was not enough: the hunter crouched.

**Dash and the weapon R-action** are one game input, the R-role getter `f_85f93c` (104 callers). `WeaponR` is the action for
the weapon's side of it; it follows `Dash` until it has a binding of its own. The split is on with
`[Actions] SplitDashWeaponR=1` or as soon as `WeaponR` has a binding (any device or class); off, nothing is added. On: the
translator hook ORs the **virtual bit 0x8000** into the gameplay word while `WeaponR` is active (no pad button gives it;
`Dash` still delivers the real R / ZR), and a hook on `f_85f93c` answers 0x8000 to the weapon's callers and the real role
bit to every other. The callers are told apart by return address (found with the PadRemap mod's getter log and confirmed
here with `MHGU_ACTION_KCLOG=1`, which logs each one once): the 15 classes' code `0x1360000..0x13f0000` and the sites
`0x47e7b0 0x4a7bb0 0x47d480 0x47d508 0x47d530 0x47d564 0x47cc58 0x47d2fc 0x47d3f8` of the input update; the dash tests
`0x47caa8 0x47cad0 0x47cb48 0x47cc24 0x47cd3c 0x47e7fc` and the two at `0x48d358 / 0x48d3f0` keep the real bit. A mod
that hooks `f_85f93c` before this layer starts keeps Dash and WeaponR one input (log line).

Weapon classes for per-class overrides: GreatSword SnS Hammer Lance HBG LBG LongSword SwitchAxe Gunlance Bow DualBlades Horn
InsectGlaive ChargeBlade Prowler.

## State-scoped actions (the general mechanism)

The game has few buttons and many consumers: one A press is an attack, an interaction, a menu confirm, a talk, the item bar's
next item. The layer takes them apart by **the state the hunter is in**, not by timing: every action has a *scope* (`scopes[]`
in `actions.c`: flags it requires, flags that forbid it, and the bits it applies to), the translator hook builds the hunter's
state for the frame (`state_of`: weapon out / away, item bar open, left stick pushed or not), and a bit is delivered only by
the actions whose scope matches. Actions with no scope deliver always, so with nothing bound every state gives the word it was
given (the golden test runs over the hunt in each state, the item bar open or not, the town and the menus).

| state flag | where it comes from |
|---|---|
| weapon out / away | `player+0x114C` (u8), as above; the Prowler and "no hunter" count as both |
| item bar open | vfunc `+0x25c(player, 1)` of the hunter (hle/itembar.c: it is true while L is held and the bar may be used), called from the player update hook and latched for the translator: one frame of latency |
| left stick pushed | the engine word's left-stick direction bits `0xf0000` (past half way) at the translator |

| scope | action | what it means |
|---|---|---|
| weapon away only | `Interact` (A) | gather, carve, talk, open; the weapon-out A is `AttackA` |
| not while the bar is open | `AttackA`, `Interact`, `ChordXA` (its A), `ItemUse` (Y) | the bar owns A and Y then; B is not the bar's: `Dodge` and `Crouch` are untouched |
| only while the bar is open | `ItemBarRight` (A), `ItemBarLeft` (Y) | L held: A scrolls right, Y left |
| weapon away, Crouch split | `Dodge` only while the stick is pushed, `Crouch` only while it is not | the roll and the crouch are one game B |

**The item bar.** With L held the hunter's item bar scrolls on A (next item) and Y (previous item): the same bits as attack /
interact / chord (A) and use item (Y), so rebinding any of those used to change how the bar scrolls (`itembar.c` names the
game's actions: `0xa` right, `0x9` left; `0xa` is the attack A's set-C event and `0x9` use item's, so the left is Y, not B:
with the Hammer in quest 101, L + A went `0x28 -> 1 -> 2` and L + B did nothing). Now, while the bar is open, A and Y in
the gameplay word come only from `ItemBarRight` / `ItemBarLeft` (defaults: Tier 0's A and Y, so nothing changes until they
are bound) and `AttackA`, `Interact`, `ChordXA`'s A and `ItemUse` are silent for those two bits; when the bar is closed the
bar's two deliver nothing. B is not the bar's, so `Dodge`, `Crouch` and the town's B are not touched by it. The bar's
inputs are claimed among themselves only (a physical input given to the bar's A is not taken away from the defaults of the
hunt's other actions, and the other way round). In Simple they are two rows after Dodge / Crouch.

**Crouch.** B with the hunter standing still is the crouch, B with the left stick pushed is the roll; one game button. `Crouch`
(default: Dodge's input, so nothing changes) splits them once it has a binding of its own on any device or class, **or**
when `Interact` and `Dodge` are on one input: with the weapon away `Crouch` then delivers B only while the stick is not
pushed and `Dodge` only while it is. Interact and Dodge on one input is therefore a pure interaction standing still (only A:
nothing crouches, nothing waits) and a roll while moving (A and B: the roll wins); a separate Crouch input crouches
standing and does nothing moving. `Crouch` may not share an input with `Interact` (Simple: binding one takes the input off
the other; Advanced: red warning; in play Interact wins standing still and `act_validate` says so). With the weapon out both
deliver B as before, because weapon B-actions (Lance, Switch Axe, Insect Glaive, bowguns, Prowler) are read standing and the
roll / crouch confusion only exists with the weapon away. A stick pushed less than half way counts as standing: a crouch
input still gives the game its B, and the game rolls if it walks.

This replaced the "Interact first" delay for Dodge: it was tried first for `SharedInteractFrames` updates and then the roll
went through, and at a chest 1 update was not enough (the hunter crouched). The gate (`act_gate`, `SharedInteractFrames`) is
kept only for Interact sharing an input with other hunt actions that have no state to tell them apart (`AttackX` ...).

## Consumer map (hunt and town; what is separable now)

Bits are the engine / hunter-copy ones (INPUTS-MASTER B.1). "Separable now": the layer has an action or state for it. "Gate
known": a state discriminator exists but is not wired. "Not yet": the game reads the shared bit in one place and decides by
internal state we cannot see from the translator. Sources: research docs `INPUTS-MASTER.md`, `ACTIONS.md`, `actions.csv`.

| consumer | bit | what tells it apart | status | what it would take |
|---|---|---|---|---|
| attack A (weapon out) | A | weapon out (`+0x114C`) | **separable now** (`AttackA`) | - |
| interact: gather, carve, talk, open | A | weapon away | **separable now** (`Interact`); chest / NPC / gather never reached in a hunt: only "A reaches the game in the right state" is verified | a hunt at a chest and an NPC to confirm |
| item bar next / previous | A / Y with L held | bar open (vfunc `+0x25c(player, 1) != 0`) | **separable now** (`ItemBarRight/Left`) | - |
| draw weapon | X (sheathed) | weapon away | **separable now** by state: `AttackX` is one action both ways; a scope `AttackX` weapon-away only would give a separate "Draw" | a `Draw` action with scope `req AWAY`, 20 lines |
| attack X | X | weapon out | **separable now** (`AttackX`; draw is the same input) | as above |
| sheathe | Y after a pause / sheathe move | weapon out and idle | gate known (state `act_st 1:0`, drawn) | not decoded which bit asks for motion 0x7 (U-5); probe with the moves log |
| use item | Y | weapon out/away | **separable now** (`ItemUse`) | - |
| roll | B + stick | stick pushed, weapon away | **separable now** (`Dodge`, split mode) | - |
| crouch | B standing still | stick not pushed, weapon away | **separable now** (`Crouch`) | - |
| weapon B-actions (Lance guard, SA, IG, HBG / LBG cancel, Prowler commands) | B | weapon class + move | gate known (class `g_var`, move `act_st:act_no`) | a `WeaponB` action: virtual bit like `WeaponR` + the 15 mappers' B tests (`trig16 0x80`) |
| dash / sprint | R-role | by caller of `f_85f93c` | **separable now** (`Dash`, `WeaponR` split) | - |
| weapon R-action: guard, charge, aim | R-role | caller of `f_85f93c` | **separable now** (`WeaponR`); ~80 other callers (prompts / flags) still read the real R | classify the rest of the callers (P16) |
| X+A chord (tackle / special), R+X, R+X+A | X+A / R+X(+A) | the engine's own two-bit test | `ChordXA` now; R+X, R+X+A not | two more chord actions delivering the bits in one frame |
| quick action / ammo / coating select | D-pad (RS by Control Type) | weapon class (HBG, LBG, Bow, Prowler read it in the mapper) | **separable now** as `CrossUp..Right` for all; not per use | per-use needs the mapper events (U-5) |
| camera reset / lock-on, town L action | L-role | none seen | **separable now** (`FieldL`); lock-on vs reset not separable | event names (U-5) |
| target camera ZL / ZR roles | ZL / ZR | none | **separable now** (`FieldZL/ZR`) | - |
| left stick click action (flag G) | L3 | Control Type sub-option | **separable now** (`FieldL3`) | - |
| right stick click | R3 | Control Type | **separable now** (`FieldR3`) | - |
| pause menu / map | Plus / Minus (mb1 and mb2) | hunt vs menu | **separable now** (`Pause`/`Map` vs `MenuPlus/Minus`) | - |
| mash / struggle | any of A B X Y L R | none (reads the six bits, mask `0x00FF05F0`) | follows the bits: not separable | a state gate on "struggling" (not found) |
| town: talk, X / Y / B / L actions, run | A X Y B L R | town context | **separable now** (`Town*`, independent) | - |
| town hot buttons (Hunter Search Y, World Map X) | mb2 Y / X | menu view | **separable now** (`MenuY`, `MenuX`) | - |
| village Multiplayer / ZR hot buttons | mb1 ZL / ZR on a menu-like screen | gameplay view with no hunter | `FieldZL/ZR` ("no hunter" = hunt actions) | a menu-like-screen state (`f_6eeb78` pad owner gate) |
| craft viewport zoom / rotate | mb1 ZL / ZR | gameplay view in a menu | same | same |
| menus: confirm, cancel, X, Y, tabs, pages, cursor, Plus, Minus, L3 / R3 | mb2 | menu view | **separable now** (`Menu*`) | per-screen: not yet (a button means the same in every menu) |
| raw-word readers (title ZL+ZR, options L+R, R3) | pad word | bypass both views | not separable | hook the raw readers (4 functions) |
| gestures | via the Plus menu list | - | menu action, not a field one | - |
| event / cutscene skip | A (host `evskip`) | host | follows the host's A, not the action layer | read the actions instead of the Switch A |

**Not yet separable, ranked by what the user likely wants next** (cost and benefit): (1) weapon B-actions apart from the roll,
for the five classes that use B, high value for Lance / Switch Axe / bowgun players, medium cost (a `WeaponB` virtual bit like
`WeaponR`, the 15 mappers read `trig16 0x80` directly); (2) a separate Draw / Sheathe (state is known: weapon out / away,
Y is the sheathe), low cost for Draw, the sheathe trigger is undecoded; (3) R+X and R+X+A chords, low cost; (4) the village
ZL / ZR hot buttons and the craft viewport, medium (needs a menu-like-screen state); (5) cutscene skip on the action layer,
low; (6) mash / struggle and the raw-word readers, not worth it.

## ini

```ini
[Actions]
Enable=1                  ; 0 (default, and no section at all): nothing hooked, bit-for-bit today's behaviour
ManageMenus=1             ; the menu view too
SheathedAttackA=0         ; 1: AttackA presses A with the weapon away too (only matters once Interact is bound)
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

Interact = key:f              ; gather / carve / talk on F, Attack A stays where it is

[Actions.Pad.Weapon.Hammer]   ; sparse override for one class, resolution: Weapon.<Class> > base > followed action > default
AttackA = pad:north
```

`sw:A`, `sw:Dash` etc. are also accepted: "what the settings' own controls (Tier 0) give that Switch button".
**Defaults are exactly Tier 0**: an action with no entry reads the Switch button Tier 0 gives it (`sw:` sources of the controller
and of `[Keybinds]`, scripted input included), so `Enable=1` with no bindings plays like `Enable=0` (golden test T0 below). A device
list overrides that device's half only: `[Actions.Pad] Dodge = ...` leaves the keyboard's Dodge on `[Keybinds]`' B. A physical
input bound to an action is **claimed** in that view: the default of the other actions of the same view no longer reads it, so a
swap needs no second line (Dodge on north no longer also attacks with X, which north was).
`act_validate` (at startup, in the log) warns about: two actions on one input in one context (another context is not a
conflict; `AttackA` and `Interact` on one input are not one either), a key the recomp keeps for itself (Esc, F1, Insert,
F7-F12), and notes that Dodge shares its game input with the weapon B-actions (they are rebound together) and, until the
split is on, Dash with the weapon R-action. Every button is an action now, so a controller button given to an
action no longer also reaches the game as its own Switch button in that context.

A scripted press (`MHGU_INPUT`, the control file's `A 8`, quickhunt) is the game's own button whatever is bound: it presses
the actions that deliver that button (so quickhunt still gets through the title with `MenuConfirm` rebound).

## The Game actions screen (settings menu, Controls > Game actions)

The screen shows the bindings of **one control scheme** at a time, named in its title ("Game actions: Controller" /
"Game actions: Keyboard"), and has two layouts. Top rows: Rebind game actions (Enable), Also in the game's menus
(ManageMenus), **Layout** (Simple / Advanced), **Control scheme** (Controller / Keyboard). Both are kept in the ini:
`[Actions] Scheme=Controller|Keyboard`, `Layout=Simple|Advanced`, written only once the player picked them. Without a
`Scheme` the screen opens on the device a button was last pressed on (`host_input_last_device`: the controller if the menu
was opened with Plus + Minus, the keyboard after F1). Without a `Layout` a fresh profile is Simple and one that already has
bindings is Advanced (`act_layout`); the layout shown is fixed when the screen opens, and Save writes it, so binding
something in Simple does not turn it into Advanced next time.

**Advanced** is the screen as it was: Attack A also interacts, Split Dash from weapon R-action, For (All weapons / one
class: then only the hunt's actions), then every action by context (Hunt / Town and Hub / Menus) with what it reads now:
its inputs, `Default`, or the inputs of the action it follows (`same as <Name>`); red with `also: <Name>` when another
action of that context is on the same input (no automatic change).

**Simple** (`ActRow`, `act_row_*` in actions.c: pure, host-tested) lists 30 merged rows in two groups. Hunt and town:
Attack X (+ the town's X), Attack A, X + A, Dodge, Use item (+ the town's Y), **Gather / open / talk, weapon away** (the
hunt's `Interact`, ini name unchanged), **Sprint** (`Dash` + the town's run) and **Weapon action: guard, charge, aim**
(`WeaponR`), always two rows, L / ZL / ZR (+ the town's), the four D-pad directions (+ the town's), L3 and R3 (+ the town's and
the menus'). Menus: **Confirm / talk / examine** (`MenuConfirm` + the town's `TownInteract`), **Cancel / back**
(`MenuCancel` + `TownCancel`), Start menu (+) and Minus (-) (the hunt's, the menus' and the town's), menu X / Y, the four menu
directions, tab left / right and page left / right. Every action is in exactly one row (host test). A row is its head and its
members; setting it writes the head and clears the members so that they keep **following** it (the follow chain), and gives a
copy to a member that does not follow (the menus' L3). A row whose members read something else (edited apart in Advanced)
shows "Custom (differs by context: see Advanced)"; Set overwrites all of them. No per-weapon rows (a note points to
Advanced).

**Taking an input away (Simple only).** After a row is given an input, every other row that is read in a common context
(hunt, town or menus; a row's contexts are its members') and holds that exact input as a single-input alternative loses it;
a row left with no input is **not set** (`None` in the ini; the engine then gets nothing for it). The input is also taken
from a row that reads it **by default** (what Tier 0 gives that Switch button for the scheme: `host_input_sw_sources`, so
Confirm on the bottom button takes Cancel's default). Left alone: rows that follow another (the owner is the one that
changes), and the hunt's `Interact`, which may share an input with other hunt actions by design (the Interact-first gate).
Not set rows are red ("Not set: choose a button"), counted in a red "Actions not set" row at the top, and the choice itself
says which rows lost the button. Save works but warns, in red, when Confirm or Cancel is not set (the menus could not be used
or left with that scheme). Advanced and the engine never see any of this: both layouts write the same per-action keys.

Rows' editing: Set (press the input: any controller button or trigger; a key, mouse button or wheel notch; Esc cancels, the
controller's choice also after 8 s; the button chosen is not also a press in the menu), Add another (up to 3), Not set
(`None` in Advanced), Default. Chords (`a+b`) are ini-only. The bindings are a draft like the other settings: Save writes
the `[Actions*]` sections (every other line of the ini is kept) and they are in use from the next frame; turning Enable on
hooks the game then, with no restart, and turning it off makes the hooks pass the pad through.

**The drawing and the context tabs (phase 2, `runtime/binding_view.*`, `draw_bind_*` in settings_panel.cpp).** A controller
outline (Controller scheme) or a keyboard with the mouse (Keyboard scheme) is drawn from the same bindings as the rows. The
focused row (controller / keyboard focus) or the one just chosen glows its input(s) with a leader arrow to a chip naming the
action; hovering a row rings its input; hovering an input (the key, or its chip) says what is on it; clicking an input while a
row is capturing binds it (the same path as a pressed input: reserved keys such as Esc and the stick's directions are refused
with a message), and clicking an input with a row focused and nothing capturing gives it to that row. The drawing follows the
chosen weapon class (Advanced) and the tab. **Tabs** Hunt / Town / Menus (the Show row, or L1 / R1 on the controller) show
one context at a time, so conflicts are per context; **Keys** shows the keyboard-only inputs (movement, Walk, item bar left /
right on keys and the wheel, chat key). These are not new settings: they edit the existing `[Keybinds]` (`s.draft.binds`) and
the old Keybinds submenu still works on the same values. The Plus + Minus menu chord is a toggle (`menu.chord`) on the
Controller Menus tab.
Layout of the window: **beside** the list when the screen has the room (the list is narrowed to give the drawing about 72
font-heights), **inline** above the rows otherwise; windows at least 50 font-heights tall that are too narrow for beside use
a drawing over the list. Below that (about 800x450) the drawing is inline and scrolls with the rows: still usable, not ideal.
`bv_fit_width` picks the widest drawing that fits the height (chips as side columns or as tiers above and below).
**Held inputs** light up live: pad buttons from the SDL state, keys / mouse buttons / wheel from `ui_keys` / `ui_mouse` in
host_input.c (kept apart from the game's `keys[]` because the overlay swallows the events while the menu is open;
`host_input_held`). In the menu the pad's bottom button is Back, so use the sticks' clicks to test the glow.
Limits: the sticks' four directions are drawn but cannot be bound (BV_STICK_*; stick clicks can); the pad's own wedge areas
of the drawing are hit-tested as polygons, small keys of the keyboard are targets of their own. Hidden-run screenshots:
`build\shots\` (p2*).

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

## Tests of 2026-11-11 (state-scoped actions; hidden window, a copy of the save, virtual controller)

* **Host** (`make test-actions`): 4,588,559 checks, 0 failures. The golden property now also runs over the hunt with the
  weapon away and unknown, and with the item bar open (every Tier 0 word comes out unchanged), plus: Crouch / Dodge /
  Interact in each state and with the stick, Interact winning an input with Crouch, the auto split (Dodge + Interact on one
  input), the item bar's A / Y with Use item moved, the bar's own actions moved, the town and menu actions following
  nothing, the Simple rows writing the town explicitly. `test-settings`, `test-settings-panel`, `test-settings-locale` pass.
* **Item bar** (quest 101, Hammer, sheathed, L held through the virtual pad, `peek` of `player+0x3298`): vanilla ini:
  L + east (A) `0x28 -> 1 -> 2`, L + west (Y) `2 -> 1 -> 0x28`, L + south (B) nothing, east / south alone nothing. With
  `MenuConfirm = MenuCancel` swapped, `TownInteract`, `Dodge` and `Interact` moved to south, the same sequence (`28 1 2 1 28`);
  before the item bar context the same run did not scroll right on L + east (A was dropped: weapon away with `Interact`
  moved). With `ItemBarLeft = pad:east`, `ItemBarRight = pad:west` the bar turns the other way and only the bar: L + east
  `0x28 -> 0x12 -> 0xc` (left wraps to the last item), L + west `0xc -> 0x12 -> 0x28`; the log shows the bar open (`bar 1`)
  exactly while L is held and the gameplay word A / Y coming from the bar's two actions.
* **Quest Box** (quest 101, `questbox` spot): with `Interact` and `Dodge` on south and `Crouch` on west, south held 0.6 s and a
  0.08 s tap open the Supply Box; every frame of the press gives the game only A (never B), from the first. 300 units away
  with the stick pushed the same input gives A + B from the first frame (the roll). Standing, south gives A only (no crouch,
  nothing to interact with); west standing gives B (the crouch), west with the stick pushed gives nothing. With `Crouch` on
  Interact's input too the log says `conflict: Crouch and Interact are both on pad:south` and standing still the input is A
  only. No chest or NPC is reachable in the harness (the quest has the box only): the chest crouch the user saw is covered by
  state, not by a reproduction.

## Tests of 2026-11-10 (this machine; hidden window, a copy of the save, the TeleportTool mod for the town spot)

* **Host** (`make test-actions`): 2,753,209 checks, 0 failures: the golden property now over the hunt (weapon out, away,
  either), the town and the menus; following; the Interact / AttackA states; every action alone delivers exactly its button
  and each context owns all 16; all 50 actions through the ini and back; scripted presses; `act_save` (other sections kept,
  saving twice changes nothing). `make test-settings test-settings-panel test-settings-locale` pass.
* **Weapon out / away flag**: `MHGU_PLSNAP` snapshots of the player (0x3800 bytes, 20 updates each) in 12 phases of a Hammer
  hunt (quest 101): sheathed idle / walk / roll, drawn idle / roll / X attack / A attack / walk, sheathed again. Exactly
  three bytes are constant in every sheathed snapshot and constant and different in every drawn one: `+0x4B5` (0 / 5),
  **`+0x114C` (0 / 1)** and `+0x120E`. `act_st:act_no` (`+0x2506/0x250A`) is not usable: idle is 1:0 and a roll 1:x in both.
* **Golden in the game** (`Enable=1 ManageMenus=1`, no bindings, scripted A B X Y L R ZL ZR D-pad Plus Minus L3 R3, quest
  101): 3170 frames (408 with input), the word given to both translators equals the one that comes out in every frame;
  1293 frames built as weapon out, 609 as weapon away, 53 as town.
* **Town** (`Confirm = pad:south`, `Cancel = pad:east`, SDL virtual controller, at the Soaratorium's item box): east (the
  game's A) does nothing; south opens the Item Box and confirms Store Items; east backs out of both (screenshots).
* **Interact split** (`AttackA = pad:rshoulder`, `Interact = pad:lshoulder`, Hammer, quest 101): weapon away, rshoulder gives
  the game nothing and lshoulder gives A; after drawing, rshoulder gives A and the hunter attacks (`0010:000`), lshoulder
  gives nothing; the old A position gives nothing in either state; sheathed again, lshoulder is A again. A real gather /
  carve / field NPC was not reached (no spot): what is verified is that A is delivered only in the right state.
* **The screen** (started with no `[Actions]` at all): Enable and menus turned on, Dodge bound with the virtual controller
  (Pad North) and with the keyboard (Space), Save: the ini has `[Actions]`, `[Actions.Pad]`, `[Actions.Kbm]`, the log says
  `translators hooked`, and in the town north is `Dodge TownCancel`, south no longer the game's B. Started again, the
  bindings are loaded; with the controller alone (no mouse): Controls > Game actions > Dodge > Set > west: `Pad West`.

* **Simple / Advanced screen** (hidden, virtual controller): a fresh ini opens Simple with the scheme of the device the menu
  was opened from; Confirm set to the bottom button gives `Confirm = Pad South`, "Actions not set: 1" and Cancel red "Not
  set: choose a button"; Save wrote `[Actions] Enable=1 ManageMenus=1 Scheme=Controller Layout=Simple`, `MenuConfirm =
  pad:south`, `MenuCancel = None` and warned about Cancel. With that ini in the town at the Item Box: the game's old A
  (east) does nothing, south opens the box and confirms, east (Cancel not set) does not close it. The same ini opened again:
  Layout and Scheme switch to Advanced and Keyboard and back (titles and rows follow). Host: 2,753,433 checks, 0 failures
  (rows, following, custom rows, claiming defaults and explicit inputs, the Interact exemption, scheme / layout parse and
  format, defaults for old profiles); `test-settings`, `test-settings-panel` and `test-settings-locale` (now also every
  action and row label of `actions.c`) pass.

## Differences from the spec (and why)

* The per-frame latch is in the **first `f_5de064` call of the frame** (mb1), not `f_5ddf1c`: the engine polls the pad inside
  `f_5ddf1c` before `f_5de064`, so latching there sees the word that was polled.
* The set the game latches is the one copied when it **read the pad** (`GetNpadState`), published together with the buttons: so the
  action word and the engine word cannot come from different host updates (that was the T0 risk).
* The sPad address is `LD32(LD32(0x1a3cbfc))` (the GOT entry points at the global that holds it), the option object `LD32(0x1a97f78)`.
* Defaults are `sw:` tokens (Tier 0 sources evaluated at sampling time) instead of a generated explicit profile: the same result
  follows `FaceButtons`, `[Keybinds]` and scripted input live, which a generated file would not.
* Contexts: the gameplay word is built for the hunt (`f_4a79cc` ran in the last frames; weapon out or away by
  `player+0x114C`), else the town (`f_48d1f0`), else the hunt's actions with both A actions; mb2 whenever `ManageMenus=1`.
  The virtual bit 0x8000 with the `f_85f93c` getter override is done (L4, above); intent edits and the mapper wrapper
  (L3-L5) are **not**: Dodge is still the same input as the weapon B-actions.
* The research's drawn discriminator (`P+0x2506 == 0x10`) is not what is used: it is the current action's group, 1 in a
  roll with the weapon out. `player+0x114C` (the KB's flag, which the static pass found no reader of) is constant per state.

## Not verified / known limits

* Not run: title menu, item box, smithy, counter, quest board, equipment box, shop with the menu view (only the Plus menu and the
  pouch); multiplayer; the Prowler; the Lance / Switch Axe / Insect Glaive / Long Sword / Gunlance etc. (3 classes tried); a real
  gamepad and real keyboard / mouse (only the SDL virtual controller, the control file and `ptr`/`wheel`); `FaceButtons = Label` mode
  with a real Nintendo / PlayStation pad (the Tier 0 sources follow the same code but it was not run); more than one controller.
* The mouse: the first click in the window still **captures** the mouse and is not a button press (existing behaviour); clicks then
  press `mouse:*` bindings, the wheel only while captured. Menus with a free pointer are not done (no hover anyway).
* The wheel does not move menus while the item bar handler ran in the last 200 ms (wheel ownership as in the spec, not tested live).
* `MHGU_INPUT` / control-file button presses count as the `sw:` sources of the controller half only.
* With `ManageMenus=0` the menus read Tier 0: a controller button given to a hunt action is still that Switch button there.
* Both views are live at once: in a hunt the menu view still reads its own actions (a button moved in the hunt keeps its
  menu meaning until the menu action is moved too; `MenuPlus` / `MenuMinus` follow `Pause` / `Map` for that reason).
* The sticks are not actions (moving, the camera; the keyboard's are `[Keybinds]`), nor the item bar's keys.
* The weapon-out flag was checked with the Hammer only; the Prowler is treated as "either" without having been run.
* The weapon R-action callers outside the 15 classes' range and the nine sites above (the other 80-odd callers of the
  getter, mostly hint and flag queries) keep the real role bit: a weapon-move test hiding in one of them would still follow
  Dash. The Prowler's classes are in the range but it was not run.
* The shared Interact input: a weapon whose A with the weapon away starts a weapon move (a draw attack) starts it during the
  first updates of the press (A is what is tried first) and is then not rolled over; the gate reads the hunter's action
  fields, so a hunter that changes action for another reason at that moment (stopping from a run) counts as interacting.
  It was run with the Hammer at the Quest Box only; NPC talk, gathering and carving were not reached.
* The Plus + Minus chord that opens the settings menu reads Tier 0's Plus and Minus, not `Pause` / `Map`.
* Hooked functions that the interpreter runs (cheats, `interpwatch`) bypass hooks. A mod that hooks one of the same functions before
  this layer starts stops the layer (log line); mods that hook later chain behind it (Forge keeps a hook already there).
* Performance was not measured: the added cost is three extra hooked entries and ~20 ns of table work per frame, plus the
  15 mapper hooks and the player/town update hooks (one `f_47bcd0` guest call each per frame).
* The game's own button prompts still show the Switch button of an action, not what it is bound to. Keyboard glyphs in
  prompts, M3 (mouse aim audit) and the "stick-equivalent" actions were not started. The screen's capture takes one input:
  chords are ini-only. Its new texts have Spanish translations that no native speaker has read.

## How to try

1. Build as usual (`make`). In the game: F1 (or Plus + Minus) > Controls > Game actions. Turn "Rebind game actions" and
   "Also in the game's menus" on, choose Controller or Keyboard and mouse, Confirm on an action > Set > press the input.
   Back, Save. It applies at once.
2. Or in `mhgurecomp.ini`, for example Confirm on the bottom button and Cancel on the right one (menus and town), a
   separate Interact, and mouse menus:
   ```ini
   [Actions]
   Enable=1
   ManageMenus=1
   [Actions.Pad]
   Confirm  = pad:south
   Cancel   = pad:east
   Interact = pad:south
   [Actions.Kbm]
   MenuConfirm = mouse:left | key:return
   MenuCancel  = mouse:right
   MenuUp      = mouse:wheelup | key:up
   MenuDown    = mouse:wheeldown | key:down
   ```
   (click once in the window so that the mouse is captured; Esc releases it.) The log shows `[actions] on ...`, the
   warnings, and `[actions] translators hooked`.
3. `Log=1` then a script: `MHGU_CONTROL=<file>` with `act Dodge 8`, `actstat`, `wheel -3`; per frame the log has
   `f<n> <ctx> w<weapon> mb1 in <engine word> out <word given to the game> cur <mb1 translation> | mb2 ... | act <set> v<context>`
   (v0 weapon out, v3 weapon away, v2 town, v4 no hunter).
4. Host tests: `make test-actions test-settings test-settings-panel test-settings-locale`.
