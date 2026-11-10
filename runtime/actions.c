// Game-action rebinding: the registry, the [Actions*] ini, the sampling of bindings into the active set and the
// view the game's translators get. See actions.h.
#include "actions.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <SDL3/SDL.h>

#include "config.h"
#include "keymap.h"

// ---- the Switch buttons (hle/input.h BTN_*) and their bits in the engine word (INPUTS-MASTER B.1)
enum { S_A, S_B, S_X, S_Y, S_LSTICK, S_RSTICK, S_L, S_R, S_ZL, S_ZR, S_PLUS, S_MINUS, S_LEFT, S_UP, S_RIGHT, S_DOWN };
static const uint32_t ew_of_btn[16] = {0x2000, 0x4000, 0x1000, 0x8000, 0x2,  0x4,  0x100, 0x200,
                                       0x400,  0x800,  0x8,    0x1,    0x80, 0x10, 0x20,  0x40};
static const char *const btn_name[16] = {"A", "B", "X", "Y", "LStick", "RStick", "L", "R", "ZL", "ZR", "Plus", "Minus",
                                         "Left", "Up", "Right", "Down"};

#define ROLE_VIRTUAL (-3)  // no pad button: the virtual weapon R bit (act_virtual_bits)
#define ROLE_DASH  (-1)  // the button of the dash role: R, or ZR (ACT_KCR)
#define ROLE_OTHER (-2)  // the other one of R / ZR (ACT_KCZR)
typedef struct {
    const char *name;
    int view;
    int btn;          // the Switch button whose engine bit it delivers (ROLE_*: by the game's Control Type)
    uint32_t extra;   // more bits delivered with it (the X+A chord: both)
    int def_sw;       // the Switch button Tier 0 gives it by default (ACT_KCR / ACT_KCZR: by role), -1 none
    int fb[3];        // the actions whose binding it follows while it has none of its own (-1 ends)
    const char *label;
    const char *shared;
} ActDef;
#define NOFB {-1, -1, -1}
#define FB1(a) {a, -1, -1}
#define FB2(a, b) {a, b, -1}
static const ActDef defs[ACT_COUNT] = {
    // a hunt
    [ACT_DODGE] = {"Dodge", ACT_VIEW_MB1, S_B, 0, S_B, NOFB, "Dodge / roll (B)",
                   "weapon B-actions (Lance, Switch Axe, Insect Glaive, bowguns, Prowler)"},
    [ACT_DASH] = {"Dash", ACT_VIEW_MB1, ROLE_DASH, 0, ACT_KCR, NOFB, "Dash (R)",
                  "the weapon R-action (guard, charge, aim) until WeaponR is bound or SplitDashWeaponR=1"},
    [ACT_WEAPON_R] = {"WeaponR", ACT_VIEW_MB1, ROLE_VIRTUAL, 0, ACT_KCR, FB1(ACT_DASH), "Weapon R-action: guard, charge, aim (R)", ""},
    [ACT_ATTACK_X] = {"AttackX", ACT_VIEW_MB1, S_X, 0, S_X, NOFB, "Attack X / draw weapon (X)", ""},
    [ACT_ATTACK_A] = {"AttackA", ACT_VIEW_MB1, S_A, 0, S_A, NOFB, "Attack A, weapon out (A)", ""},
    [ACT_ITEM_USE] = {"ItemUse", ACT_VIEW_MB1, S_Y, 0, S_Y, NOFB, "Use item / sheathe (Y)", ""},
    [ACT_CHORD_XA] = {"ChordXA", ACT_VIEW_MB1, S_X, 0x2000, -1, NOFB, "X + A together on one input", ""},
    [ACT_INTERACT] = {"Interact", ACT_VIEW_MB1, S_A, 0, S_A, FB1(ACT_ATTACK_A), "Interact, weapon away: gather, carve, talk (A)", ""},
    [ACT_FIELD_L] = {"FieldL", ACT_VIEW_MB1, S_L, 0, S_L, NOFB, "L button: camera, item select (L)", ""},
    [ACT_FIELD_ZL] = {"FieldZL", ACT_VIEW_MB1, S_ZL, 0, S_ZL, NOFB, "ZL button (ZL)", ""},
    [ACT_FIELD_ZR] = {"FieldZR", ACT_VIEW_MB1, ROLE_OTHER, 0, ACT_KCZR, NOFB, "ZR button (ZR)", ""},
    [ACT_CROSS_UP] = {"CrossUp", ACT_VIEW_MB1, S_UP, 0, S_UP, NOFB, "D-pad up", ""},
    [ACT_CROSS_DOWN] = {"CrossDown", ACT_VIEW_MB1, S_DOWN, 0, S_DOWN, NOFB, "D-pad down", ""},
    [ACT_CROSS_LEFT] = {"CrossLeft", ACT_VIEW_MB1, S_LEFT, 0, S_LEFT, NOFB, "D-pad left", ""},
    [ACT_CROSS_RIGHT] = {"CrossRight", ACT_VIEW_MB1, S_RIGHT, 0, S_RIGHT, NOFB, "D-pad right", ""},
    [ACT_FIELD_L3] = {"FieldL3", ACT_VIEW_MB1, S_LSTICK, 0, S_LSTICK, NOFB, "Left stick click (L3)", ""},
    [ACT_FIELD_R3] = {"FieldR3", ACT_VIEW_MB1, S_RSTICK, 0, S_RSTICK, NOFB, "Right stick click (R3)", ""},
    [ACT_PAUSE] = {"Pause", ACT_VIEW_MB1, S_PLUS, 0, S_PLUS, NOFB, "Start menu (+)", ""},
    [ACT_MAP] = {"Map", ACT_VIEW_MB1, S_MINUS, 0, S_MINUS, NOFB, "Minus button (-)", ""},
    // the menus
    [ACT_MENU_CONFIRM] = {"MenuConfirm", ACT_VIEW_MB2, S_A, 0, S_A, NOFB, "Confirm (A)", ""},
    [ACT_MENU_CANCEL] = {"MenuCancel", ACT_VIEW_MB2, S_B, 0, S_B, NOFB, "Cancel / back (B)", ""},
    [ACT_MENU_UP] = {"MenuUp", ACT_VIEW_MB2, S_UP, 0, S_UP, NOFB, "Up", ""},
    [ACT_MENU_DOWN] = {"MenuDown", ACT_VIEW_MB2, S_DOWN, 0, S_DOWN, NOFB, "Down", ""},
    [ACT_MENU_LEFT] = {"MenuLeft", ACT_VIEW_MB2, S_LEFT, 0, S_LEFT, NOFB, "Left", ""},
    [ACT_MENU_RIGHT] = {"MenuRight", ACT_VIEW_MB2, S_RIGHT, 0, S_RIGHT, NOFB, "Right", ""},
    [ACT_MENU_TAB_L] = {"MenuTabL", ACT_VIEW_MB2, S_L, 0, S_L, NOFB, "Tab left (L)", ""},
    [ACT_MENU_TAB_R] = {"MenuTabR", ACT_VIEW_MB2, S_R, 0, S_R, NOFB, "Tab right (R)", ""},
    [ACT_MENU_X] = {"MenuX", ACT_VIEW_MB2, S_X, 0, S_X, NOFB, "Menu action X (X)", ""},
    [ACT_MENU_Y] = {"MenuY", ACT_VIEW_MB2, S_Y, 0, S_Y, NOFB, "Menu action Y (Y)", ""},
    [ACT_MENU_PAGE_L] = {"MenuPageL", ACT_VIEW_MB2, S_ZL, 0, S_ZL, NOFB, "Page left (ZL)", ""},
    [ACT_MENU_PAGE_R] = {"MenuPageR", ACT_VIEW_MB2, S_ZR, 0, S_ZR, NOFB, "Page right (ZR)", ""},
    [ACT_MENU_PLUS] = {"MenuPlus", ACT_VIEW_MB2, S_PLUS, 0, S_PLUS, FB1(ACT_PAUSE), "Plus in menus (+)", ""},
    [ACT_MENU_MINUS] = {"MenuMinus", ACT_VIEW_MB2, S_MINUS, 0, S_MINUS, FB1(ACT_MAP), "Minus in menus (-)", ""},
    [ACT_MENU_L3] = {"MenuL3", ACT_VIEW_MB2, S_LSTICK, 0, S_LSTICK, NOFB, "Left stick click in menus (L3)", ""},
    [ACT_MENU_R3] = {"MenuR3", ACT_VIEW_MB2, S_RSTICK, 0, S_RSTICK, NOFB, "Right stick click in menus (R3)", ""},
    // a town
    [ACT_TOWN_INTERACT] = {"TownInteract", ACT_VIEW_TOWN, S_A, 0, S_A, FB2(ACT_MENU_CONFIRM, ACT_INTERACT), "Talk / interact (A)", ""},
    [ACT_TOWN_CANCEL] = {"TownCancel", ACT_VIEW_TOWN, S_B, 0, S_B, FB2(ACT_MENU_CANCEL, ACT_DODGE), "B button in town (B)", ""},
    [ACT_TOWN_X] = {"TownX", ACT_VIEW_TOWN, S_X, 0, S_X, FB1(ACT_ATTACK_X), "X button in town (X)", ""},
    [ACT_TOWN_Y] = {"TownY", ACT_VIEW_TOWN, S_Y, 0, S_Y, FB1(ACT_ITEM_USE), "Y button in town (Y)", ""},
    [ACT_TOWN_L] = {"TownL", ACT_VIEW_TOWN, S_L, 0, S_L, FB1(ACT_FIELD_L), "L button in town (L)", ""},
    [ACT_TOWN_RUN] = {"TownRun", ACT_VIEW_TOWN, ROLE_DASH, 0, ACT_KCR, FB1(ACT_DASH), "Run (R)", ""},
    [ACT_TOWN_ZL] = {"TownZL", ACT_VIEW_TOWN, S_ZL, 0, S_ZL, FB1(ACT_FIELD_ZL), "ZL button in town (ZL)", ""},
    [ACT_TOWN_ZR] = {"TownZR", ACT_VIEW_TOWN, ROLE_OTHER, 0, ACT_KCZR, FB1(ACT_FIELD_ZR), "ZR button in town (ZR)", ""},
    [ACT_TOWN_UP] = {"TownUp", ACT_VIEW_TOWN, S_UP, 0, S_UP, FB1(ACT_CROSS_UP), "D-pad up in town", ""},
    [ACT_TOWN_DOWN] = {"TownDown", ACT_VIEW_TOWN, S_DOWN, 0, S_DOWN, FB1(ACT_CROSS_DOWN), "D-pad down in town", ""},
    [ACT_TOWN_LEFT] = {"TownLeft", ACT_VIEW_TOWN, S_LEFT, 0, S_LEFT, FB1(ACT_CROSS_LEFT), "D-pad left in town", ""},
    [ACT_TOWN_RIGHT] = {"TownRight", ACT_VIEW_TOWN, S_RIGHT, 0, S_RIGHT, FB1(ACT_CROSS_RIGHT), "D-pad right in town", ""},
    [ACT_TOWN_L3] = {"TownL3", ACT_VIEW_TOWN, S_LSTICK, 0, S_LSTICK, FB1(ACT_FIELD_L3), "Left stick click in town (L3)", ""},
    [ACT_TOWN_R3] = {"TownR3", ACT_VIEW_TOWN, S_RSTICK, 0, S_RSTICK, FB1(ACT_FIELD_R3), "Right stick click in town (R3)", ""},
    [ACT_TOWN_PLUS] = {"TownPlus", ACT_VIEW_TOWN, S_PLUS, 0, S_PLUS, FB1(ACT_PAUSE), "Start menu in town (+)", ""},
    [ACT_TOWN_MINUS] = {"TownMinus", ACT_VIEW_TOWN, S_MINUS, 0, S_MINUS, FB1(ACT_MAP), "Minus button in town (-)", ""},
};
// The order a controls screen lists them in
static const int order[] = {
    ACT_ATTACK_X, ACT_ATTACK_A, ACT_CHORD_XA, ACT_DODGE, ACT_ITEM_USE, ACT_INTERACT, ACT_DASH, ACT_WEAPON_R, ACT_FIELD_ZR, ACT_FIELD_L,
    ACT_FIELD_ZL, ACT_CROSS_UP, ACT_CROSS_DOWN, ACT_CROSS_LEFT, ACT_CROSS_RIGHT, ACT_FIELD_L3, ACT_FIELD_R3, ACT_PAUSE,
    ACT_MAP,
    ACT_MENU_CONFIRM, ACT_MENU_CANCEL, ACT_MENU_X, ACT_MENU_Y, ACT_MENU_UP, ACT_MENU_DOWN, ACT_MENU_LEFT, ACT_MENU_RIGHT,
    ACT_MENU_TAB_L, ACT_MENU_TAB_R, ACT_MENU_PAGE_L, ACT_MENU_PAGE_R, ACT_MENU_PLUS, ACT_MENU_MINUS, ACT_MENU_L3, ACT_MENU_R3,
    ACT_TOWN_INTERACT, ACT_TOWN_CANCEL, ACT_TOWN_X, ACT_TOWN_Y, ACT_TOWN_RUN, ACT_TOWN_ZR, ACT_TOWN_L, ACT_TOWN_ZL,
    ACT_TOWN_UP, ACT_TOWN_DOWN, ACT_TOWN_LEFT, ACT_TOWN_RIGHT, ACT_TOWN_L3, ACT_TOWN_R3, ACT_TOWN_PLUS, ACT_TOWN_MINUS,
};
static const char *const weapon_names[ACT_WEAPONS] = {"GreatSword", "SnS", "Hammer", "Lance", "HBG", "LBG", "LongSword",
                                                      "SwitchAxe", "Gunlance", "Bow", "DualBlades", "Horn",
                                                      "InsectGlaive", "ChargeBlade", "Prowler"};

