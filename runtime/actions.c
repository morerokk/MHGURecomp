// Game-action rebinding: the registry, the [Actions*] ini, the sampling of bindings into the active set and the
// view the game's translators get. See actions.h.
#include "actions.h"

#include <ctype.h>
#include <stdarg.h>
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

typedef struct {
    const char *name;
    int view;
    int btn;          // the Switch button whose engine bit it delivers (-1: the dash role; see mask)
    uint32_t extra;   // more bits delivered with it (the X+A chord: both)
    int def_sw;       // the Switch button Tier 0 gives it by default (ACT_KCR: the dash role), -1 none
    const char *shared;
} ActDef;
static const ActDef defs[ACT_COUNT] = {
    [ACT_DODGE] = {"Dodge", ACT_VIEW_MB1, S_B, 0, S_B, "weapon B-actions (Lance, Switch Axe, Insect Glaive, bowguns, Prowler)"},
    [ACT_DASH] = {"Dash", ACT_VIEW_MB1, -1, 0, ACT_KCR, "Weapon R-action (guard, charge, aim): one game input"},
    [ACT_ATTACK_X] = {"AttackX", ACT_VIEW_MB1, S_X, 0, S_X, ""},
    [ACT_ATTACK_A] = {"AttackA", ACT_VIEW_MB1, S_A, 0, S_A, "interact, talk, open, gather: one game input"},
    [ACT_ITEM_USE] = {"ItemUse", ACT_VIEW_MB1, S_Y, 0, S_Y, ""},
    [ACT_CHORD_XA] = {"ChordXA", ACT_VIEW_MB1, S_X, 0x2000, -1, "AttackX and AttackA pressed together"},
    [ACT_MENU_CONFIRM] = {"MenuConfirm", ACT_VIEW_MB2, S_A, 0, S_A, ""},
    [ACT_MENU_CANCEL] = {"MenuCancel", ACT_VIEW_MB2, S_B, 0, S_B, ""},
    [ACT_MENU_UP] = {"MenuUp", ACT_VIEW_MB2, S_UP, 0, S_UP, ""},
    [ACT_MENU_DOWN] = {"MenuDown", ACT_VIEW_MB2, S_DOWN, 0, S_DOWN, ""},
    [ACT_MENU_LEFT] = {"MenuLeft", ACT_VIEW_MB2, S_LEFT, 0, S_LEFT, ""},
    [ACT_MENU_RIGHT] = {"MenuRight", ACT_VIEW_MB2, S_RIGHT, 0, S_RIGHT, ""},
    [ACT_MENU_TAB_L] = {"MenuTabL", ACT_VIEW_MB2, S_L, 0, S_L, ""},
    [ACT_MENU_TAB_R] = {"MenuTabR", ACT_VIEW_MB2, S_R, 0, S_R, ""},
    [ACT_MENU_X] = {"MenuX", ACT_VIEW_MB2, S_X, 0, S_X, ""},
    [ACT_MENU_Y] = {"MenuY", ACT_VIEW_MB2, S_Y, 0, S_Y, ""},
    [ACT_MENU_PAGE_L] = {"MenuPageL", ACT_VIEW_MB2, S_ZL, 0, S_ZL, ""},
    [ACT_MENU_PAGE_R] = {"MenuPageR", ACT_VIEW_MB2, S_ZR, 0, S_ZR, ""},
};
static const char *const weapon_names[ACT_WEAPONS] = {"GreatSword", "SnS", "Hammer", "Lance", "HBG", "LBG", "LongSword",
                                                      "SwitchAxe", "Gunlance", "Bow", "DualBlades", "Horn",
                                                      "InsectGlaive", "ChargeBlade", "Prowler"};

