# Host settings overlay

Minus + Plus (Back/Select + Start) or F1 opens a single panel inside the game.
Insert still opens Forge. Options, Save and Reset share one scrolling flow, in that order.
Back appears only as a guide beneath the options. Discard is available only
in the unsaved-changes confirmation when leaving the menu. Menu Options also contains Restart and Exit. The panel is attached on
every opening; detach allows dragging, and Move menu works with the controller.

Shoulder glyphs bracket the tab labels at the top. The tab header stays pinned
inside the panel, remains clickable while scrolled and never overlaps option
selection. Only the options/actions between the header and hints scroll. The bottom guide labels
the D-pad as Navigation, the confirm button as Confirm, and the back button
as Back, using the selected controller layout and English/Spanish language.
The guide wraps when needed and stays pinned inside the bottom of the panel.
Long option lists scroll above it, including Save and Reset; focused rows scroll
fully clear of the guide.

## Navigation and editing

L/R cycles seven tabs once per press and selects their first field. The tabs are
Display, Graphics & Audio, Controls, Network, Misc, Mods and Menu Options. Misc
contains the monster-HP multipliers (under a "Monster HP Scaling" title: a header row,
skipped by navigation) and quest-specific overrides. D-pad or
left stick moves between rows; left/right adjusts the selected value. Confirm
opens the host-address text editor, or a dropdown on rows with choices (an arrow after
their value): their choices in a small window of its own under the row (above it when
there is more room there), the current one marked by a bar. Up/down move in it,
Confirm picks, Back or a click elsewhere closes it. Numeric rows adjust
directly with left/right; clicking or confirming them never opens a keypad, except
the frame rate, whose Confirm opens a dropdown of 30, 60, 90, 120, 150 and 240 (left/right
still go one frame at a time). Each compound setting has
one focus stop per editable value. Mouse and physical keyboard remain supported.

Above the guides, a pinned help box explains the row last focused or pointed at with
the mouse: the comment lines above its key in the default ini (`config.c` DEFAULT_INI,
`config_help`; wrapped lines joined, "Default: x" last), or a text of the row's own
(Per-quest overrides, Check hub server). It keeps room for the longest help of the tab,
so moving between rows doesn't move the guides. The ini's text is English; each line goes
through `settings_locale.h`, so a translation added there is used.

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

Menu Options offers English/Spanish and interface scale 50–200% (`MenuScale` under
`[Application]`). The root scales with font/DPI, is bounded by the viewport and
keeps its top position when conditional fields appear. Tabs wrap to two rows
when needed. Labels wrap; long values truncate without horizontal scrolling.
Focus has both a filled background and a bright outline; the active tab has an
underline. The full panel scrolls, including actions; no nested scroll region is
required.

## Validation

`make test-settings-locale` checks English/Spanish label and error-message
coverage, duplicate catalog entries, and nonempty translations.

`make test-settings` covers INI round trips, MenuScale bounds, Auto/Desktop,
environment values, comments, aliases, quest removal, invalid input, failed
writes and live settings.

`make test-settings-panel` drives real ImGui without game files or a graphics
device: six tabs and shoulder repeat, first-field focus, editor cancellation,
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