const char *act_name(int id) { return id >= 0 && id < ACT_COUNT ? defs[id].name : ""; }
const char *act_label(int id) { return id >= 0 && id < ACT_COUNT ? defs[id].label : ""; }
int act_view_of(int id) { return defs[id].view; }
const char *act_view_name(int view) {
    return view == ACT_VIEW_MB1 ? "Hunt" : view == ACT_VIEW_MB2 ? "Menus" : view == ACT_VIEW_TOWN ? "Town" : "";
}
int act_default_switch(int id) { return defs[id].def_sw; }
const char *act_shared_with(int id) { return defs[id].shared; }
const int *act_fallbacks(int id) { return defs[id].fb; }
int act_order(int view, int *ids, int max) {
    int n = 0;
    for (unsigned i = 0; i < sizeof order / sizeof *order && n < max; i++)
        if (defs[order[i]].view == view) ids[n++] = order[i];
    return n;
}
static int role_btn(int btn, bool dash_zr) {
    if (btn == ROLE_DASH || btn == ACT_KCR) return dash_zr ? S_ZR : S_R;
    if (btn == ROLE_OTHER || btn == ACT_KCZR) return dash_zr ? S_R : S_ZR;
    return btn;
}
uint32_t act_ew_bits(int id, bool dash_zr) {
    const ActDef *d = &defs[id];
    if (d->btn == ROLE_VIRTUAL) return 0;
    return ew_of_btn[role_btn(d->btn, dash_zr)] | d->extra;
}
const char *act_weapon_name(int cls) { return cls >= 0 && cls < ACT_WEAPONS ? weapon_names[cls] : ""; }