const char *act_name(int id) { return id >= 0 && id < ACT_COUNT ? defs[id].name : ""; }
int act_view_of(int id) { return defs[id].view; }
int act_default_switch(int id) { return defs[id].def_sw; }
const char *act_shared_with(int id) { return defs[id].shared; }
uint32_t act_ew_bits(int id, bool dash_zr) {
    const ActDef *d = &defs[id];
    return (d->btn < 0 ? ew_of_btn[dash_zr ? S_ZR : S_R] : ew_of_btn[d->btn]) | d->extra;
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
static const struct { const char *name; int code; } pad_names[] = {
    {"south", ACT_PAD + SDL_GAMEPAD_BUTTON_SOUTH}, {"east", ACT_PAD + SDL_GAMEPAD_BUTTON_EAST},
    {"west", ACT_PAD + SDL_GAMEPAD_BUTTON_WEST}, {"north", ACT_PAD + SDL_GAMEPAD_BUTTON_NORTH},
    {"lshoulder", ACT_PAD + SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}, {"rshoulder", ACT_PAD + SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
    {"ltrigger", ACT_PAD_LT}, {"rtrigger", ACT_PAD_RT}, {"lstick", ACT_PAD + SDL_GAMEPAD_BUTTON_LEFT_STICK},
    {"rstick", ACT_PAD + SDL_GAMEPAD_BUTTON_RIGHT_STICK}, {"dpup", ACT_PAD + SDL_GAMEPAD_BUTTON_DPAD_UP},
    {"dpdown", ACT_PAD + SDL_GAMEPAD_BUTTON_DPAD_DOWN}, {"dpleft", ACT_PAD + SDL_GAMEPAD_BUTTON_DPAD_LEFT},
    {"dpright", ACT_PAD + SDL_GAMEPAD_BUTTON_DPAD_RIGHT}, {"start", ACT_PAD + SDL_GAMEPAD_BUTTON_START},
    {"back", ACT_PAD + SDL_GAMEPAD_BUTTON_BACK},
};
static const struct { const char *name; int code; } mouse_names[] = {
    {"left", KEY_MOUSE + SDL_BUTTON_LEFT}, {"right", KEY_MOUSE + SDL_BUTTON_RIGHT},
    {"middle", KEY_MOUSE + SDL_BUTTON_MIDDLE}, {"x1", KEY_MOUSE + SDL_BUTTON_X1}, {"x2", KEY_MOUSE + SDL_BUTTON_X2},
    {"wheelup", KEY_WHEEL_UP}, {"wheeldown", KEY_WHEEL_DOWN},
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
    if (!strcasecmp(kind, "sw")) {  // sw:A, sw:Dash (the dash role); sw.pad:A / sw.kbm:A for one device
        if (same_name(val, "dash") || same_name(val, "kcr")) return *code = ACT_SW + 2 * 0x20 + ACT_KCR, true;
        for (int i = 0; i < 16; i++)
            if (same_name(val, btn_name[i])) return *code = ACT_SW + 2 * 0x20 + i, true;
        return false;
    }
    return false;
}

void act_format_input(int code, char *buf, size_t size) {
    if (code >= ACT_SW && code < ACT_SW + 0x60) {
        int dev = (code - ACT_SW) / 0x20, b = (code - ACT_SW) % 0x20;
        snprintf(buf, size, "sw%s:%s", dev == 0 ? ".pad" : dev == 1 ? ".kbm" : "", b == ACT_KCR ? "Dash" : btn_name[b & 15]);
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

bool act_load(const char *path, ActProfile *p) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (!buf) return fclose(f), false;
    n = (long)fread(buf, 1, (size_t)n, f);
    buf[n] = 0;
    fclose(f);
    act_parse(buf, p);
    free(buf);
    return true;
}

size_t act_format(const ActProfile *p, char *buf, size_t size) {
    size_t n = 0;
#define PUT(...) do { if (n < size) n += (size_t)snprintf(buf + n, size - n, __VA_ARGS__); } while (0)
    PUT("[Actions]\nEnable=%d\nManageMenus=%d\nPointer=%d\nSplitDashWeaponR=%d\nLog=%d\n", p->enable, p->menus, p->pointer, p->split, p->log);
    static const char *const sect[2] = {"Pad", "Kbm"};
    for (int d = 0; d < 2; d++) {
        for (int w = -1; w < ACT_WEAPONS; w++) {
            const ActBinding *row = w < 0 ? p->dev[d].base : p->dev[d].weapon[w];
            bool any = false;
            for (int i = 0; i < ACT_COUNT; i++) any |= row[i].set;
            if (!any) continue;
            if (w < 0) PUT("\n[Actions.%s]\n", sect[d]);
            else PUT("\n[Actions.%s.Weapon.%s]\n", sect[d], weapon_names[w]);
            for (int i = 0; i < ACT_COUNT; i++) {
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

// ---- the profile in use
static ActProfile prof;
static bool prof_on;
bool act_enabled(void) { return prof_on; }
const ActProfile *act_profile(void) { return &prof; }
void act_use_profile(const ActProfile *p) { prof = *p, prof_on = p->enable; }

// The binding an action has for a device and weapon variant (v 0: none): the weapon class's own if it has one, else
// the base's; an unset one is "the default" (a NULL).
static const ActBinding *binding_of(int dev, int id, int v) {
    if (v > 0 && prof.dev[dev].weapon[v - 1][id].set) return &prof.dev[dev].weapon[v - 1][id];
    return prof.dev[dev].base[id].set ? &prof.dev[dev].base[id] : NULL;
}

static bool is_physical(int c) { return c < ACT_SW || c >= ACT_SW + 0x60; }

// The Switch button a controller input is for Tier 0 whatever FaceButtons says (-1: a face button, or none)
static int pad_switch_button(int code) {
    switch (code) {
    case ACT_PAD + SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return S_L;
    case ACT_PAD + SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return S_R;
    case ACT_PAD_LT: return S_ZL;
    case ACT_PAD_RT: return S_ZR;
    case ACT_PAD + SDL_GAMEPAD_BUTTON_LEFT_STICK: return S_LSTICK;
    case ACT_PAD + SDL_GAMEPAD_BUTTON_RIGHT_STICK: return S_RSTICK;
    case ACT_PAD + SDL_GAMEPAD_BUTTON_START: return S_PLUS;
    case ACT_PAD + SDL_GAMEPAD_BUTTON_BACK: return S_MINUS;
    case ACT_PAD + SDL_GAMEPAD_BUTTON_DPAD_UP: return S_UP;
    case ACT_PAD + SDL_GAMEPAD_BUTTON_DPAD_DOWN: return S_DOWN;
    case ACT_PAD + SDL_GAMEPAD_BUTTON_DPAD_LEFT: return S_LEFT;
    case ACT_PAD + SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return S_RIGHT;
    }
    return -1;
}

int act_validate(const ActProfile *p) {
    int warnings = 0;
    ActProfile saved = prof;
    bool saved_on = prof_on;
    prof = *p;
    for (int d = 0; d < 2; d++)
        for (int v = 0; v < ACT_VARS; v++) {
            if (v > 0 && !p->dev[d].has_weapon[v - 1]) continue;
            for (int i = 0; i < ACT_COUNT; i++) {
                const ActBinding *bi = binding_of(d, i, v);
                if (!bi) continue;
                for (int a = 0; a < bi->n; a++)
                    for (int k = 0; k < bi->alt[a].n; k++) {
                        int c = bi->alt[a].code[k];
                        int sb = pad_switch_button(c);
                        if (sb >= 0 && !((act_view_managed(defs[i].view, false) | act_view_managed(defs[i].view, true)) & ew_of_btn[sb])) {
                            char one[64];
                            act_format_input(c, one, sizeof one);
                            say("[actions] %s is on %s, which is also the Switch %s: that button is not an action of this view yet, "
                                "so it still reaches the game", defs[i].name, one, btn_name[sb]), warnings++;
                        }
                        if (c < SDL_SCANCODE_COUNT && key_reserved(c)) {
                            say("[actions] %s uses %s, which the recomp keeps for itself (%s)", defs[i].name,
                                SDL_GetScancodeName((SDL_Scancode)c), key_reserved(c)), warnings++;
                        }
                    }
                for (int j = i + 1; j < ACT_COUNT; j++) {
                    const ActBinding *bj = binding_of(d, j, v);
                    if (!bj || defs[i].view != defs[j].view) continue;
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
        if (*defs[i].shared && (p->dev[0].base[i].set || p->dev[1].base[i].set) && defs[i].view == ACT_VIEW_MB1)
            say("[actions] note: %s is the same game input as %s", defs[i].name, defs[i].shared), warnings++;
    prof = saved, prof_on = saved_on;
    return warnings;
}

void act_init(const char *ini_path) {
    ActProfile p;
    if (ini_path && !act_load(ini_path, &p)) act_parse("", &p);
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
    for (int d = 0; d < 2; d++)
        for (int v = 0; v < ACT_VARS; v++)
            for (int i = 0; i < ACT_COUNT; i++) {
                const ActBinding *b = v ? (prof.dev[d].weapon[v - 1][i].set ? &prof.dev[d].weapon[v - 1][i] : NULL) : (prof.dev[d].base[i].set ? &prof.dev[d].base[i] : NULL);
                for (int a = 0; b && a < b->n; a++)
                    for (int k = 0; k < b->alt[a].n; k++)
                        if (b->alt[a].code[k] == code) return true;
            }
    return false;
}

// The physical inputs the explicit bindings of a view (for a weapon variant) use: Tier 0's default for any action of
// that view doesn't read them (a button given to Dodge no longer also attacks).
typedef struct { int n; int code[160]; } Claims;
static void claims_of(int view, int v, Claims *cl) {
    cl->n = 0;
    for (int i = 0; i < ACT_COUNT; i++) {
        if (defs[i].view != view) continue;
        for (int d = 0; d < 2; d++) {
            const ActBinding *b = binding_of(d, i, v);
            for (int a = 0; b && a < b->n; a++)
                for (int k = 0; k < b->alt[a].n; k++) {
                    int c = b->alt[a].code[k];
                    if (is_physical(c) && cl->n < 160) cl->code[cl->n++] = c;
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
    if (btn == ACT_KCR) btn = h->dash_zr ? S_ZR : S_R;
    if (!(h->sw_word >> btn & 1)) return false;
    if ((dev & 1) && (h->sw_script >> btn & 1)) return true;
    for (int d = 0; d < 2; d++) {
        if (!(dev >> d & 1) || !h->sw_sources) continue;
        int src[16], n = h->sw_sources(h->user, btn, d, src, 16);
        for (int i = 0; i < n; i++)
            if (!claimed(cl, src[i]) && h->held(h->user, src[i])) return true;
    }
    return false;
}
static bool input_held(const ActHost *h, const Claims *cl, int code) {
    if (code >= ACT_SW && code < ACT_SW + 0x60) return sw_held(h, cl, ((code - ACT_SW) / 0x20) == 2 ? 3 : 1 << ((code - ACT_SW) / 0x20), (code - ACT_SW) % 0x20);
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
    for (int v = 0; v < ACT_VARS; v++) {
        uint64_t hold = 0;
        bool own = v == 0 || prof.dev[0].has_weapon[v - 1] || prof.dev[1].has_weapon[v - 1];
        if (!own) {
            out->hold[v] = out->hold[0], out->pulse[v] = out->pulse[0];
            prev_hold[v] = prev_hold[0];
            continue;
        }
        if (!h->suppress && prof_on) {
            Claims cl[ACT_VIEWS];
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
            }
            hold |= act_injected(h->frame);
        }
        out->hold[v] = hold;
        out->pulse[v] = hold & ~prev_hold[v];
        prev_hold[v] = hold;
    }
}

// ---- the view
uint32_t act_view_managed(int view, bool dash_zr) {
    uint32_t m = 0;
    for (int i = 0; i < ACT_COUNT; i++)
        if (defs[i].view == view) m |= act_ew_bits(i, dash_zr);
    return m;
}

uint32_t act_view(int view, uint32_t ew, uint64_t active, bool dash_zr) {
    uint32_t managed = 0, on = 0;
    for (int i = 0; i < ACT_COUNT; i++) {
        if (defs[i].view != view) continue;
        uint32_t bits = act_ew_bits(i, dash_zr);
        managed |= bits;
        if (active >> i & 1) on |= bits;
    }
    return (ew & ~managed) | on;
}
