# Host settings overlay

Minus + Plus (Back/Select + Start) or F1 opens a single panel inside the game.
Insert still opens Forge. Options, Save and Reset share one scrolling flow, in that order.
Back appears only as a guide beneath the options. Discard is available only
in the unsaved-changes confirmation when leaving the menu. App also contains Restart and Exit. The panel is attached on
every opening; detach allows dragging, and Move menu works with the controller.

Shoulder glyphs bracket the tab labels at the top. The tab header stays pinned
inside the panel, remains clickable while scrolled and never overlaps option
selection. Only the options/actions between the header and hints scroll. The bottom guide labels
the D-pad as Navigation, the confirm button as Confirm, and the back button
as Back, using the selected controller layout and language.
The guide wraps when needed and stays pinned inside the bottom of the panel.
Long option lists scroll above it, including Save and Reset; focused rows scroll
fully clear of the guide.

## Navigation and editing

L/R cycles eight tabs once per press and selects their first field. The tabs are
Display, Graphics & Audio, Controls, Network, Miscellaneous, Mods, Cheats and App. Miscellaneous
contains the monster-HP multipliers and quest-specific overrides. D-pad or
left stick moves between rows; left/right adjusts the selected value. Confirm
opens a choice list or the host-address text editor. Numeric rows adjust
directly with left/right; clicking or confirming them never opens a keypad. Each compound setting has
one focus stop per editable value. Mouse and physical keyboard remain supported.

Back, Escape and F1/chord share a single-level route: cancel an editor, cancel a
confirmation, or leave the main panel. The panel owns its navigation keys before
ImGui NewFrame so native cancel/windowing cannot consume the same input again.
Returning from a subview restores its source field and scroll position.
Shoulders change sections only in the main view. In the on-screen keyboard they
move the text cursor. Character keys, Delete, Clear, Space, Case, Accept and
Cancel can all be selected with the controller. Physical text input, UTF-8
backspace, Ctrl+A, Ctrl+V and Enter are supported. Invalid text stays in the
editor with an inline explanation. Quest ID adjustment skips existing IDs;
new quest IDs are unique and UI row IDs survive deletion and ID changes.

Save and Reset have no shortcut glyphs: activate their selected row.
There are no face-button save/discard shortcuts. The Back guide reflects the actual
SDL face mapping and the Xbox/Switch/PlayStation visual preference. Confirmation
views deliberately have no face-button shortcut guides: Confirm activates the
selected row, and Back cancels. Cancel is selected by default. Layout selection
only changes the host menu's symbols; contextual game confirmation is separate.

## Changes and persistence

Closing the menu with changes offers Cancel, Save and close, or Discard and close.
Exit offers Save and exit or Exit without saving when options or uncommitted text
are pending. ConfirmExit=Off skips the ordinary close confirmation only when
there are no pending changes. Restart saves settings after explicit confirmation.
No settings operation saves game progress. Reset this section changes only the
draft and requires a subsequent Save. A write failure preserves draft and file.

Save applies volume, FPS, FOV, FXAA, MSAA, VSync, mouse sensitivity, face mapping,
background input and confirmation policy without restarting. Render changes
are consumed at render boundaries. Language, symbols and interface scale preview
immediately and persist on Save. Cold fields carry an asterisk; the saved status
indicates a pending restart only when cold settings differ from startup values.
Raw Auto/Desktop values and unchanged environment overrides are preserved.
Comments, unknown INI keys and existing aliases survive atomic saves, and a line whose value did not change stays
as the player wrote it ("21:9", "Borderless Window"). Every setting is one row of the table in `runtime/config.c`
(`g_settings`: section, key and other spellings, type, range or choices, restart needed, error message): reading,
writing and checking the file, and the asterisks and number ranges here, all come from it. A new setting is a row
there, a line in the default file text (`DEFAULT_INI`) and a row in this menu.