// Names compare without case, spaces, '_' and '-'.
static void squash(const char *in, char *out, size_t size) {
    size_t n = 0;
    for (; *in && n + 1 < size; in++)
        if (!isspace((unsigned char)*in) && *in != '_' && *in != '-') out[n++] = (char)tolower((unsigned char)*in);
    out[n] = 0;
}
static bool same_name(const char *a, const char *b) {
    char x[96], y[96];
    squash(a, x, sizeof x), squash(b, y, sizeof y);
    return !strcmp(x, y);
}
int act_find(const char *name) {
    for (int i = 0; i < ACT_COUNT; i++)
        if (same_name(name, defs[i].name)) return i;
    // short names: the menus' pair (the town's A / B follow it while they have no binding of their own)
    if (same_name(name, "Confirm")) return ACT_MENU_CONFIRM;
    if (same_name(name, "Cancel")) return ACT_MENU_CANCEL;
    return -1;
}
int act_weapon_find(const char *name) {
    for (int i = 0; i < ACT_WEAPONS; i++)
        if (same_name(name, weapon_names[i])) return i;
    return -1;
}

// ---- messages
static void (*log_fn)(const char *);
void act_set_log(void (*fn)(const char *line)) { log_fn = fn; }
static void say(const char *fmt, ...) {
    char line[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (log_fn) log_fn(line);
    else fprintf(stderr, "%s\n", line);
}

// ---- inputs
static const struct { const char *name; int code; const char *label; } pad_names[] = {
    {"south", ACT_PAD + SDL_GAMEPAD_BUTTON_SOUTH, "Pad South"}, {"east", ACT_PAD + SDL_GAMEPAD_BUTTON_EAST, "Pad East"},
    {"west", ACT_PAD + SDL_GAMEPAD_BUTTON_WEST, "Pad West"}, {"north", ACT_PAD + SDL_GAMEPAD_BUTTON_NORTH, "Pad North"},
    {"lshoulder", ACT_PAD + SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, "Pad L1"},
    {"rshoulder", ACT_PAD + SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, "Pad R1"},
    {"ltrigger", ACT_PAD_LT, "Pad L2"}, {"rtrigger", ACT_PAD_RT, "Pad R2"},
    {"lstick", ACT_PAD + SDL_GAMEPAD_BUTTON_LEFT_STICK, "Pad L3"},
    {"rstick", ACT_PAD + SDL_GAMEPAD_BUTTON_RIGHT_STICK, "Pad R3"},
    {"dpup", ACT_PAD + SDL_GAMEPAD_BUTTON_DPAD_UP, "D-pad Up"},
    {"dpdown", ACT_PAD + SDL_GAMEPAD_BUTTON_DPAD_DOWN, "D-pad Down"},
    {"dpleft", ACT_PAD + SDL_GAMEPAD_BUTTON_DPAD_LEFT, "D-pad Left"},
    {"dpright", ACT_PAD + SDL_GAMEPAD_BUTTON_DPAD_RIGHT, "D-pad Right"},
    {"start", ACT_PAD + SDL_GAMEPAD_BUTTON_START, "Pad Start"}, {"back", ACT_PAD + SDL_GAMEPAD_BUTTON_BACK, "Pad Back"},
};
static const struct { const char *name; int code; const char *label; } mouse_names[] = {
    {"left", KEY_MOUSE + SDL_BUTTON_LEFT, "Mouse Left"}, {"right", KEY_MOUSE + SDL_BUTTON_RIGHT, "Mouse Right"},
    {"middle", KEY_MOUSE + SDL_BUTTON_MIDDLE, "Mouse Middle"}, {"x1", KEY_MOUSE + SDL_BUTTON_X1, "Mouse 4"},
    {"x2", KEY_MOUSE + SDL_BUTTON_X2, "Mouse 5"}, {"wheelup", KEY_WHEEL_UP, "Wheel Up"},
    {"wheeldown", KEY_WHEEL_DOWN, "Wheel Down"},
};

static int key_by_name(const char *name) {  // "space", "Left Shift", "leftshift", "return"
    int c = config_key_code(name);
    if (c) return c;
    for (int i = 1; i < SDL_SCANCODE_COUNT; i++) {
        const char *n = SDL_GetScancodeName((SDL_Scancode)i);
        if (n && *n && same_name(n, name)) return i;
    }
    return 0;
}

bool act_parse_input(const char *tok, int *code) {
    while (isspace((unsigned char)*tok)) tok++;
    const char *colon = strchr(tok, ':');
    if (!colon) return false;
    char kind[16], val[64];
    snprintf(kind, sizeof kind, "%.*s", (int)(colon - tok < 15 ? colon - tok : 15), tok);
    snprintf(val, sizeof val, "%s", colon + 1);
    for (char *e = val + strlen(val); e > val && isspace((unsigned char)e[-1]);) *--e = 0;
    if (!strcasecmp(kind, "key")) {
        int c = key_by_name(val);
        if (!c) return false;
        *code = c;
        return true;
    }
    if (!strcasecmp(kind, "mouse")) {
        for (size_t i = 0; i < sizeof mouse_names / sizeof *mouse_names; i++)
            if (same_name(val, mouse_names[i].name)) return *code = mouse_names[i].code, true;
        return false;
    }
    if (!strcasecmp(kind, "pad")) {
        for (size_t i = 0; i < sizeof pad_names / sizeof *pad_names; i++)
            if (same_name(val, pad_names[i].name)) return *code = pad_names[i].code, true;
        return false;
    }
    if (!strcasecmp(kind, "sw")) {  // sw:A, sw:Dash (the dash role), sw:DashOther (the other of R / ZR)
        if (same_name(val, "dash") || same_name(val, "kcr")) return *code = ACT_SW + 2 * 0x20 + ACT_KCR, true;
        if (same_name(val, "dashother") || same_name(val, "kczr")) return *code = ACT_SW + 2 * 0x20 + ACT_KCZR, true;
        for (int i = 0; i < 16; i++)
            if (same_name(val, btn_name[i])) return *code = ACT_SW + 2 * 0x20 + i, true;
        return false;
    }
    return false;
}

void act_format_input(int code, char *buf, size_t size) {
    if (code >= ACT_SW && code < ACT_SW + 0x60) {
        int dev = (code - ACT_SW) / 0x20, b = (code - ACT_SW) % 0x20;
        snprintf(buf, size, "sw%s:%s", dev == 0 ? ".pad" : dev == 1 ? ".kbm" : "",
                 b == ACT_KCR ? "Dash" : b == ACT_KCZR ? "DashOther" : btn_name[b & 15]);
    } else if (code >= ACT_PAD && code < ACT_PAD + 0x80) {
        for (size_t i = 0; i < sizeof pad_names / sizeof *pad_names; i++)
            if (pad_names[i].code == code) return (void)snprintf(buf, size, "pad:%s", pad_names[i].name);
        snprintf(buf, size, "pad:#%d", code - ACT_PAD);
    } else if (code >= KEY_MOUSE) {
        for (size_t i = 0; i < sizeof mouse_names / sizeof *mouse_names; i++)
            if (mouse_names[i].code == code) return (void)snprintf(buf, size, "mouse:%s", mouse_names[i].name);
        snprintf(buf, size, "mouse:#%d", code - KEY_MOUSE);
    } else {
        const char *n = SDL_GetScancodeName((SDL_Scancode)code);
        snprintf(buf, size, "key:%s", n && *n ? n : "?");
    }
}

int act_input_device(int code) {
    if (code >= ACT_SW && code < ACT_SW + 0x60) return -1;
    return code >= ACT_PAD && code < ACT_PAD + 0x80 ? 0 : 1;
}
void act_input_label(int code, char *buf, size_t size) {
    for (size_t i = 0; i < sizeof pad_names / sizeof *pad_names; i++)
        if (pad_names[i].code == code) return (void)snprintf(buf, size, "%s", pad_names[i].label);
    for (size_t i = 0; i < sizeof mouse_names / sizeof *mouse_names; i++)
        if (mouse_names[i].code == code) return (void)snprintf(buf, size, "%s", mouse_names[i].label);
    if (code > 0 && code < SDL_SCANCODE_COUNT) {
        const char *n = SDL_GetScancodeName((SDL_Scancode)code);
        if (n && *n) return (void)snprintf(buf, size, "%s", n);
    }
    act_format_input(code, buf, size);
}

// "a+b | c": up to ACT_ALTS alternatives of up to ACT_CHORD inputs. "None" (or empty): unbound, on purpose.
bool act_parse_binding(const char *value, ActBinding *b) {
    memset(b, 0, sizeof *b);
    b->set = true;
    char buf[256];
    snprintf(buf, sizeof buf, "%s", value);
    bool ok = true;
    char *s = buf;
    while (isspace((unsigned char)*s)) s++;
    if (!*s || !strncasecmp(s, "none", 4)) return true;
    char *save = NULL;
    for (char *alt = strtok_r(s, "|", &save); alt; alt = strtok_r(NULL, "|", &save)) {
        ActChord ch = {0};
        char *save2 = NULL;
        bool good = true;
        for (char *in = strtok_r(alt, "+", &save2); in; in = strtok_r(NULL, "+", &save2)) {
            int code;
            if (ch.n == ACT_CHORD || !act_parse_input(in, &code)) { good = false; ok = false; say("[actions] input \"%s\" ignored", in); continue; }
            ch.code[ch.n++] = code;
        }
        if (good && ch.n) {
            if (b->n == ACT_ALTS) { say("[actions] more than %d alternatives: the rest ignored", ACT_ALTS); ok = false; break; }
            b->alt[b->n++] = ch;
        }
    }
    return ok;
}

void act_format_binding(const ActBinding *b, char *buf, size_t size) {
    size_t n = 0;
    buf[0] = 0;
    if (!b->n) return (void)snprintf(buf, size, "None");
    for (int a = 0; a < b->n; a++) {
        if (a) n += (size_t)snprintf(buf + n, size - n, " | ");
        for (int i = 0; i < b->alt[a].n && n < size; i++) {
            char one[64];
            act_format_input(b->alt[a].code[i], one, sizeof one);
            n += (size_t)snprintf(buf + n, size - n, "%s%s", i ? "+" : "", one);
        }
    }
}

// ---- ini
static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}
static bool parse_bool(const char *v) { return !strcasecmp(v, "1") || !strcasecmp(v, "true") || !strcasecmp(v, "on") || !strcasecmp(v, "yes"); }
static bool actions_section(const char *name) {  // "Actions" or "Actions.<...>"
    return !strncasecmp(name, "Actions", 7) && (!name[7] || name[7] == '.' || name[7] == ']');
}

