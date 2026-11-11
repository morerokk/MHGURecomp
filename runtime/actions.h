// Game-action rebinding (docs/IMPLEMENTATION-NOTES.md, docs/input-rework/ACTIONS.md of the research branch).
//
// The host samples the bindings of [Actions.Pad] / [Actions.Kbm] into a set of active actions (act_sample, on the
// window thread, once per host_input_update, together with the Switch pad word it sets). The game thread latches the
// set when it polls the pad, and runtime/hle/input_view.c hooks the engine's two button translators (f_5de4b0 for the
// gameplay view mb1, f_5de4f8 for the menu view mb2): act_view() replaces the buttons of the word they are given by
// those of the active actions of the context (a hunt, a town, the menus). Every button of the game is an action in
// each of the three contexts. Nothing else of the game is touched, and with [Actions] Enable=0 (the default) nothing is
// hooked at all.
//
// Pure C: no guest memory and no window; the host's keys, mouse and controller come in through ActHost. The ini
// parser needs only runtime/config.c's key names.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

struct ActProfile;

// ---- the actions
typedef enum {
    // a hunt (the gameplay view while a hunter plays)
    ACT_DODGE, ACT_DASH, ACT_ATTACK_X, ACT_ATTACK_A, ACT_ITEM_USE, ACT_CHORD_XA,
    // the menus (the menu view)
    ACT_MENU_CONFIRM, ACT_MENU_CANCEL, ACT_MENU_UP, ACT_MENU_DOWN, ACT_MENU_LEFT, ACT_MENU_RIGHT,
    ACT_MENU_TAB_L, ACT_MENU_TAB_R, ACT_MENU_X, ACT_MENU_Y, ACT_MENU_PAGE_L, ACT_MENU_PAGE_R,
    // a hunt, the rest of the pad
    ACT_INTERACT, ACT_FIELD_L, ACT_FIELD_ZL, ACT_FIELD_ZR, ACT_CROSS_UP, ACT_CROSS_DOWN, ACT_CROSS_LEFT, ACT_CROSS_RIGHT,
    ACT_FIELD_L3, ACT_FIELD_R3, ACT_PAUSE, ACT_MAP,
    // a hunt: the weapon's R-action (guard, charge, aim), apart from Dash once split (act_split)
    ACT_WEAPON_R,
    // a hunt: crouching (B while the hunter stands still), apart from Dodge once split (act_crouch_split)
    ACT_CROUCH,
    // a hunt, while the item bar is open (L held): the bar's next (A) / previous (Y) item, apart from AttackA / ItemUse ...
    ACT_ITEMBAR_RIGHT, ACT_ITEMBAR_LEFT,
    // the menus, the rest of the pad
    ACT_MENU_PLUS, ACT_MENU_MINUS, ACT_MENU_L3, ACT_MENU_R3,
    // a town or the Hub (the gameplay view while the town avatar plays)
    ACT_TOWN_INTERACT, ACT_TOWN_CANCEL, ACT_TOWN_X, ACT_TOWN_Y, ACT_TOWN_L, ACT_TOWN_RUN, ACT_TOWN_ZL, ACT_TOWN_ZR,
    ACT_TOWN_UP, ACT_TOWN_DOWN, ACT_TOWN_LEFT, ACT_TOWN_RIGHT, ACT_TOWN_L3, ACT_TOWN_R3, ACT_TOWN_PLUS, ACT_TOWN_MINUS,
    ACT_COUNT
} ActId;
// The contexts. An action belongs to one of the first three. For act_view a hunt is one of three: ACT_VIEW_MB1 with
// the weapon out (the A button is AttackA's), ACT_VIEW_SHEATHED with it put away (A is Interact's) and
// ACT_VIEW_HUNT_ANY when that is not known or does not apply (the Prowler, no hunter on screen): both press A.
enum { ACT_VIEW_MB1, ACT_VIEW_MB2, ACT_VIEW_TOWN, ACT_VIEWS };
#define ACT_VIEW_SHEATHED ACT_VIEWS
#define ACT_VIEW_HUNT_ANY (ACT_VIEWS + 1)
// OR-ed into a hunt view for act_view: the hunter's item bar is open (L held). A and Y then come only from ItemBarRight /
// ItemBarLeft and every other action that delivers them is silent: state-scoped actions (actions.c, scopes[])
#define ACT_VIEW_BAR 0x100
#define ACT_KCR  16  // "the Switch button of the dash role": R, or ZR with the game's second Control Type (options +0x446E)
#define ACT_KCZR 17  // the other one of the two: ZR, or R