App offers seven languages (English, Français, Español, Deutsch, Italiano, Traditional_Chinese, Simplified_Chinese) and interface scale 100–200% (`MenuScale` under
`[Application]`). The root scales with font/DPI, is bounded by the viewport and
keeps its top position when conditional fields appear. Tabs wrap to two rows
when needed. Labels wrap; long values truncate without horizontal scrolling.
Focus has both a filled background and a bright outline; the active tab has an
underline. The full panel scrolls, including actions; no nested scroll region is
required.

## Cheats tab

The Cheats tab lists every cheat loaded from the `cheats` folder, showing whether
each cheat is On or Off, with master codes marked (`{Name}`).

Cheats apply immediately when toggled without requiring Save or a restart:
- Switching a cheat on runs its operations immediately. A cheat that writes into
  executable game code routes that function into the interpreter (`dispatch_interpret`).
- Each cheat maintains an open-addressing journal of byte writes (`jt`). Switching a
  cheat off restores any bytes that still match the cheat's written values while
  leaving memory reused by the game alone.
- "Turn all cheats off" disables all cheats at once.
- Toggle choices are saved and remembered across sessions in `cheats/cheats_state.ini`.
  Files prefixed with `_` load in the disabled state by default. Resetting the section
  re-enables non-`_` cheats and disables `_` cheats.

## Validation

`make test-settings-locale` checks host UI translation coverage across all supported languages (English, Français, Español, Deutsch, Italiano, Traditional_Chinese, Simplified_Chinese), duplicate catalog entries, and nonempty translations.

`make test-settings` covers INI round trips, MenuScale bounds, Auto/Desktop,
environment values, comments, aliases, quest removal, invalid input, failed
writes and live settings.

`make test-settings-panel` drives real ImGui without game files or a graphics
device: eight tabs and shoulder repeat, first-field focus, editor cancellation,
origin restoration, dirty close guards, inactive former save/discard shortcuts, no ImGui
windowing on held face buttons, failed writes, numeric activation without a keypad, pending text during
close, explicit exit/restart, quest addition/removal/reset, 200% Spanish layout,
scrolling and a 640×480 viewport. Runtime smoke checks use an isolated save and
config. Physical controller feel and platform-specific DPI still need human
acceptance before an upstream PR.

## Confirmation experiment (host settings only)

Controls > Confirm button (recomp) selects Original, Bottom button or Right button,
independently of FaceButtons and ButtonIcons. Save applies it immediately and
persists `MenuConfirm = Original|South|East` under [Controls]. The original policy
is the default. The new button is used for navigation, all settings confirmations
and Back; guides follow the saved policy. Held buttons are suppressed during a
policy change and must release before the new mapping accepts them. Returning
from an overlay also waits for held face buttons to release before gameplay.
Forge retains its existing mapping, as do the original game's menus and combat.

This is the testable host portion of contextual confirmation. In-game sPad fields
+0x6B8C/+0x6B90 were confirmed at runtime as 0x2000/0x4000, but exchanging them did
not establish a working title-screen B-confirm path. No guest-memory probe or
unverified in-game remap is shipped. Original menu consumers and their prompts
still require separate identification and validation before this option can
cover the game itself.

## FSR settings

 Graphics & Audio exposes all seven
[Upscaler] fields: FSR, provider (FSR 3.1 / FSR 4), quality, sharpness, object
motion, reactive mask and animated surfaces. They persist without removing INI
comments/aliases, participate in draft equality, dirty guards and section reset,
and are marked restart-only. FSR 4 retains upstream's provider/fallback rules;
selecting it does not install a driver or loader.

Active FSR keeps live MSAA at 1 and FXAA off, including after saving other
settings or staging FSR Off for the next boot. The editor shows the AA rows as disabled "Off (FSR)" while FSR owns them,
and keeps the raw AA values for when FSR is disabled. Existing settings are retained; newly generated defaults
and section reset follow upstream's small-monster-sync Off default.

Regression tests cover all FSR quality presets, every new field's persistence,
invalid enum/float/toggle input, saving with active FSR, staged disable followed
by restart, and the maximum 256 quest overrides alongside the expanded schema.
The panel test exercises provider/quality selection and saving with a gamepad.