void act_parse(const char *text, ActProfile *p) {
    memset(p, 0, sizeof *p);
    enum { NONE, MAIN, DEV } kind = NONE;
    int dev = 0, wc = -1;  // wc: the weapon class of the section, -1 none
    int line_no = 0;
    for (const char *s = text; *s;) {
        const char *nl = strchr(s, '\n');
        size_t len = nl ? (size_t)(nl - s) : strlen(s);
        char line[512];
        snprintf(line, sizeof line, "%.*s", (int)(len < sizeof line - 1 ? len : sizeof line - 1), s);
        s += len + (nl ? 1 : 0);
        line_no++;
        char *t = trim(line);
        if (*t == '[') {
            char *close = strchr(t, ']');
            kind = NONE, wc = -1;
            if (!close) continue;
            *close = 0;
            char name[96];
            snprintf(name, sizeof name, "%s", t + 1);
            if (!strcasecmp(name, "Actions")) kind = MAIN;
            else if (!strncasecmp(name, "Actions.", 8)) {
                const char *rest = name + 8;
                if (!strncasecmp(rest, "Pad", 3)) dev = 0, rest += 3;
                else if (!strncasecmp(rest, "Kbm", 3)) dev = 1, rest += 3;
                else { say("[actions] line %d: unknown section [%s] (ignored)", line_no, name); continue; }
                if (!*rest) kind = DEV;
                else if (!strncasecmp(rest, ".Weapon.", 8)) {
                    wc = act_weapon_find(rest + 8);
                    if (wc < 0) say("[actions] line %d: unknown weapon class \"%s\" (section ignored)", line_no, rest + 8);
                    else kind = DEV, p->dev[dev].has_weapon[wc] = true;
                } else say("[actions] line %d: unknown section [%s] (ignored)", line_no, name);
            }
            continue;
        }
        if (kind == NONE || !*t || *t == ';' || *t == '#') continue;
        char *eq = strchr(t, '=');
        if (!eq) { say("[actions] line %d: no '=' (ignored)", line_no); continue; }
        *eq = 0;
        char *key = trim(t), *val = eq + 1;
        for (char *c = val; *c; c++)  // (a trailing comment: ';' or '#' after a space)
            if ((*c == ';' || *c == '#') && (c == val || isspace((unsigned char)c[-1]))) { *c = 0; break; }
        val = trim(val);
        if (kind == MAIN) {
            if (same_name(key, "Enable")) p->enable = parse_bool(val);
            else if (same_name(key, "ManageMenus")) p->menus = parse_bool(val);
            else if (same_name(key, "Pointer")) p->pointer = parse_bool(val);
            else if (same_name(key, "SplitDashWeaponR")) p->split = parse_bool(val);
            else if (same_name(key, "SheathedAttackA")) p->sheathed_attack = parse_bool(val);
            else if (same_name(key, "SharedInteractFrames")) p->shared_frames = atoi(val) < 0 ? 0 : atoi(val) > 30 ? 30 : atoi(val);
            else if (same_name(key, "Log")) p->log = parse_bool(val);
            else say("[actions] line %d: unknown setting \"%s\" in [Actions] (ignored)", line_no, key);
            continue;
        }
        int id = act_find(key);
        if (id < 0) { say("[actions] line %d: unknown action \"%s\" (ignored)", line_no, key); continue; }
        ActBinding *b = wc >= 0 ? &p->dev[dev].weapon[wc][id] : &p->dev[dev].base[id];
        ActBinding tmp;
        if (!act_parse_binding(val, &tmp))
            say("[actions] line %d: %s = \"%s\": some inputs were not understood (ignored)", line_no, key, val);
        if (tmp.n || !*val || !strncasecmp(val, "none", 4)) *b = tmp;  // (a value with nothing usable leaves the default)
    }
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (!buf) return fclose(f), NULL;
    n = (long)fread(buf, 1, (size_t)n, f);
    buf[n] = 0;
    fclose(f);
    return buf;
}