const char *act_name(int id);             // "Dodge"
const char *act_label(int id);            // "Dodge / roll": for a controls screen
int act_find(const char *name);           // case-insensitive, spaces and '_' ignored; -1: none
int act_view_of(int id);                  // ACT_VIEW_MB1 / MB2 / TOWN
const char *act_view_name(int view);      // "Hunt", "Menus", "Town"
uint32_t act_ew_bits(int id, bool dash_zr);  // the engine-word bits (INPUTS-MASTER B.1) the action delivers (WeaponR: none)
// The virtual button the weapon R-action is when it is split from Dash: a bit of the gameplay word that no pad button
// is, which the game's R-role getter (f_85f93c) returns to the callers that are weapon moves (hle/input_view.c)
#define ACT_VBIT_WEAPON_R 0x8000u
bool act_split(void);                     // Dash and the weapon R-action are two inputs (SplitDashWeaponR=1, or WeaponR is bound)
// The settings menu's confirm position (config.h MENU_CONFIRM_SOUTH / EAST) that follows the controller's MenuConfirm
// binding while the layer is on; -1: not bound to the bottom or right button, use the menu's own setting
int act_menu_confirm_policy(void);
bool act_menus_on(void);   // the layer and ManageMenus are on: the menu view takes the actions (MenuUp ...)
bool act_split_in(const struct ActProfile *p);
// The hunter's B button is the roll while the left stick is pushed and the crouch while it is not. With Crouch bound
// to an input of its own (any device or weapon class), or with Dodge and Interact on one input, the two are told apart: with the weapon away Crouch delivers B
// only while the stick is not pushed and Dodge only while it is, so an input shared by Interact and Dodge is a pure
// interaction standing still (nothing crouches, nothing waits) and a roll while moving. Crouch never shares with Interact.
#define ACT_STICK_L_DIRS 0xf0000u  // the left stick pushed past half way, in the engine word (INPUTS-MASTER B.1)
bool act_crouch_split(void);
bool act_crouch_split_in(const struct ActProfile *p);
uint32_t act_virtual_bits(uint64_t active);  // the virtual bits the gameplay word gets (any thread): 0 unless split
int act_default_switch(int id);           // the Switch button (input.h BTN_*, ACT_KCR, ACT_KCZR) Tier 0 gives it; -1: none
int act_default_button(int id, bool dash_zr);  // the same, the dash role turned into R or ZR: a Switch button (input.h BTN_*); -1: none
const char *act_shared_with(int id);      // "" or the actions that are the same game input (documented, not an error)
// The actions whose binding this one uses while it has none of its own, in order (-1 ends the list): TownInteract
// follows MenuConfirm, then Interact, which follows AttackA. With none bound it is the default (Tier 0).
const int *act_fallbacks(int id);
int act_order(int view, int *ids, int max);  // the actions of a context in the order a controls screen lists them

// ---- inputs. A code is: an SDL scancode (< 0x1000); KEY_MOUSE + button / KEY_WHEEL_* (config.h); ACT_PAD + SDL gamepad
// button, or ACT_PAD_LT / ACT_PAD_RT for a trigger; ACT_SW + dev * 0x20 + Switch button: what Tier 0 (the keybinds and
// controller of the settings) gives that Switch button (dev 0 the controller, 1 keyboard and mouse, 2 both).
#define ACT_PAD    0x2000
#define ACT_PAD_LT (ACT_PAD + 0x40)
#define ACT_PAD_RT (ACT_PAD + 0x41)
#define ACT_SW     0x3000
#define ACT_WEAPONS 15   // GreatSword SnS Hammer Lance HBG LBG LongSword SwitchAxe Gunlance Bow DualBlades Horn InsectGlaive ChargeBlade Prowler
#define ACT_VARS   (ACT_WEAPONS + 1)  // 0: no weapon override, 1..15: that class
#define ACT_ALTS   3     // "a | b | c"
#define ACT_CHORD  3     // "a+b+c"
const char *act_weapon_name(int cls);  // 0..14
int act_weapon_find(const char *name);

typedef struct { int n; int code[ACT_CHORD]; } ActChord;
typedef struct { bool set; int n; ActChord alt[ACT_ALTS]; } ActBinding;  // set with n == 0: "None" (unbound)
typedef struct {
    ActBinding base[ACT_COUNT];
    ActBinding weapon[ACT_WEAPONS][ACT_COUNT];
    bool has_weapon[ACT_WEAPONS];
} ActDevice;
typedef struct ActProfile {
    bool enable;    // [Actions] Enable: the gameplay view (hunt and town)
    bool menus;     // ManageMenus: the menu view too
    bool pointer;   // Pointer: feed the engine's pointer block
    bool split;     // SplitDashWeaponR: Dash and the weapon R-action are two inputs (also when WeaponR has a binding)
    bool sheathed_attack;  // SheathedAttackA: AttackA presses A with the weapon put away too (default: only Interact does)
    int shared_frames;     // SharedInteractFrames: how long Interact is tried first on an input other hunt actions share (0: 1)
    bool log;       // Log: one line per frame
    int scheme;     // Scheme: which control scheme the settings screen shows (ACT_SCHEME_*; 0: the device last used)
    int layout;     // Layout: Simple or Advanced (ACT_LAYOUT_*; 0: Simple, unless the profile already has bindings)
    ActDevice dev[2];  // 0 [Actions.Pad], 1 [Actions.Kbm]
} ActProfile;

enum { ACT_SCHEME_AUTO, ACT_SCHEME_PAD, ACT_SCHEME_KBM };           // [Actions] Scheme = Controller | Keyboard
enum { ACT_LAYOUT_AUTO, ACT_LAYOUT_SIMPLE, ACT_LAYOUT_ADVANCED };  // [Actions] Layout = Simple | Advanced
int act_layout(const ActProfile *p);  // SIMPLE or ADVANCED: the profile's, else Advanced if it has any binding, else Simple

// ---- the Simple layout: merged rows (the settings screen; the engine only ever sees the actions)
// A row is one action (its head) and the actions that go with it in other contexts: Confirm is MenuConfirm and the
// town's TownInteract. Setting a row writes the head and clears its members so that they keep following it; a member
// that does not follow the head gets a copy.
typedef struct { const char *name; const char *label; int group; int head; int members[3]; } ActRow;
int act_row_count(void);
const ActRow *act_row(int i);
const char *act_row_group_name(int group);  // 0 "Hunt and town", 1 "Menus"
int act_row_of(int id);                     // the row an action is in
uint32_t act_row_views(int row);            // bit per ACT_VIEW_* the row's actions are read in
typedef enum { ACT_ROW_DEFAULT, ACT_ROW_BOUND, ACT_ROW_UNSET, ACT_ROW_CUSTOM } ActRowState;
// What a row reads for a device: *shown is its head's binding (NULL-like: n 0 and set false for Default); CUSTOM when
// a member reads something else (edited apart in Advanced)
ActRowState act_row_state(const ActProfile *p, int dev, int row, ActBinding *shown);
void act_row_set(ActProfile *p, int dev, int row, const ActBinding *b);  // b NULL: Default; b with n 0: not set
// Another row in the same contexts has this input: Simple does not allow two (the Interact row, which may share an
// input with other hunt actions by design, and rows that follow another are left alone)
bool act_rows_clash(const ActProfile *p, int dev, int a, int b);
// After row was set to an input: every other row of the same contexts that reads it (an alternative, or its default)
// loses it and is left not set when that was its last one. Returns the rows changed (bit per row).
uint64_t act_row_claim(ActProfile *p, int dev, int row, int code, int (*sw_sources)(void *, int, int, int *, int),
                       void *user, bool dash_zr);
int act_rows_unset(const ActProfile *p, int dev, uint64_t *which);  // how many rows are not set (and which)

// ---- ini
void act_set_log(void (*fn)(const char *line));  // where messages go (default stderr)
// Parses the [Actions*] sections of an ini text into *p (zeroed first). Unknown names, bad tokens and conflicts are
// reported through the log; they never fail the parse (an ignored line leaves that action on its default).
void act_parse(const char *text, ActProfile *p);
bool act_load(const char *path, ActProfile *p);  // act_parse of a file; false: unreadable
// The [Actions*] sections as the ini has them (only what differs from the defaults); returns the length written.
size_t act_format(const ActProfile *p, char *buf, size_t size);
// Writes the profile into the ini at `path`: its [Actions*] sections are replaced by act_format's (added at the end
// when it had none), every other line is kept as it is. false: the file could not be written.
bool act_save(const char *path, const ActProfile *p);
// Conflict and shared-input warnings of a profile, one line each through the log; returns how many.
int act_validate(const ActProfile *p);
// Parses one binding value ("key:space | mouse:x1", "pad:east", "key:leftctrl+key:e", "None") into *b. false: bad
// (what was understood is kept, the rest ignored).
bool act_parse_binding(const char *value, ActBinding *b);
void act_format_binding(const ActBinding *b, char *buf, size_t size);
bool act_parse_input(const char *token, int *code);  // "key:space" -> code
void act_format_input(int code, char *buf, size_t size);
void act_input_label(int code, char *buf, size_t size);  // "Space", "Mouse left", "Pad south": for a controls screen
int act_input_device(int code);                 // 0 the controller, 1 keyboard and mouse, -1 neither (a sw: token)