bool act_load(const char *path, ActProfile *p) {
    char *buf = read_file(path);
    if (!buf) return false;
    act_parse(buf, p);
    free(buf);
    return true;
}

size_t act_format(const ActProfile *p, char *buf, size_t size) {
    size_t n = 0;
#define PUT(...) do { if (n < size) n += (size_t)snprintf(buf + n, size - n, __VA_ARGS__); } while (0)
    PUT("[Actions]\nEnable=%d\nManageMenus=%d\n", p->enable, p->menus);
    if (p->pointer) PUT("Pointer=1\n");
    if (p->split) PUT("SplitDashWeaponR=1\n");
    if (p->sheathed_attack) PUT("SheathedAttackA=1\n");
    if (p->shared_frames) PUT("SharedInteractFrames=%d\n", p->shared_frames);
    if (p->log) PUT("Log=1\n");
    static const char *const sect[2] = {"Pad", "Kbm"};
    for (int d = 0; d < 2; d++) {
        for (int w = -1; w < ACT_WEAPONS; w++) {
            const ActBinding *row = w < 0 ? p->dev[d].base : p->dev[d].weapon[w];
            bool any = false;
            for (int i = 0; i < ACT_COUNT; i++) any |= row[i].set;
            if (!any) continue;
            if (w < 0) PUT("\n[Actions.%s]\n", sect[d]);
            else PUT("\n[Actions.%s.Weapon.%s]\n", sect[d], weapon_names[w]);
            for (unsigned k = 0; k < sizeof order / sizeof *order; k++) {
                int i = order[k];
                if (!row[i].set) continue;
                char v[256];
                act_format_binding(&row[i], v, sizeof v);
                PUT("%s = %s\n", defs[i].name, v);
            }
        }
    }
#undef PUT
    return n < size ? n : size ? size - 1 : 0;
}