// ---- sampling (window thread)
typedef struct {
    uint64_t hold[ACT_VARS];   // the actions held, per weapon variant
    uint64_t pulse[ACT_VARS];  // pressed since the last sample (a press shorter than a game frame is still delivered)
    int ptr_x, ptr_y;          // pointer feed (M2), in the 1280x720 UI space
    bool ptr_down, ptr_valid;
} ActSnap;

typedef struct {
    void *user;
    bool (*held)(void *user, int code);  // a physical input (key, mouse button, wheel pulse, controller) is down
    // The physical codes Tier 0 reads for a Switch button: dev 0 the controller's, 1 the keyboard's and mouse's.
    int (*sw_sources)(void *user, int btn, int dev, int *codes, int max);
    uint64_t sw_word;    // the Switch buttons Tier 0 has set (host_input.c's word, after its own suppressions)
    uint64_t sw_script;  // those of them from scripted input (MHGU_INPUT, the control file), which have no source
    bool suppress;       // Forge's menu or a text entry has the input: nothing is held
    bool dash_zr;        // the dash role is ZR (options +0x446E)
    uint64_t frame;      // presented frames (for act_inject)
} ActHost;

bool act_enabled(void);                         // a profile with Enable=1 is loaded
const ActProfile *act_profile(void);
void act_init(const char *ini_path);            // loads the profile (env MHGU_ACTIONS=0|1 overrides Enable); once, at startup
// Use this profile from now on (the controls screen, tests). Any thread: the sampler takes it at its next update; the
// game side hooks the translators the first time one with Enable=1 is in use (input_view_apply, game thread).
void act_use_profile(const ActProfile *p);
void act_get_profile(ActProfile *out);          // a copy of the one in use (any thread)
// What an action reads for a device and weapon variant (0: no class) right now: its own binding, the one it follows,
// or NULL for the default. *from (if not NULL): the action the binding belongs to (id itself, or one it follows).
const ActBinding *act_effective(const ActProfile *p, int dev, int id, int var, int *from);
void act_sample(const ActHost *h, ActSnap *out);
void act_inject(int id, unsigned frames, uint64_t frame_now);  // "act <Name> [frames]" (control file)
uint64_t act_injected(uint64_t frame_now);      // bitset of the injected, still running
bool act_uses_input(int code);                  // some explicit binding uses this input (the host acts on a wheel notch)

// ---- one input for Interact and other hunt actions (the game thread, with the weapon away)
// An input bound to Interact and to another hunt action (Dodge, AttackX...) presses both game buttons, and the game
// answers the other one (A + B is a roll, not a box opening). The game does not say beforehand whether something can be
// interacted with, so the press is tried as Interact first: only A for a few frames; if the hunter's action changes in
// that time (the box opens, the NPC turns, the gathering starts) the press was an interaction and the other actions
// stay off for as long as it is held; if not, the other actions get the press (a late roll), and a press that ends
// sooner still gets a short press of them.
typedef struct { int phase; uint64_t since, until, held; } ActGate;
uint64_t act_interact_share(int var);  // the hunt actions that share an input with Interact (any thread)
int act_shared_frames(void);
// `active` is the actions held (hold | pulse); share: act_interact_share; changed: the hunter's action differs from the
// one when the press began (the caller tracks it while gate->phase is 0); frame: the game's update count
uint64_t act_gate(ActGate *gate, uint64_t active, uint64_t share, bool changed, uint64_t frame, int delay);

// ---- the view (game thread)
// The word the translator gets in a context after the actions: the buttons the context's actions own are replaced by
// those of the active ones (`active` is hold | pulse of the weapon's variant); every other bit is as it was.
uint32_t act_view(int view, uint32_t ew, uint64_t active, bool dash_zr);
uint32_t act_view_managed(int view, bool dash_zr);  // the bits act_view replaces
#ifdef __cplusplus
}
#endif