bool act_save(const char *path, const ActProfile *p) {
    char *old = read_file(path);
    size_t cap = (old ? strlen(old) : 0) + 65536;
    char *out = malloc(cap);
    if (!out) return free(old), false;
    size_t n = 0;
    bool skip = false;
    for (const char *s = old ? old : ""; *s;) {  // every line outside the [Actions*] sections, as it is
        const char *nl = strchr(s, '\n');
        size_t len = nl ? (size_t)(nl - s) + 1 : strlen(s);
        const char *t = s;
        while (*t == ' ' || *t == '\t') t++;
        if (*t == '[') skip = actions_section(t + 1);
        if (!skip) memcpy(out + n, s, len), n += len;
        s += len;
    }
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' ')) n--;
    if (n) out[n++] = '\n', out[n++] = '\n';
    n += act_format(p, out + n, cap - n);
    free(old);
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.actions.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return free(out), false;
    bool ok = fwrite(out, 1, n, f) == n;
    ok = fclose(f) == 0 && ok;
    free(out);
    if (!ok) return remove(tmp), false;
    remove(path);
    if (rename(tmp, path) != 0) return false;
    return true;
}

// ---- the profile in use. The sampler (window thread) reads it at every update and a controls screen may replace it:
// a short spin lock around both (the game thread only reads one flag of it).
static ActProfile prof;
static bool prof_on;
static atomic_bool prof_split;  // act_split_in(prof), for the game thread
static atomic_ullong share_tab[ACT_VARS];  // act_interact_share, per weapon variant
static atomic_int shared_frames_now = 5;
static atomic_flag prof_lock = ATOMIC_FLAG_INIT;
static void lock(void) { while (atomic_flag_test_and_set_explicit(&prof_lock, memory_order_acquire)) {} }
static void unlock(void) { atomic_flag_clear_explicit(&prof_lock, memory_order_release); }
bool act_enabled(void) { return prof_on; }
const ActProfile *act_profile(void) { return &prof; }
static const ActBinding *binding_in(const ActProfile *p, int dev, int id, int v, int *from);
// Two bindings press the same thing: the same inputs, or one a chord that has the other's inputs
static bool chords_overlap(const ActChord *x, const ActChord *y) {
    const ActChord *a = x->n <= y->n ? x : y, *b = x->n <= y->n ? y : x;  // a the smaller: all of its inputs in b
    for (int i = 0; i < a->n; i++) {
        bool in = false;
        for (int k = 0; k < b->n; k++) in |= a->code[i] == b->code[k];
        if (!in) return false;
    }
    return a->n > 0;
}
// The hunt actions that share an input with Interact for a weapon variant, either device (AttackA is Interact's own
// pair: with the weapon away it is not delivered anyway; actions that are the same binding by following are not shared)
static uint64_t share_in(const ActProfile *p, int v) {
    uint64_t m = 0;
    for (int d = 0; d < 2; d++) {
        int fi = ACT_INTERACT;
        const ActBinding *bi = binding_in(p, d, ACT_INTERACT, v, &fi);
        if (!bi) continue;
        for (int j = 0; j < ACT_COUNT; j++) {
            if (j == ACT_INTERACT || j == ACT_ATTACK_A || defs[j].view != ACT_VIEW_MB1 || j == ACT_WEAPON_R) continue;
            int fj = j;
            const ActBinding *bj = binding_in(p, d, j, v, &fj);
            if (!bj || fj == fi) continue;
            for (int a = 0; a < bi->n; a++)
                for (int e = 0; e < bj->n; e++)
                    if (chords_overlap(&bi->alt[a], &bj->alt[e])) m |= 1ull << j;
        }
    }
    return m;
}
uint64_t act_interact_share(int var) { return var >= 0 && var < ACT_VARS ? atomic_load(&share_tab[var]) : 0; }
int act_shared_frames(void) { return atomic_load(&shared_frames_now); }

uint64_t act_gate(ActGate *g, uint64_t active, uint64_t share, bool changed, uint64_t frame, int delay) {
    const uint64_t I = 1ull << ACT_INTERACT;
    bool amb = (active & I) && (active & share);
    if (g->phase == 4 && frame < g->until) {  // a press that ended while it was tried: the others get a short press
        return (active & ~I) | g->held;
    }
    if (!amb) {
        if (g->phase == 1 && !changed && g->held) {  // (released before it was known)
            g->phase = 4, g->until = frame + 3;
            return (active & ~I) | g->held;
        }
        g->phase = 0, g->held = 0;
        return active;
    }
    if (g->phase == 0 || g->phase == 4) g->phase = 1, g->since = frame, g->held = 0;
    if (g->phase == 1) {
        g->held |= active & share;
        if (changed) g->phase = 2;
        else if (frame - g->since >= (uint64_t)(delay > 0 ? delay : 5)) g->phase = 3;
    }
    if (g->phase == 3) return active & ~I;
    return active & ~share;
}
void act_use_profile(const ActProfile *p) {
    lock();
    prof = *p;
    prof_on = p->enable;
    atomic_store(&prof_split, act_split_in(p));
    for (int v = 0; v < ACT_VARS; v++) atomic_store(&share_tab[v], share_in(p, v));
    atomic_store(&shared_frames_now, p->shared_frames > 0 ? p->shared_frames : 5);
    unlock();
}
// Dash and the weapon R-action are one game input (the R-role getter) unless the profile asks for two: the flag, or a
// WeaponR binding of its own for any device or weapon class
bool act_split_in(const ActProfile *p) {
    if (p->split) return true;
    for (int d = 0; d < 2; d++) {
        if (p->dev[d].base[ACT_WEAPON_R].set) return true;
        for (int w = 0; w < ACT_WEAPONS; w++)
            if (p->dev[d].weapon[w][ACT_WEAPON_R].set) return true;
    }
    return false;
}
bool act_split(void) { return prof_on && atomic_load(&prof_split); }
uint32_t act_virtual_bits(uint64_t active) {
    return act_split() && (active >> ACT_WEAPON_R & 1) ? ACT_VBIT_WEAPON_R : 0;
}
void act_get_profile(ActProfile *out) {
    lock();
    *out = prof;
    unlock();
}

// The binding an action has for a device and weapon variant (v 0: none): the weapon class's own if it has one, else
// the base's, else that of the first action it follows that has one; none at all is "the default" (a NULL).
static const ActBinding *binding_in(const ActProfile *p, int dev, int id, int v, int *from) {
    const ActBinding *own = v > 0 && p->dev[dev].weapon[v - 1][id].set ? &p->dev[dev].weapon[v - 1][id]
                            : p->dev[dev].base[id].set ? &p->dev[dev].base[id] : NULL;
    if (own) {
        if (from) *from = id;
        return own;
    }
    for (int k = 0; k < 3 && defs[id].fb[k] >= 0; k++) {
        const ActBinding *b = binding_in(p, dev, defs[id].fb[k], v, from);
        if (b) return b;
    }
    return NULL;
}
const ActBinding *act_effective(const ActProfile *p, int dev, int id, int var, int *from) {
    return binding_in(p, dev, id, var, from);
}
static const ActBinding *binding_of(int dev, int id, int v) { return binding_in(&prof, dev, id, v, NULL); }

static bool is_physical(int c) { return c < ACT_SW || c >= ACT_SW + 0x60; }

int act_validate(const ActProfile *p) {
    int warnings = 0;
    for (int d = 0; d < 2; d++)
        for (int v = 0; v < ACT_VARS; v++) {
            if (v > 0 && !p->dev[d].has_weapon[v - 1]) continue;
            for (int i = 0; i < ACT_COUNT; i++) {
                int fi = i;
                const ActBinding *bi = binding_in(p, d, i, v, &fi);
                if (!bi) continue;
                if (fi == i)
                    for (int a = 0; a < bi->n; a++)
                        for (int k = 0; k < bi->alt[a].n; k++) {
                            int c = bi->alt[a].code[k];
                            if (c < SDL_SCANCODE_COUNT && key_reserved(c))
                                say("[actions] %s uses %s, which the recomp keeps for itself (%s)", defs[i].name,
                                    SDL_GetScancodeName((SDL_Scancode)c), key_reserved(c)), warnings++;
                        }
                for (int j = i + 1; j < ACT_COUNT; j++) {
                    int fj = j;
                    const ActBinding *bj = binding_in(p, d, j, v, &fj);
                    if (!bj || defs[i].view != defs[j].view || fi == fj) continue;
                    // (two actions that follow others: the conflict is the one of those, reported there)
                    if ((fi != i || fj != j) && defs[fi].view == defs[fj].view) continue;
                    // (A with the weapon out and A with it away are never read together)
                    if (!p->sheathed_attack && ((i == ACT_ATTACK_A && j == ACT_INTERACT) || (i == ACT_INTERACT && j == ACT_ATTACK_A))) continue;
                    for (int a = 0; a < bi->n; a++)
                        for (int e = 0; e < bj->n; e++) {
                            const ActChord *x = &bi->alt[a], *y = &bj->alt[e];
                            if (x->n != y->n) continue;  // (a chord and a single input differ: the chord is more specific)
                            bool eq = true;
                            for (int k = 0; k < x->n && eq; k++) {
                                bool found = false;
                                for (int m = 0; m < y->n; m++) found |= x->code[k] == y->code[m];
                                eq = found;
                            }
                            if (!eq) continue;
                            char one[64];
                            act_format_input(x->code[0], one, sizeof one);
                            say("[actions] conflict: %s and %s are both on %s%s%s", defs[i].name, defs[j].name, one,
                                v ? " for " : "", v ? weapon_names[v - 1] : ""), warnings++;
                        }
                }
            }
        }
    for (int i = 0; i < ACT_COUNT; i++)
        if (*defs[i].shared && (p->dev[0].base[i].set || p->dev[1].base[i].set) && defs[i].view == ACT_VIEW_MB1 &&
            !(i == ACT_DASH && act_split_in(p)))
            say("[actions] note: %s is the same game input as %s", defs[i].name, defs[i].shared), warnings++;
    return warnings;
}

void act_init(const char *ini_path) {
    static ActProfile p;
    if (!ini_path || !act_load(ini_path, &p)) act_parse("", &p);
    const char *e = getenv("MHGU_ACTIONS");
    if (e) p.enable = atoi(e) != 0;
    if ((e = getenv("MHGU_ACTIONS_MENUS"))) p.menus = atoi(e) != 0;
    if ((e = getenv("MHGU_ACTIONS_POINTER"))) p.pointer = atoi(e) != 0;
    if ((e = getenv("MHGU_ACTION_LOG"))) p.log = atoi(e) != 0;
    act_use_profile(&p);
    if (p.enable) {
        say("[actions] on (menus %s, pointer %s)", p.menus ? "on" : "off", p.pointer ? "on" : "off");
        act_validate(&p);
    }
}

// ---- sampling
static uint64_t inject_until[ACT_COUNT];
static uint64_t prev_hold[ACT_VARS];

void act_inject(int id, unsigned frames, uint64_t now) {
    if (id >= 0 && id < ACT_COUNT) inject_until[id] = now + (frames ? frames : 8);
}
uint64_t act_injected(uint64_t now) {
    uint64_t m = 0;
    for (int i = 0; i < ACT_COUNT; i++)
        if (inject_until[i] > now) m |= 1ull << i;
    return m;
}

bool act_uses_input(int code) {
    if (!prof_on) return false;
    bool found = false;
    lock();
    for (int d = 0; d < 2 && !found; d++)
        for (int v = 0; v < ACT_VARS && !found; v++) {
            if (v && !prof.dev[d].has_weapon[v - 1]) continue;
            const ActBinding *row = v ? prof.dev[d].weapon[v - 1] : prof.dev[d].base;
            for (int i = 0; i < ACT_COUNT && !found; i++)
                for (int a = 0; row[i].set && a < row[i].n; a++)
                    for (int k = 0; k < row[i].alt[a].n; k++)
                        if (row[i].alt[a].code[k] == code) found = true;
        }
    unlock();
    return found;
}

// The physical inputs the bindings of a context (for a weapon variant) use: Tier 0's default for any action of that
// context doesn't read them (a button given to Dodge no longer also attacks).
typedef struct { int n; int code[400]; } Claims;
static void claims_of(int view, int v, Claims *cl) {
    cl->n = 0;
    for (int i = 0; i < ACT_COUNT; i++) {
        if (defs[i].view != view) continue;
        for (int d = 0; d < 2; d++) {
            const ActBinding *b = binding_of(d, i, v);
            for (int a = 0; b && a < b->n; a++)
                for (int k = 0; k < b->alt[a].n; k++) {
                    int c = b->alt[a].code[k];
                    if (is_physical(c) && cl->n < 400) cl->code[cl->n++] = c;
                }
        }
    }
}
static bool claimed(const Claims *cl, int c) {
    for (int i = 0; i < cl->n; i++)
        if (cl->code[i] == c) return true;
    return false;
}

static bool sw_held(const ActHost *h, const Claims *cl, int dev, int btn) {  // dev bit 0 pad, bit 1 kbm
    btn = role_btn(btn, h->dash_zr);
    if (!(h->sw_word >> btn & 1)) return false;
    for (int d = 0; d < 2; d++) {
        if (!(dev >> d & 1) || !h->sw_sources) continue;
        int src[16], n = h->sw_sources(h->user, btn, d, src, 16);
        for (int i = 0; i < n; i++)
            if (!claimed(cl, src[i]) && h->held(h->user, src[i])) return true;
    }
    return false;
}
static bool input_held(const ActHost *h, const Claims *cl, int code) {
    if (code >= ACT_SW && code < ACT_SW + 0x60) {
        int dev = (code - ACT_SW) / 0x20, btn = (code - ACT_SW) % 0x20;
        if (dev != 1 && (h->sw_script >> role_btn(btn, h->dash_zr) & 1)) return true;
        return sw_held(h, cl, dev == 2 ? 3 : 1 << dev, btn);
    }
    return h->held(h->user, code);
}
static bool binding_held(const ActHost *h, const Claims *cl, const ActBinding *b) {
    for (int a = 0; a < b->n; a++) {
        bool all = b->alt[a].n > 0;
        for (int k = 0; k < b->alt[a].n && all; k++) all = input_held(h, cl, b->alt[a].code[k]);
        if (all) return true;
    }
    return false;
}

void act_sample(const ActHost *h, ActSnap *out) {
    lock();
    for (int v = 0; v < ACT_VARS; v++) {
        uint64_t hold = 0;
        bool own = v == 0 || prof.dev[0].has_weapon[v - 1] || prof.dev[1].has_weapon[v - 1];
        if (!own) {
            out->hold[v] = out->hold[0], out->pulse[v] = out->pulse[0];
            prev_hold[v] = prev_hold[0];
            continue;
        }
        if (!h->suppress && prof_on) {
            static Claims cl[ACT_VIEWS];  // (window thread only, under the lock)
            for (int w = 0; w < ACT_VIEWS; w++) claims_of(w, v, &cl[w]);
            for (int i = 0; i < ACT_COUNT; i++) {
                const Claims *c = &cl[defs[i].view];
                for (int d = 0; d < 2; d++) {
                    const ActBinding *b = binding_of(d, i, v);
                    if (b) {
                        if (binding_held(h, c, b)) hold |= 1ull << i;
                    } else if (defs[i].def_sw >= 0 && sw_held(h, c, 1 << d, defs[i].def_sw)) {
                        hold |= 1ull << i;
                    }
                }
                // a scripted press (MHGU_INPUT, the control file, quickhunt) is the game's own button, whatever is
                // bound: it presses the actions that deliver that button
                if (defs[i].def_sw >= 0 && (h->sw_script >> role_btn(defs[i].def_sw, h->dash_zr) & 1)) hold |= 1ull << i;
            }
            hold |= act_injected(h->frame);
        }
        out->hold[v] = hold;
        out->pulse[v] = hold & ~prev_hold[v];
        prev_hold[v] = hold;
    }
    unlock();
}

// ---- the view
// A hunt's A button: AttackA with the weapon out (ACT_VIEW_MB1), Interact with it away (ACT_VIEW_SHEATHED). While
// Interact has no binding of its own it follows AttackA, and the two are one button, as in the game. With the weapon
// away AttackA still presses A together with AttackX or Dash (the draw attacks of some weapons), or always with
// [Actions] SheathedAttackA=1.
static bool applies(int view, int i, uint64_t active) {
    if (view == ACT_VIEW_MB1) return i != ACT_INTERACT;
    if (view == ACT_VIEW_SHEATHED && i == ACT_ATTACK_A)
        return prof.sheathed_attack || (active & (1ull << ACT_ATTACK_X | 1ull << ACT_DASH)) != 0;
    return true;
}
static int base_view(int view) { return view == ACT_VIEW_SHEATHED || view == ACT_VIEW_HUNT_ANY ? ACT_VIEW_MB1 : view; }

uint32_t act_view_managed(int view, bool dash_zr) {
    uint32_t m = 0;
    for (int i = 0; i < ACT_COUNT; i++)
        if (defs[i].view == base_view(view)) m |= act_ew_bits(i, dash_zr);
    return m;
}

uint32_t act_view(int view, uint32_t ew, uint64_t active, bool dash_zr) {
    uint32_t managed = 0, on = 0;
    int base = base_view(view);
    for (int i = 0; i < ACT_COUNT; i++) {
        if (defs[i].view != base) continue;
        uint32_t bits = act_ew_bits(i, dash_zr);
        managed |= bits;
        if ((active >> i & 1) && applies(view, i, active)) on |= bits;
    }
    return (ew & ~managed) | on;
}
