// Host-only test of game-action rebinding (runtime/actions.c): the ini, the sampling and, above all, the golden
// property of the default profile: every engine word comes out of act_view exactly as it went in (so [Actions]
// Enable=1 with no bindings plays like Enable=0). No game files, no window, no generated code.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "../runtime/actions.h"
#include "../runtime/config.h"

volatile uint64_t g_present_count;  // (config.c reads it)
#define FIELD ((1ull << ACT_MENU_CONFIRM) - 1)  // the actions of the gameplay view
static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// Switch button -> engine word bit (INPUTS-MASTER B.1), written out again here on purpose
static const uint32_t ew_of[16] = {0x2000, 0x4000, 0x1000, 0x8000, 0x2, 0x4, 0x100, 0x200, 0x400, 0x800, 0x8, 0x1, 0x80, 0x10, 0x20, 0x40};

// ---- a fake host: a Switch word b, each button with one controller source (ACT_PAD + 20 + btn) and one keyboard source
// (scancode 40 + btn); physical state is a set of held codes
static int held_codes[64], nheld;
static bool fake_held(void *u, int code) { (void)u; for (int i = 0; i < nheld; i++) if (held_codes[i] == code) return true; return false; }
static int fake_sources(void *u, int btn, int dev, int *out, int max) {
    (void)u; (void)max;
    out[0] = dev == 0 ? ACT_PAD + 20 + btn : 40 + btn;
    return 1;
}
static void press(int code) { held_codes[nheld++] = code; }
static void release_all(void) { nheld = 0; }
// Tier 0: the word for the held fake sources
static uint64_t tier0_word(void) {
    uint64_t b = 0;
    for (int btn = 0; btn < 16; btn++)
        if (fake_held(NULL, ACT_PAD + 20 + btn) || fake_held(NULL, 40 + btn)) b |= 1ull << btn;
    return b;
}
static uint32_t ew_of_word(uint64_t b) {
    uint32_t w = 0;
    for (int i = 0; i < 16; i++) if (b >> i & 1) w |= ew_of[i];
    return w;
}
static ActHost host_for(uint64_t b, bool zr, uint64_t frame) {
    ActHost h = {NULL, fake_held, fake_sources, b, 0, false, zr, frame};
    return h;
}

static void quiet(const char *line) { (void)line; }
static char logbuf[4096];
static void collect(const char *line) { strncat(logbuf, line, sizeof logbuf - strlen(logbuf) - 2); strcat(logbuf, "\n"); }

static ActProfile parse(const char *text) { ActProfile p; act_parse(text, &p); return p; }

static void test_registry(void) {
    CHECK(act_find("dodge") == ACT_DODGE && act_find("Attack_X") == ACT_ATTACK_X && act_find("menu confirm") == ACT_MENU_CONFIRM, "act_find");
    CHECK(act_find("nonsense") < 0, "act_find unknown");
    CHECK(act_ew_bits(ACT_DASH, false) == 0x200 && act_ew_bits(ACT_DASH, true) == 0x800, "dash role bits");
    CHECK(act_ew_bits(ACT_CHORD_XA, false) == 0x3000, "chord bits");
    CHECK(act_weapon_find("hammer") == 2 && act_weapon_find("SnS") == 1, "weapon names");
    for (int i = 0; i < ACT_COUNT; i++) CHECK(*act_name(i) && act_find(act_name(i)) == i, "name round trip %d", i);
}

static void test_ini(void) {
    act_set_log(collect);
    logbuf[0] = 0;
    ActProfile p = parse("[Actions]\nEnable=1\nManageMenus = true ; menus too\nPointer=0\nLog=1\n"
                         "[Actions.Pad]\nDodge = pad:north\nItemUse = pad:east | pad:lshoulder+pad:rtrigger\n"
                         "AttackA = None\nBogus = pad:south\nDash = pad:nosuch\n"
                         "[Actions.Kbm]\nDodge = key:space\nAttackX = mouse:left\nMenuUp = key:up | mouse:wheelup\nMenuDown = mouse:wheeldown\n"
                         "[Actions.Pad.Weapon.Hammer]\nAttackA = pad:north\n"
                         "[Actions.Pad.Weapon.Nope]\nAttackA = pad:north\n"
                         "[Display]\nResolution=1280x720\n");
    CHECK(p.enable && p.menus && !p.pointer && p.log, "main keys");
    CHECK(p.dev[0].base[ACT_DODGE].set && p.dev[0].base[ACT_DODGE].n == 1 && p.dev[0].base[ACT_DODGE].alt[0].code[0] == ACT_PAD + SDL_GAMEPAD_BUTTON_NORTH, "pad dodge");
    CHECK(p.dev[0].base[ACT_ITEM_USE].n == 2 && p.dev[0].base[ACT_ITEM_USE].alt[1].n == 2, "alternatives and chord");
    CHECK(p.dev[0].base[ACT_ATTACK_A].set && p.dev[0].base[ACT_ATTACK_A].n == 0, "None is bound to nothing, on purpose");
    CHECK(!p.dev[0].base[ACT_DASH].set, "a binding with nothing understood leaves the default");
    CHECK(p.dev[1].base[ACT_DODGE].alt[0].code[0] == SDL_SCANCODE_SPACE, "key:space");
    CHECK(p.dev[1].base[ACT_ATTACK_X].alt[0].code[0] == KEY_MOUSE + SDL_BUTTON_LEFT, "mouse:left");
    CHECK(p.dev[1].base[ACT_MENU_UP].n == 2 && p.dev[1].base[ACT_MENU_UP].alt[1].code[0] == KEY_WHEEL_UP, "wheel");
    CHECK(p.dev[0].has_weapon[2] && p.dev[0].weapon[2][ACT_ATTACK_A].set, "weapon override");
    CHECK(strstr(logbuf, "unknown action \"Bogus\"") && strstr(logbuf, "unknown weapon class \"Nope\""), "unknown names are reported: %s", logbuf);
    // format and parse again
    char text[4096];
    act_format(&p, text, sizeof text);
    ActProfile q = parse(text);
    char text2[4096];
    act_format(&q, text2, sizeof text2);
    CHECK(!strcmp(text, text2), "format is stable:\n%s\n---\n%s", text, text2);
    CHECK(q.enable && q.menus && q.dev[1].base[ACT_MENU_DOWN].alt[0].code[0] == KEY_WHEEL_DOWN && q.dev[0].weapon[2][ACT_ATTACK_A].set, "round trip");

    // back-compat: no section at all, or Enable=0, hooks nothing; the legacy [Keybinds] and friends are not ours
    ActProfile none = parse("[Display]\nResolution=1280x720\n[Keybinds]\nA = Mouse Left\n");
    CHECK(!none.enable && !none.menus, "no [Actions]: off");
    ActProfile off = parse("[Actions]\nEnable=0\n[Actions.Pad]\nDodge=pad:north\n");
    CHECK(!off.enable, "Enable=0");
    act_set_log(quiet);
}

static void test_validate(void) {
    act_set_log(collect);
    logbuf[0] = 0;
    ActProfile p = parse("[Actions]\nEnable=1\n[Actions.Kbm]\nDodge = key:space\nAttackA = key:space\nItemUse = key:escape\n"
                         "MenuConfirm = key:space\n[Actions.Pad]\nChordXA = pad:lshoulder\nDash = pad:rshoulder\n");
    int n = act_validate(&p);
    CHECK(strstr(logbuf, "conflict: Dodge and AttackA"), "conflict in one view: %s", logbuf);
    CHECK(!strstr(logbuf, "MenuConfirm and Dodge"), "no conflict across views");
    CHECK(strstr(logbuf, "ItemUse uses Escape"), "reserved key warned: %s", logbuf);
    CHECK(strstr(logbuf, "Dodge is the same game input as"), "shared-input note");
    CHECK(!strstr(logbuf, "Dodge and Interact"), "an action that follows another is not a second conflict: %s", logbuf);
    CHECK(!strstr(logbuf, "AttackA and Interact"), "A with the weapon out and away are not read together");
    CHECK(!strstr(logbuf, "TownInteract and TownCancel"), "the town's actions follow nothing: no conflict that was not asked for: %s", logbuf);
    CHECK(n >= 4, "count %d", n);
    act_set_log(quiet);
}

// ---- the golden property
static void test_golden(void) {
    ActProfile p = parse("[Actions]\nEnable=1\nManageMenus=1\n");
    act_use_profile(&p);
    uint32_t noise[] = {0, 0x10000, 0x80000, 0x1000000, 0xFF0000, 0x40000000, 0x02000000};
    for (int zr = 0; zr < 2; zr++) {
        for (uint32_t b = 0; b < 65536; b++) {
            uint32_t ew = ew_of_word(b);
            release_all();
            for (int i = 0; i < 16; i++) if (b >> i & 1) press((i & 1) ? ACT_PAD + 20 + i : 40 + i);  // some by pad, some by key
            ActHost h = host_for(tier0_word(), zr, 1000 + b);
            ActSnap s;
            memset(&s, 0, sizeof s);
            act_sample(&h, &s);
            for (unsigned k = 0; k < sizeof noise / sizeof *noise; k++) {
                uint32_t in = ew | noise[k];
                uint64_t act = s.hold[0] | s.pulse[0];
                CHECK(act_view(ACT_VIEW_MB1, in, act, zr) == in, "T0 mb1 b=%04x zr=%d noise=%x", b, zr, noise[k]);
                CHECK(act_view(ACT_VIEW_MB2, in, act, zr) == in, "T0 mb2 b=%04x zr=%d noise=%x", b, zr, noise[k]);
                CHECK(act_view(ACT_VIEW_TOWN, in, act, zr) == in, "T0 town b=%04x zr=%d noise=%x", b, zr, noise[k]);
                CHECK(act_view(ACT_VIEW_SHEATHED, in, act, zr) == in && act_view(ACT_VIEW_HUNT_ANY, in, act, zr) == in, "T0 hunt states b=%04x zr=%d noise=%x", b, zr, noise[k]);
                CHECK(act_view(ACT_VIEW_MB1 | ACT_VIEW_BAR, in, act, zr) == in && act_view(ACT_VIEW_SHEATHED | ACT_VIEW_BAR, in, act, zr) == in, "T0 item bar b=%04x zr=%d noise=%x", b, zr, noise[k]);
                if (failures > 20) return;
            }
        }
    }
    // pulses: a press that is over before the game looks is still delivered for a frame, and does not stick
    release_all();
    ActSnap s;
    ActHost h = host_for(0, false, 50);
    act_sample(&h, &s);
    press(ACT_PAD + 20 + 1);
    h = host_for(tier0_word(), false, 51);
    act_sample(&h, &s);
    CHECK(s.pulse[0] >> ACT_DODGE & 1 && s.hold[0] >> ACT_DODGE & 1, "dodge pulse+hold");
    release_all();
    h = host_for(0, false, 52);
    act_sample(&h, &s);
    CHECK(!(s.hold[0] | s.pulse[0]), "released");
}

static int map_sources(void *u, int btn, int dev, int *out, int max) {
    (void)u; (void)max;
    if (dev == 0) {
        if (btn == 2) return out[0] = ACT_PAD + SDL_GAMEPAD_BUTTON_NORTH, 1;  // X: north (Switch positions)
        if (btn == 0) return out[0] = ACT_PAD + SDL_GAMEPAD_BUTTON_EAST, 1;   // A: east
        if (btn == 1) return out[0] = ACT_PAD + SDL_GAMEPAD_BUTTON_SOUTH, 1;  // B: south
        if (btn == 3) return out[0] = ACT_PAD + SDL_GAMEPAD_BUTTON_WEST, 1;   // Y: west
        return out[0] = ACT_PAD + 30 + btn, 1;
    }
    return out[0] = 40 + btn, 1;
}
static uint64_t map_word(void) {
    uint64_t b = 0;
    for (int btn = 0; btn < 16; btn++) {
        int s[4];
        for (int d = 0; d < 2; d++) { map_sources(NULL, btn, d, s, 4); if (fake_held(NULL, s[0])) b |= 1ull << btn; }
    }
    return b;
}
static void test_rebind2(void) {
    ActProfile p = parse("[Actions]\nEnable=1\n[Actions.Pad]\nDodge = pad:north\nItemUse = pad:east\n");
    act_use_profile(&p);
    int N = ACT_PAD + SDL_GAMEPAD_BUTTON_NORTH, E = ACT_PAD + SDL_GAMEPAD_BUTTON_EAST, S = ACT_PAD + SDL_GAMEPAD_BUTTON_SOUTH;
    ActSnap s;
    ActHost h;
    release_all();
    press(N);
    h = (ActHost){NULL, fake_held, map_sources, map_word(), 0, false, false, 100};
    act_sample(&h, &s);
    CHECK((s.hold[0] & FIELD) == 1ull << ACT_DODGE, "north dodges and no longer attacks with X: %llx", (unsigned long long)s.hold[0]);
    uint32_t out = act_view(ACT_VIEW_MB1, ew_of_word(map_word()), s.hold[0], false);
    CHECK(out == 0x4000, "the game sees B only: %x", out);
    release_all();
    press(E);
    h.sw_word = map_word();
    act_sample(&h, &s);
    CHECK((s.hold[0] & FIELD) == 1ull << ACT_ITEM_USE, "east uses the item and no longer attacks with A: %llx", (unsigned long long)s.hold[0]);
    out = act_view(ACT_VIEW_MB1, ew_of_word(map_word()), s.hold[0], false);
    CHECK(out == 0x8000, "the game sees Y only: %x", out);
    release_all();
    press(S);  // south was Tier 0's B: still dodges? it is now free (Dodge moved to north): nothing
    h.sw_word = map_word();
    act_sample(&h, &s);
    CHECK((s.hold[0] & FIELD) == 0 || (s.hold[0] & FIELD) == 1ull << ACT_DODGE, "south: %llx", (unsigned long long)s.hold[0]);
    CHECK(act_view(ACT_VIEW_MB1, ew_of_word(map_word()), s.hold[0], false) == ((s.hold[0] & FIELD) ? 0x4000u : 0u), "south view");
    // the keyboard side of Dodge is still Tier 0's default (only the controller was rebound)
    release_all();
    press(40 + 1);
    h.sw_word = map_word();
    act_sample(&h, &s);
    CHECK((s.hold[0] & FIELD) == 1ull << ACT_DODGE, "kbm B still dodges: %llx", (unsigned long long)s.hold[0]);
    // suppression (Forge's menu, a text entry)
    h.suppress = true;
    act_sample(&h, &s);
    CHECK(s.hold[0] == 0, "suppressed");
    h.suppress = false;
    // injected actions (the control file's "act")
    release_all();
    h.sw_word = 0;
    act_inject(ACT_DASH, 4, 200);
    h.frame = 201;
    act_sample(&h, &s);
    CHECK(s.hold[0] == 1ull << ACT_DASH, "act Dash");
    h.frame = 204;
    act_sample(&h, &s);
    CHECK(s.hold[0] == 0, "act Dash is over");
    // the chord: X and A in one frame, plus the dash role
    uint32_t w = act_view(ACT_VIEW_MB1, 0, 1ull << ACT_CHORD_XA, false);
    CHECK(w == 0x3000, "chord X+A %x", w);
    w = act_view(ACT_VIEW_MB1, 0, 1ull << ACT_DASH, true);
    CHECK(w == 0x800, "dash on ZR %x", w);
    w = act_view(ACT_VIEW_MB1, 0x200, 0, true);
    CHECK(w == 0, "with ZR the dash role, R is FieldZR's: not held %x", w);
    w = act_view(ACT_VIEW_MB1, 0, 1ull << ACT_FIELD_ZR, true);
    CHECK(w == 0x200, "FieldZR is the other one of R / ZR %x", w);
    w = act_view(ACT_VIEW_MB1, 0, 1ull << ACT_FIELD_ZR, false);
    CHECK(w == 0x800, "FieldZR %x", w);
}

static void test_weapon(void) {
    ActProfile p = parse("[Actions]\nEnable=1\n[Actions.Pad]\nAttackA = pad:east\n[Actions.Pad.Weapon.Hammer]\nAttackA = pad:north\n");
    act_use_profile(&p);
    int N = ACT_PAD + SDL_GAMEPAD_BUTTON_NORTH, E = ACT_PAD + SDL_GAMEPAD_BUTTON_EAST;
    ActSnap s;
    ActHost h = (ActHost){NULL, fake_held, map_sources, 0, 0, false, false, 300};
    release_all();
    press(E);
    h.sw_word = map_word();
    act_sample(&h, &s);
    CHECK(s.hold[0] >> ACT_ATTACK_A & 1 && !(s.hold[3] >> ACT_ATTACK_A & 1), "east attacks, but not for the Hammer (variant 3): %llx %llx", (unsigned long long)s.hold[0], (unsigned long long)s.hold[3]);
    CHECK(s.hold[1] >> ACT_ATTACK_A & 1, "a class without an override has the base");
    release_all();
    press(N);
    h.sw_word = map_word();
    act_sample(&h, &s);
    CHECK(s.hold[3] >> ACT_ATTACK_A & 1, "north attacks with the Hammer");
}

static void test_chord_alt(void) {
    ActProfile p = parse("[Actions]\nEnable=1\n[Actions.Kbm]\nDodge = key:space | key:leftctrl+key:e\nMenuUp = mouse:wheelup\n");
    act_use_profile(&p);
    ActSnap s;
    ActHost h = (ActHost){NULL, fake_held, map_sources, 0, 0, false, false, 400};
    release_all();
    press(SDL_SCANCODE_E);
    act_sample(&h, &s);
    CHECK(!(s.hold[0] >> ACT_DODGE & 1), "half a chord");
    press(SDL_SCANCODE_LCTRL);
    act_sample(&h, &s);
    CHECK(s.hold[0] >> ACT_DODGE & 1, "the chord");
    release_all();
    press(KEY_WHEEL_UP);
    act_sample(&h, &s);
    CHECK(s.hold[0] >> ACT_MENU_UP & 1, "wheel pulse");
    CHECK(act_uses_input(KEY_WHEEL_UP) && !act_uses_input(KEY_WHEEL_DOWN), "act_uses_input");
}

// The town and the menus are independent: rebinding one never moves the other, or the hunt (the Simple layout's merged
// rows write both, see test_rows); with nothing of its own an action reads the button the game has
static void test_town(void) {
    CHECK(act_find("Confirm") == ACT_MENU_CONFIRM && act_find("cancel") == ACT_MENU_CANCEL, "Confirm / Cancel names");
    ActProfile p = parse("[Actions]\nEnable=1\nManageMenus=1\n[Actions.Pad]\nConfirm = pad:south\nCancel = pad:east\n");
    CHECK(p.dev[0].base[ACT_MENU_CONFIRM].set && p.dev[0].base[ACT_MENU_CANCEL].set, "aliases parse");
    act_use_profile(&p);
    int E = ACT_PAD + SDL_GAMEPAD_BUTTON_EAST, S = ACT_PAD + SDL_GAMEPAD_BUTTON_SOUTH, N = ACT_PAD + SDL_GAMEPAD_BUTTON_NORTH;
    ActSnap s;
    ActHost h;
    release_all();
    press(S);  // Switch B's position: Confirm now, in the menus
    h = (ActHost){NULL, fake_held, map_sources, map_word(), 0, false, false, 300};
    act_sample(&h, &s);
    uint32_t in = ew_of_word(map_word());  // the engine word Tier 0 gives: B (0x4000)
    CHECK(act_view(ACT_VIEW_MB2, in, s.hold[0], false) == 0x2000, "menu: south confirms: %x", act_view(ACT_VIEW_MB2, in, s.hold[0], false));
    CHECK(act_view(ACT_VIEW_TOWN, in, s.hold[0], false) == 0x4000, "town: south is still B (nothing was merged): %x", act_view(ACT_VIEW_TOWN, in, s.hold[0], false));
    CHECK(act_view(ACT_VIEW_MB1, in, s.hold[0], false) == 0x4000, "hunt: south is still B (dodge)");
    CHECK(act_view(ACT_VIEW_SHEATHED, in, s.hold[0], false) == 0x4000, "hunt, weapon away: south is still B");
    release_all();
    press(E);  // the old A position
    h.sw_word = map_word();
    act_sample(&h, &s);
    in = ew_of_word(map_word());  // 0x2000
    CHECK(act_view(ACT_VIEW_TOWN, in, s.hold[0], false) == 0x2000, "town: east is still A: %x", act_view(ACT_VIEW_TOWN, in, s.hold[0], false));
    CHECK(act_view(ACT_VIEW_MB2, in, s.hold[0], false) == 0x4000, "menu: east cancels");
    CHECK(act_view(ACT_VIEW_MB1, in, s.hold[0], false) == 0x2000, "hunt: east still attacks (A)");
    CHECK(act_view(ACT_VIEW_SHEATHED, in, s.hold[0], false) == 0x2000, "hunt, weapon away: east still interacts (A)");
    int from = -1;
    CHECK(!act_effective(&p, 0, ACT_TOWN_INTERACT, 0, &from), "TownInteract follows nothing");
    // the town's own bindings
    p = parse("[Actions]\nEnable=1\nManageMenus=1\n[Actions.Pad]\nConfirm = pad:south\nCancel = pad:east\nTownInteract = pad:south\nTownCancel = pad:east\n");
    act_use_profile(&p);
    release_all();
    press(S);
    h.sw_word = map_word();
    act_sample(&h, &s);
    in = ew_of_word(map_word());
    CHECK(act_view(ACT_VIEW_TOWN, in, s.hold[0], false) == 0x2000, "town: south talks once TownInteract is bound: %x", act_view(ACT_VIEW_TOWN, in, s.hold[0], false));
    release_all();
    press(E);
    h.sw_word = map_word();
    act_sample(&h, &s);
    in = ew_of_word(map_word());
    CHECK(act_view(ACT_VIEW_TOWN, in, s.hold[0], false) == 0x4000, "town: east is B once TownCancel is bound: %x", act_view(ACT_VIEW_TOWN, in, s.hold[0], false));
    // the hunt's actions do not move the town's: a moved Dodge and AttackX
    p = parse("[Actions]\nEnable=1\n[Actions.Pad]\nDodge = pad:north\nAttackX = pad:south\n");
    act_use_profile(&p);
    release_all();
    press(N);
    h.sw_word = map_word();
    act_sample(&h, &s);
    CHECK(act_view(ACT_VIEW_TOWN, 0, s.hold[0], false) == 0x1000, "town: north is still X there (the hunt's Dodge moved, not the town's): %x", act_view(ACT_VIEW_TOWN, 0, s.hold[0], false));
    CHECK(act_view(ACT_VIEW_MB1, 0, s.hold[0], false) == 0x4000, "dodge presses B in the hunt");
    release_all();
    press(S);
    h.sw_word = map_word();
    act_sample(&h, &s);
    CHECK(act_view(ACT_VIEW_TOWN, 0, s.hold[0], false) == 0x4000, "town: south is still B there: %x", act_view(ACT_VIEW_TOWN, 0, s.hold[0], false));
    // the menus' Plus does not follow the hunt's Pause
    p = parse("[Actions]\nEnable=1\nManageMenus=1\n[Actions.Pad]\nPause = pad:south\n");
    CHECK(!act_effective(&p, 0, ACT_MENU_PLUS, 0, NULL) && !act_effective(&p, 0, ACT_TOWN_PLUS, 0, NULL), "menu and town Plus follow nothing");
}

// A hunt's A button: AttackA with the weapon out, Interact with it away; one button until Interact is bound
static void test_interact(void) {
    int E = ACT_PAD + SDL_GAMEPAD_BUTTON_EAST, S = ACT_PAD + SDL_GAMEPAD_BUTTON_SOUTH, N = ACT_PAD + SDL_GAMEPAD_BUTTON_NORTH,
        R = ACT_PAD + SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
    ActSnap s;
    ActHost h = (ActHost){NULL, fake_held, map_sources, 0, 0, false, false, 500};
    // AttackA alone moves both, as in the game
    ActProfile p = parse("[Actions]\nEnable=1\n[Actions.Pad]\nAttackA = pad:rshoulder\n");
    act_use_profile(&p);
    release_all();
    press(R);
    act_sample(&h, &s);
    CHECK(act_view(ACT_VIEW_MB1, 0, s.hold[0], false) == 0x2000, "weapon out: rshoulder is A");
    CHECK(act_view(ACT_VIEW_SHEATHED, 0, s.hold[0], false) == 0x2000, "weapon away: Interact follows AttackA: %x", act_view(ACT_VIEW_SHEATHED, 0, s.hold[0], false));
    CHECK(act_view(ACT_VIEW_HUNT_ANY, 0, s.hold[0], false) == 0x2000, "no state: A");
    // split
    p = parse("[Actions]\nEnable=1\n[Actions.Pad]\nAttackA = pad:rshoulder\nInteract = pad:south\nDodge = pad:east\nAttackX = pad:north\n");
    act_use_profile(&p);
    release_all();
    press(R);
    act_sample(&h, &s);
    CHECK(act_view(ACT_VIEW_MB1, 0, s.hold[0], false) == 0x2000, "weapon out: AttackA is A");
    CHECK(act_view(ACT_VIEW_SHEATHED, 0, s.hold[0], false) == 0, "weapon away: AttackA alone presses nothing: %x", act_view(ACT_VIEW_SHEATHED, 0, s.hold[0], false));
    CHECK(act_view(ACT_VIEW_HUNT_ANY, 0, s.hold[0], false) == 0x2000, "Prowler / unknown: AttackA is A");
    press(N);  // with AttackX: the draw attacks that need both
    act_sample(&h, &s);
    CHECK(act_view(ACT_VIEW_SHEATHED, 0, s.hold[0], false) == 0x3000, "weapon away: AttackX + AttackA is X+A: %x", act_view(ACT_VIEW_SHEATHED, 0, s.hold[0], false));
    release_all();
    press(S);
    act_sample(&h, &s);
    CHECK(act_view(ACT_VIEW_MB1, 0, s.hold[0], false) == 0, "weapon out: Interact presses nothing: %x", act_view(ACT_VIEW_MB1, 0, s.hold[0], false));
    CHECK(act_view(ACT_VIEW_SHEATHED, 0, s.hold[0], false) == 0x2000, "weapon away: Interact is A");
    CHECK(act_view(ACT_VIEW_HUNT_ANY, 0, s.hold[0], false) == 0x2000, "Prowler / unknown: Interact is A");
    CHECK(act_view(ACT_VIEW_TOWN, 0, s.hold[0], false) == 0, "town: the hunt's Interact is not the town's: nothing there");
    release_all();
    press(E);
    act_sample(&h, &s);
    CHECK(act_view(ACT_VIEW_SHEATHED, 0, s.hold[0], false) == 0x4000 && act_view(ACT_VIEW_MB1, 0, s.hold[0], false) == 0x4000, "east dodges in both");
    // SheathedAttackA=1: AttackA is A in both states
    p = parse("[Actions]\nEnable=1\nSheathedAttackA=1\n[Actions.Pad]\nAttackA = pad:rshoulder\nInteract = pad:south\n");
    act_use_profile(&p);
    release_all();
    press(R);
    act_sample(&h, &s);
    CHECK(act_view(ACT_VIEW_SHEATHED, 0, s.hold[0], false) == 0x2000, "SheathedAttackA");
    // no conflict warning for the pair on one input, a conflict with the option on
    act_set_log(collect);
    logbuf[0] = 0;
    p = parse("[Actions]\nEnable=1\n[Actions.Pad]\nAttackA = pad:east\nInteract = pad:east\n");
    act_validate(&p);
    CHECK(!strstr(logbuf, "conflict"), "AttackA and Interact on one input: %s", logbuf);
    act_set_log(quiet);
}

// Every button of the pad is an action in each context: each action alone delivers exactly its button
static void test_all_buttons(void) {
    ActProfile p = parse("[Actions]\nEnable=1\nManageMenus=1\n");
    act_use_profile(&p);
    for (int view = 0; view < ACT_VIEWS; view++) {
        CHECK(act_view_managed(view, false) == 0xFFFF && act_view_managed(view, true) == 0xFFFF, "%s owns the 16 buttons: %x", act_view_name(view), act_view_managed(view, false));
        int ids[ACT_COUNT], n = act_order(view, ids, ACT_COUNT);
        uint32_t all = 0;
        for (int k = 0; k < n; k++) {
            CHECK(act_view_of(ids[k]) == view && *act_label(ids[k]), "order and label of %s", act_name(ids[k]));
            for (int j = 0; j < k; j++) CHECK(ids[j] != ids[k], "%s listed twice", act_name(ids[k]));
            int v = view == ACT_VIEW_MB1 ? ACT_VIEW_HUNT_ANY : view;
            bool bar_action = ids[k] == ACT_ITEMBAR_RIGHT || ids[k] == ACT_ITEMBAR_LEFT;  // (only while the bar is open)
            uint32_t w = act_view(v, 0xFFFF | 0x10000, 1ull << ids[k], false);
            CHECK(w == ((bar_action ? 0u : act_ew_bits(ids[k], false)) | 0x10000), "%s alone: %x", act_name(ids[k]), w);
            if (bar_action) {
                w = act_view(v | ACT_VIEW_BAR, 0x10000, 1ull << ids[k], false);
                CHECK(w == (act_ew_bits(ids[k], false) | 0x10000), "%s alone, bar open: %x", act_name(ids[k]), w);
            }
            all |= bar_action ? act_ew_bits(ids[k], false) : w;
        }
        int count = 0;
        for (int i = 0; i < ACT_COUNT; i++) count += act_view_of(i) == view;
        CHECK(n == count, "%s: every action is listed (%d of %d)", act_view_name(view), n, count);
        CHECK((all & 0xFFFF) == 0xFFFF, "%s reaches every button: %x", act_view_name(view), all);
    }
    // every action can be bound and comes back from the ini; a binding of one device leaves the other's default
    char text[16384] = "[Actions]\nEnable=1\nManageMenus=1\n[Actions.Kbm]\n";
    for (int i = 0; i < ACT_COUNT; i++) snprintf(text + strlen(text), sizeof text - strlen(text), "%s = key:%c\n", act_name(i), 'a' + i % 26);
    p = parse(text);
    for (int i = 0; i < ACT_COUNT; i++)
        CHECK(p.dev[1].base[i].set && p.dev[1].base[i].alt[0].code[0] == SDL_SCANCODE_A + i % 26 && !p.dev[0].base[i].set, "%s parsed", act_name(i));
    static char out[65536], out2[65536];
    act_format(&p, out, sizeof out);
    ActProfile q = parse(out);
    act_format(&q, out2, sizeof out2);
    CHECK(!strcmp(out, out2) && !memcmp(&p, &q, sizeof p), "all actions round trip");
}

// Scripted presses (MHGU_INPUT, the control file, quickhunt) are the game's buttons whatever is bound
static void test_script(void) {
    ActProfile p = parse("[Actions]\nEnable=1\nManageMenus=1\n[Actions.Pad]\nConfirm = pad:south\nCancel = pad:east\nDodge = pad:north\n");
    act_use_profile(&p);
    release_all();
    ActSnap s;
    ActHost h = (ActHost){NULL, fake_held, map_sources, 1ull << 0, 1ull << 0, false, false, 600};  // a scripted A
    act_sample(&h, &s);
    CHECK(act_view(ACT_VIEW_MB2, 0x2000, s.hold[0], false) == 0x2000, "scripted A confirms in the menus with Confirm rebound: %x", act_view(ACT_VIEW_MB2, 0x2000, s.hold[0], false));
    CHECK(act_view(ACT_VIEW_TOWN, 0x2000, s.hold[0], false) == 0x2000 && act_view(ACT_VIEW_MB1, 0x2000, s.hold[0], false) == 0x2000, "and is A in the town and the hunt");
    h.sw_word = h.sw_script = 1ull << 1;  // a scripted B
    act_sample(&h, &s);
    CHECK(act_view(ACT_VIEW_MB1, 0x4000, s.hold[0], false) == 0x4000 && act_view(ACT_VIEW_MB2, 0x4000, s.hold[0], false) == 0x4000, "scripted B");
}

// act_save: the [Actions*] sections of an ini are replaced, the rest of the file is kept
static void test_save(void) {
    const char *path = "obj/settings-test/actions-save-test.ini";
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL, "temp ini");
    if (!f) return;
    fputs("; a comment\n[Display]\nResolution = 1280x720\n\n[Actions]\nEnable=1\n[Actions.Pad]\nDodge = pad:north\n\n[Keybinds]\nA = Mouse Left\n", f);
    fclose(f);
    ActProfile p = parse("[Actions]\nEnable=1\nManageMenus=1\n[Actions.Pad]\nInteract = pad:south\n[Actions.Kbm.Weapon.Bow]\nAttackA = mouse:right\n");
    CHECK(act_save(path, &p), "act_save");
    static ActProfile q;
    CHECK(act_load(path, &q) && !memcmp(&p, &q, sizeof p), "saved profile reads back the same");
    char buf[4096] = "";
    f = fopen(path, "rb");
    if (f) { size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f); }
    CHECK(strstr(buf, "; a comment\n[Display]\nResolution = 1280x720\n") && strstr(buf, "[Keybinds]\nA = Mouse Left\n"), "other sections kept: %s", buf);
    CHECK(!strstr(buf, "Dodge") && strstr(buf, "Interact = pad:south") && strstr(buf, "[Actions.Kbm.Weapon.Bow]"), "actions replaced: %s", buf);
    const char *first = strstr(buf, "[Actions]");
    CHECK(first && !strstr(first + 1, "[Actions]"), "one [Actions] section");
    CHECK(act_save(path, &p), "act_save again");
    char buf2[4096] = "";
    f = fopen(path, "rb");
    if (f) { size_t n = fread(buf2, 1, sizeof buf2 - 1, f); buf2[n] = 0; fclose(f); }
    CHECK(!strcmp(buf, buf2), "saving twice changes nothing");
    remove(path);
    char label[64];
    act_input_label(ACT_PAD + SDL_GAMEPAD_BUTTON_SOUTH, label, sizeof label);
    CHECK(!strcmp(label, "Pad South") && act_input_device(ACT_PAD_LT) == 0 && act_input_device(SDL_SCANCODE_SPACE) == 1 && act_input_device(KEY_WHEEL_UP) == 1, "labels and devices");
}

// Dash and the weapon R-action: one game input until WeaponR is bound or SplitDashWeaponR=1
static void test_weapon_r(void) {
    ActProfile p = parse("[Actions]\nEnable=1\n");
    CHECK(!act_split_in(&p), "split is off by default");
    act_use_profile(&p);
    CHECK(!act_split() && act_virtual_bits(1ull << ACT_WEAPON_R) == 0, "no virtual bit while one input");
    p = parse("[Actions]\nEnable=1\nSplitDashWeaponR=1\n");
    CHECK(act_split_in(&p), "the flag splits");
    act_use_profile(&p);
    CHECK(act_split() && act_virtual_bits(1ull << ACT_WEAPON_R) == ACT_VBIT_WEAPON_R && act_virtual_bits(1ull << ACT_DASH) == 0, "virtual bit for WeaponR only");
    p = parse("[Actions]\nEnable=1\n[Actions.Pad]\nWeaponR = pad:rtrigger\n");
    CHECK(act_split_in(&p), "binding WeaponR splits");
    p = parse("[Actions]\nEnable=1\n[Actions.Kbm.Weapon.Bow]\nWeaponR = mouse:right\n");
    CHECK(act_split_in(&p), "a per-weapon WeaponR splits too");
    // WeaponR follows Dash while unbound: Dash moved, WeaponR with it
    p = parse("[Actions]\nEnable=1\n[Actions.Pad]\nDash = pad:west\n");
    act_use_profile(&p);
    int W = ACT_PAD + SDL_GAMEPAD_BUTTON_WEST, RS = ACT_PAD + SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
    ActSnap s;
    ActHost h = (ActHost){NULL, fake_held, map_sources, 0, 0, false, false, 700};
    release_all();
    press(W);
    h.sw_word = map_word();
    act_sample(&h, &s);
    CHECK((s.hold[0] >> ACT_DASH & 1) && (s.hold[0] >> ACT_WEAPON_R & 1), "west is Dash and, following it, WeaponR");
    // split inputs: Dash on west, WeaponR on rshoulder
    p = parse("[Actions]\nEnable=1\n[Actions.Pad]\nDash = pad:west\nWeaponR = pad:rshoulder\n");
    act_use_profile(&p);
    CHECK(act_split(), "split by binding");
    release_all();
    press(W);
    h.sw_word = map_word();
    act_sample(&h, &s);
    CHECK((s.hold[0] >> ACT_DASH & 1) && !(s.hold[0] >> ACT_WEAPON_R & 1), "west: Dash only");
    CHECK(act_view(ACT_VIEW_MB1, 0, s.hold[0], false) == 0x200 && act_virtual_bits(s.hold[0]) == 0, "the game sees R, no virtual bit");
    release_all();
    press(RS);
    h.sw_word = map_word();
    act_sample(&h, &s);
    CHECK(!(s.hold[0] >> ACT_DASH & 1) && (s.hold[0] >> ACT_WEAPON_R & 1), "rshoulder: WeaponR only (and R is no longer Dash)");
    CHECK(act_view(ACT_VIEW_MB1, 0x200, s.hold[0], false) == 0, "no R reaches the game");
    CHECK(act_virtual_bits(s.hold[0]) == ACT_VBIT_WEAPON_R, "the virtual bit");
    CHECK(act_view(ACT_VIEW_TOWN, 0, s.hold[0], false) == 0, "the town's run is TownRun's");
    // the ini
    char text[2048];
    p = parse("[Actions]\nEnable=1\nSplitDashWeaponR=1\nSharedInteractFrames=7\n[Actions.Pad]\nWeaponR = pad:rshoulder\n");
    CHECK(p.split && p.shared_frames == 7, "keys");
    act_format(&p, text, sizeof text);
    CHECK(strstr(text, "SplitDashWeaponR=1") && strstr(text, "SharedInteractFrames=7") && strstr(text, "WeaponR = pad:rshoulder"), "format: %s", text);
    CHECK(parse("[Actions]\nSharedInteractFrames=99\n").shared_frames == 30, "clamped");
    // Dash's shared-input note goes away once split
    act_set_log(collect);
    logbuf[0] = 0;
    p = parse("[Actions]\nEnable=1\n[Actions.Pad]\nDash = pad:west\n");
    act_validate(&p);
    CHECK(strstr(logbuf, "Dash is the same game input"), "note while one input: %s", logbuf);
    logbuf[0] = 0;
    p = parse("[Actions]\nEnable=1\n[Actions.Pad]\nDash = pad:west\nWeaponR = pad:rshoulder\n");
    act_validate(&p);
    CHECK(!strstr(logbuf, "Dash is the same game input"), "no note once split: %s", logbuf);
    act_set_log(quiet);
}

// One input for Interact and Dodge (the quest box): Interact first, then the others
static void test_gate(void) {
    const uint64_t I = 1ull << ACT_INTERACT, D = 1ull << ACT_DODGE, X = 1ull << ACT_ATTACK_X;
    ActProfile p = parse("[Actions]\nEnable=1\n[Actions.Pad]\nInteract = pad:west\nDodge = pad:west\n");
    act_use_profile(&p);
    CHECK(act_interact_share(0) == 0, "Dodge and Interact on one input: told apart by the state, nothing to try first: %llx", (unsigned long long)act_interact_share(0));
    CHECK(act_crouch_split(), "and that splits the crouch from the roll");
    p = parse("[Actions]\nEnable=1\n[Actions.Pad]\nInteract = pad:west\nDodge = pad:east\n");
    act_use_profile(&p);
    CHECK(act_interact_share(0) == 0, "separate inputs share nothing");
    p = parse("[Actions]\nEnable=1\n[Actions.Pad]\nAttackA = pad:west\nDodge = pad:east\n");
    act_use_profile(&p);
    CHECK(act_interact_share(0) == 0, "Interact following AttackA, which is not a contender");
    p = parse("[Actions]\nEnable=1\n[Actions.Pad]\nInteract = pad:west\nDodge = pad:west\nAttackX = pad:west\n[Actions.Pad.Weapon.Bow]\nDodge = pad:south\n");
    act_use_profile(&p);
    CHECK(act_interact_share(0) == X, "AttackX still waits for Interact: %llx", (unsigned long long)act_interact_share(0));
    CHECK(act_interact_share(10) == X, "the Bow's own Dodge is elsewhere: %llx", (unsigned long long)act_interact_share(10));
    p = parse("[Actions]\nEnable=1\n[Actions.Kbm]\nInteract = key:e\nDodge = key:leftctrl+key:e\nAttackX = key:q\n");
    act_use_profile(&p);
    CHECK(act_interact_share(0) == 0 && act_crouch_split(), "a chord that holds the input is the same: %llx", (unsigned long long)act_interact_share(0));
    CHECK(act_shared_frames() == 1, "default frames");

    // not ambiguous: nothing changes
    ActGate g = {0};
    CHECK(act_gate(&g, D, D, false, 100, 5) == D && act_gate(&g, I, D, false, 101, 5) == I && act_gate(&g, I | X, D, false, 102, 5) == (I | X), "no ambiguity: as it is");
    // pressed on a box: Interact only; the action changes: the others stay off for the whole hold
    g = (ActGate){0};
    uint64_t both = I | D;
    CHECK(act_gate(&g, both, D, false, 200, 5) == I, "frame 0: Interact only");
    CHECK(act_gate(&g, both, D, false, 202, 5) == I, "frame 2: Interact only");
    CHECK(act_gate(&g, both, D, true, 204, 5) == I, "the action changed: an interaction");
    CHECK(act_gate(&g, both, D, true, 230, 5) == I, "and it stays one while held");
    CHECK(act_gate(&g, 0, D, true, 231, 5) == 0 && g.phase == 0, "released");
    // nothing to interact with: a roll, 5 frames late
    g = (ActGate){0};
    CHECK(act_gate(&g, both, D, false, 300, 5) == I, "frame 0");
    CHECK(act_gate(&g, both, D, false, 304, 5) == I, "frame 4");
    CHECK(act_gate(&g, both, D, false, 305, 5) == D, "frame 5: the roll, and no A");
    CHECK(act_gate(&g, both, D, false, 320, 5) == D, "held: still");
    CHECK(act_gate(&g, 0, D, false, 321, 5) == 0 && g.phase == 0, "released");
    // a tap shorter than that: the roll comes after it
    g = (ActGate){0};
    CHECK(act_gate(&g, both, D, false, 400, 5) == I, "tap frame 0");
    CHECK(act_gate(&g, both, D, false, 401, 5) == I, "tap frame 1");
    CHECK(act_gate(&g, 0, D, false, 402, 5) == D, "released unresolved: the roll is pressed");
    CHECK(act_gate(&g, 0, D, false, 404, 5) == D, "for a few frames");
    CHECK(act_gate(&g, 0, D, false, 405, 5) == 0, "and ends");
    // a tap on a box: the action changes after the release: nothing else
    g = (ActGate){0};
    act_gate(&g, both, D, false, 500, 5);
    CHECK(act_gate(&g, 0, D, true, 501, 5) == 0, "released and it was an interaction (changed): no roll");
    // two contenders: both are held back, both come
    g = (ActGate){0};
    CHECK(act_gate(&g, I | D | X, D | X, false, 600, 5) == I, "Dodge and AttackX held back");
    CHECK(act_gate(&g, I | D | X, D | X, false, 606, 5) == (D | X), "then both");
    // the next press starts again
    CHECK(act_gate(&g, 0, D | X, false, 607, 5) == 0, "release");
    CHECK(act_gate(&g, both, D | X, false, 700, 5) == I, "next press: Interact first again");
}

// The Simple layout: merged rows, what setting one writes, and what claiming an input does to the others
static int row_named(const char *name) {
    for (int i = 0; i < act_row_count(); i++)
        if (!strcmp(act_row(i)->name, name)) return i;
    return -1;
}
static void test_rows(void) {
    // every action is in exactly one row, and a row's actions share a button (their defaults are one Switch button)
    int seen[ACT_COUNT] = {0};
    for (int i = 0; i < act_row_count(); i++) {
        const ActRow *r = act_row(i);
        CHECK(*r->label && *r->name, "row %d has a name and a label", i);
        seen[r->head]++;
        for (int k = 0; k < 3 && r->members[k] >= 0; k++) seen[r->members[k]]++;
        CHECK(act_row_of(r->head) == i, "row_of head of %s", r->name);
    }
    for (int a = 0; a < ACT_COUNT; a++) CHECK(seen[a] == 1, "%s is in %d rows", act_name(a), seen[a]);
    CHECK(act_row_group_name(0)[0] && act_row_group_name(1)[0], "group names");
    int confirm = row_named("MenuConfirm"), cancel = row_named("MenuCancel"), plus = row_named("Pause"), l3 = row_named("FieldL3");
    int sprint = row_named("Sprint"), weaponr = row_named("WeaponR"), interact = row_named("Interact"), dodge = row_named("Dodge");
    CHECK(confirm >= 0 && cancel >= 0 && plus >= 0 && l3 >= 0 && sprint >= 0 && weaponr >= 0 && interact >= 0 && dodge >= 0, "rows exist");
    CHECK(act_row(sprint)->head == ACT_DASH && act_row(weaponr)->head == ACT_WEAPON_R && sprint != weaponr, "Sprint and the weapon action are two rows");
    CHECK(act_row_of(ACT_TOWN_INTERACT) == confirm && act_row_of(ACT_TOWN_RUN) == sprint, "town members");
    CHECK((act_row_views(confirm) >> ACT_VIEW_MB2 & 1) && (act_row_views(confirm) >> ACT_VIEW_TOWN & 1) && !(act_row_views(confirm) >> ACT_VIEW_MB1 & 1), "Confirm is read in the menus and the town");

    ActProfile p = parse("[Actions]\nEnable=1\nManageMenus=1\n");
    ActBinding b, shown;
    memset(&b, 0, sizeof b);
    b.set = true, b.n = 1, b.alt[0].n = 1, b.alt[0].code[0] = ACT_PAD + SDL_GAMEPAD_BUTTON_SOUTH;
    CHECK(act_row_state(&p, 0, confirm, &shown) == ACT_ROW_DEFAULT, "a fresh row is the default");
    act_row_set(&p, 0, confirm, &b);
    CHECK(p.dev[0].base[ACT_MENU_CONFIRM].set && p.dev[0].base[ACT_TOWN_INTERACT].set, "Confirm writes the head and the town's talk (a merged row)");
    CHECK(act_row_state(&p, 0, confirm, &shown) == ACT_ROW_BOUND && shown.alt[0].code[0] == b.alt[0].code[0], "Confirm is bound");
    int from = -1;
    CHECK(act_effective(&p, 1, ACT_TOWN_INTERACT, 0, &from) == NULL, "the other device is untouched");
    CHECK(act_effective(&p, 0, ACT_TOWN_INTERACT, 0, &from) && from == ACT_TOWN_INTERACT, "the town's talk has its own copy");
    act_row_set(&p, 0, confirm, NULL);
    CHECK(act_row_state(&p, 0, confirm, NULL) == ACT_ROW_DEFAULT && !p.dev[0].base[ACT_MENU_CONFIRM].set && !p.dev[0].base[ACT_TOWN_INTERACT].set, "Default clears it");
    // members that do not follow the head get a copy (the left stick click of the menus), those that do are cleared
    act_row_set(&p, 0, l3, &b);
    CHECK(p.dev[0].base[ACT_FIELD_L3].set && p.dev[0].base[ACT_MENU_L3].set && p.dev[0].base[ACT_TOWN_L3].set, "L3: a copy for the menus and the town");
    act_row_set(&p, 0, plus, &b);
    CHECK(p.dev[0].base[ACT_PAUSE].set && p.dev[0].base[ACT_MENU_PLUS].set && p.dev[0].base[ACT_TOWN_PLUS].set, "Start: a copy for the menus and the town");
    CHECK(act_row_state(&p, 0, plus, NULL) == ACT_ROW_BOUND && act_row_state(&p, 0, l3, NULL) == ACT_ROW_BOUND, "merged rows read as one");
    // edited apart in Advanced: Custom, and Set overwrites all of them
    p = parse("[Actions]\nEnable=1\n[Actions.Pad]\nMenuConfirm = pad:south\nTownInteract = pad:north\n");
    CHECK(act_row_state(&p, 0, confirm, NULL) == ACT_ROW_CUSTOM, "members that differ: custom");
    act_row_set(&p, 0, confirm, &b);
    CHECK(act_row_state(&p, 0, confirm, NULL) == ACT_ROW_BOUND && p.dev[0].base[ACT_TOWN_INTERACT].set && p.dev[0].base[ACT_TOWN_INTERACT].alt[0].code[0] == b.alt[0].code[0], "Set overwrites the town's too");
    // None is "not set"
    memset(&b, 0, sizeof b);
    b.set = true;
    act_row_set(&p, 0, cancel, &b);
    CHECK(act_row_state(&p, 0, cancel, NULL) == ACT_ROW_UNSET && act_rows_unset(&p, 0, NULL) == 1, "not set is counted");
}

static void test_claim(void) {
    int S = ACT_PAD + SDL_GAMEPAD_BUTTON_SOUTH, N = ACT_PAD + SDL_GAMEPAD_BUTTON_NORTH, W = ACT_PAD + SDL_GAMEPAD_BUTTON_WEST;
    int confirm = row_named("MenuConfirm"), cancel = row_named("MenuCancel"), interact = row_named("Interact"), dodge = row_named("Dodge");
    int sprint = row_named("Sprint"), weaponr = row_named("WeaponR"), attacka = row_named("AttackA");
    ActBinding on_south;
    memset(&on_south, 0, sizeof on_south);
    on_south.set = true, on_south.n = 1, on_south.alt[0].n = 1, on_south.alt[0].code[0] = S;
    // Tier 0's B is south (map_sources): Confirm on south takes it from Cancel's default, which is then not set
    ActProfile p = parse("[Actions]\nEnable=1\nManageMenus=1\n");
    act_row_set(&p, 0, confirm, &on_south);
    uint64_t ch = act_row_claim(&p, 0, confirm, S, map_sources, NULL, false);
    CHECK(ch == 1ull << cancel, "only Cancel changed: %llx", (unsigned long long)ch);
    CHECK(act_row_state(&p, 0, cancel, NULL) == ACT_ROW_UNSET, "Cancel is not set");
    CHECK(act_row_state(&p, 0, dodge, NULL) == ACT_ROW_DEFAULT, "the hunt's Dodge (B, another context) is untouched");
    uint64_t which = 0;
    CHECK(act_rows_unset(&p, 0, &which) == 1 && which == 1ull << cancel, "one row not set");
    CHECK(act_row_state(&p, 1, cancel, NULL) == ACT_ROW_DEFAULT, "the other device is untouched");
    // an input that was one of several alternatives is only taken off
    p = parse("[Actions]\nEnable=1\n[Actions.Pad]\nMenuCancel = pad:south | pad:north\n");
    act_row_set(&p, 0, confirm, &on_south);
    act_row_claim(&p, 0, confirm, S, map_sources, NULL, false);
    ActBinding shown;
    CHECK(act_row_state(&p, 0, cancel, &shown) == ACT_ROW_BOUND && shown.n == 1 && shown.alt[0].code[0] == N, "Cancel keeps its other input");
    CHECK(p.dev[0].base[ACT_TOWN_CANCEL].set && p.dev[0].base[ACT_TOWN_CANCEL].n == 1 && p.dev[0].base[ACT_TOWN_CANCEL].alt[0].code[0] == N, "and the town's B keeps it too (the merged row)");
    // two rows on one input in one context clash, in different contexts they do not
    p = parse("[Actions]\nEnable=1\n[Actions.Pad]\nMenuConfirm = pad:south\nMenuCancel = pad:south\nDodge = pad:south\n");
    CHECK(act_rows_clash(&p, 0, confirm, cancel), "Confirm and Cancel clash");
    CHECK(!act_rows_clash(&p, 0, confirm, dodge) && !act_rows_clash(&p, 0, cancel, dodge), "Dodge is another context");
    // Interact shares an input with other hunt actions by design (it is tried first), both ways
    p = parse("[Actions]\nEnable=1\n[Actions.Pad]\nInteract = pad:west\nDodge = pad:west\nAttackA = pad:west\n");
    CHECK(!act_rows_clash(&p, 0, interact, dodge) && !act_rows_clash(&p, 0, interact, attacka), "Interact is exempt");
    ActBinding on_west = on_south;
    on_west.alt[0].code[0] = W;
    act_row_set(&p, 0, dodge, &on_west);
    uint64_t took = act_row_claim(&p, 0, dodge, W, map_sources, NULL, false);
    CHECK((took >> attacka & 1) && (took >> row_named("ItemUse") & 1) && !(took >> interact & 1), "Dodge on west takes it from Attack A and Use item's default (west is Y), not from Interact: %llx", (unsigned long long)took);
    CHECK(act_row_state(&p, 0, interact, NULL) == ACT_ROW_BOUND, "Interact keeps it");
    // a row that follows another is not touched (the weapon action follows Sprint)
    p = parse("[Actions]\nEnable=1\n[Actions.Pad]\nDash = pad:rshoulder\n");
    ActBinding on_r = on_south;
    on_r.alt[0].code[0] = ACT_PAD + SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
    CHECK(act_row_claim(&p, 0, sprint, on_r.alt[0].code[0], map_sources, NULL, false) == 0, "nothing follows into a conflict");
    CHECK(act_row_state(&p, 0, weaponr, NULL) == ACT_ROW_DEFAULT || act_row_state(&p, 0, weaponr, NULL) == ACT_ROW_BOUND, "the weapon action still follows Sprint");
    // the dash role: ZR with the second Control Type
    p = parse("[Actions]\nEnable=1\n");
    act_row_set(&p, 0, row_named("FieldZR"), &on_south);
    CHECK(act_row_claim(&p, 0, row_named("FieldZR"), S, map_sources, NULL, true) == (1ull << dodge | 1ull << row_named("MenuCancel")), "ZR on south takes the default B of the hunt's Dodge and the town's Cancel");
}

static void test_scheme_layout(void) {
    ActProfile p = parse("");
    CHECK(p.scheme == ACT_SCHEME_AUTO && p.layout == ACT_LAYOUT_AUTO, "no keys: auto");
    CHECK(act_layout(&p) == ACT_LAYOUT_SIMPLE, "a fresh profile is Simple");
    p = parse("[Actions]\nEnable=1\n[Actions.Pad]\nDodge = pad:north\n");
    CHECK(act_layout(&p) == ACT_LAYOUT_ADVANCED, "a profile with bindings stays Advanced");
    p = parse("[Actions]\nEnable=1\nLayout=Simple\n[Actions.Pad]\nDodge = pad:north\n");
    CHECK(act_layout(&p) == ACT_LAYOUT_SIMPLE, "an explicit Layout wins");
    p = parse("[Actions]\nEnable=1\nScheme=Keyboard\nLayout = advanced\n");
    CHECK(p.scheme == ACT_SCHEME_KBM && p.layout == ACT_LAYOUT_ADVANCED && act_layout(&p) == ACT_LAYOUT_ADVANCED, "keys parse");
    CHECK(parse("[Actions]\nScheme=pad\n").scheme == ACT_SCHEME_PAD && parse("[Actions]\nScheme=Controller\n").scheme == ACT_SCHEME_PAD, "Controller");
    CHECK(parse("[Actions]\nScheme=nonsense\n").scheme == ACT_SCHEME_AUTO, "unknown: auto");
    char text[4096], text2[4096];
    act_format(&p, text, sizeof text);
    CHECK(strstr(text, "Scheme=Keyboard") && strstr(text, "Layout=Advanced"), "formatted: %s", text);
    ActProfile q = parse(text);
    act_format(&q, text2, sizeof text2);
    CHECK(!strcmp(text, text2) && q.scheme == p.scheme && q.layout == p.layout, "round trip");
    act_format(&(ActProfile){.enable = true}, text, sizeof text);
    CHECK(!strstr(text, "Scheme") && !strstr(text, "Layout"), "nothing written while auto");
}

// State-scoped actions: the item bar's A / B, the crouch and the roll, interact and attack
static void test_scopes(void) {
    int E = ACT_PAD + SDL_GAMEPAD_BUTTON_EAST, S = ACT_PAD + SDL_GAMEPAD_BUTTON_SOUTH, N = ACT_PAD + SDL_GAMEPAD_BUTTON_NORTH,
        W = ACT_PAD + SDL_GAMEPAD_BUTTON_WEST;
    const uint32_t MOVING = 0x10000;
    ActSnap s;
    ActHost h = (ActHost){NULL, fake_held, map_sources, 0, 0, false, false, 700};
    #define SAMPLE(code) do { release_all(); press(code); h.sw_word = map_word(); act_sample(&h, &s); } while (0)
    #define VIEW(view, ew) act_view((view), (ew), s.hold[0], false)
    // ---- Interact and Dodge on one input, Crouch on its own: no delay, the state decides
    ActProfile p = parse("[Actions]\nEnable=1\n[Actions.Pad]\nInteract = pad:south\nDodge = pad:south\nCrouch = pad:west\n");
    act_use_profile(&p);
    CHECK(act_crouch_split() && act_interact_share(0) == 0, "split, and nothing waits");
    SAMPLE(S);
    CHECK(VIEW(ACT_VIEW_SHEATHED, 0) == 0x2000, "weapon away, standing: only A (a pure interaction): %x", VIEW(ACT_VIEW_SHEATHED, 0));
    CHECK(VIEW(ACT_VIEW_SHEATHED, MOVING) == (MOVING | 0x6000), "weapon away, moving: A and B (the roll wins): %x", VIEW(ACT_VIEW_SHEATHED, MOVING));
    CHECK(VIEW(ACT_VIEW_MB1, 0) == 0x4000, "weapon out, standing: B as ever (weapon B-actions): %x", VIEW(ACT_VIEW_MB1, 0));
    CHECK(VIEW(ACT_VIEW_HUNT_ANY, 0) == 0x6000, "no state known: both, as before: %x", VIEW(ACT_VIEW_HUNT_ANY, 0));
    SAMPLE(W);  // the crouch's own input
    CHECK(VIEW(ACT_VIEW_SHEATHED, 0) == 0x4000, "weapon away, standing: Crouch is B: %x", VIEW(ACT_VIEW_SHEATHED, 0));
    CHECK(VIEW(ACT_VIEW_SHEATHED, MOVING) == MOVING, "moving: Crouch does nothing: %x", VIEW(ACT_VIEW_SHEATHED, MOVING));
    CHECK(VIEW(ACT_VIEW_MB1, MOVING) == (MOVING | 0x4000) || VIEW(ACT_VIEW_MB1, MOVING) == MOVING, "weapon out: not scoped by the stick of the sheathed state");
    // ---- Crouch never shares with Interact: Interact wins standing
    p = parse("[Actions]\nEnable=1\n[Actions.Pad]\nInteract = pad:west\nCrouch = pad:west\n");
    act_use_profile(&p);
    SAMPLE(W);
    CHECK(VIEW(ACT_VIEW_SHEATHED, 0) == 0x2000, "Crouch and Interact on one input: Interact wins standing: %x", VIEW(ACT_VIEW_SHEATHED, 0));
    act_set_log(collect);
    logbuf[0] = 0;
    act_validate(&p);
    CHECK(strstr(logbuf, "conflict: Crouch and Interact are both on"), "and it is said: %s", logbuf);
    act_set_log(quiet);
    // ---- Dodge and Interact on one input, Crouch following Dodge: the input only interacts standing
    p = parse("[Actions]\nEnable=1\n[Actions.Pad]\nInteract = pad:south\nDodge = pad:south\n");
    act_use_profile(&p);
    CHECK(act_crouch_split(), "Dodge and Interact on one input splits");
    SAMPLE(S);
    CHECK(VIEW(ACT_VIEW_SHEATHED, 0) == 0x2000 && VIEW(ACT_VIEW_SHEATHED, MOVING) == (MOVING | 0x6000), "no crouch and no delay: %x %x", VIEW(ACT_VIEW_SHEATHED, 0), VIEW(ACT_VIEW_SHEATHED, MOVING));
    // ---- Dodge alone: as before
    p = parse("[Actions]\nEnable=1\n[Actions.Pad]\nDodge = pad:south\n");
    act_use_profile(&p);
    CHECK(!act_crouch_split(), "Dodge alone does not split");
    SAMPLE(S);
    CHECK(VIEW(ACT_VIEW_SHEATHED, 0) == 0x4000 && VIEW(ACT_VIEW_SHEATHED, MOVING) == (MOVING | 0x4000), "B standing and moving");
    // ---- the item bar: A and B are the bar's only
    p = parse("[Actions]\nEnable=1\n[Actions.Pad]\nDodge = pad:south\nInteract = pad:south\nAttackA = pad:east\n");
    act_use_profile(&p);
    SAMPLE(S);  // Tier 0's B
    CHECK(VIEW(ACT_VIEW_SHEATHED | ACT_VIEW_BAR, 0) == 0x4000, "bar open, south: the bar's previous item (B), not Interact's A: %x", VIEW(ACT_VIEW_SHEATHED | ACT_VIEW_BAR, 0));
    CHECK(VIEW(ACT_VIEW_MB1 | ACT_VIEW_BAR, 0) == 0x4000, "bar open, weapon out, south: B");
    CHECK(VIEW(ACT_VIEW_SHEATHED, 0) == 0x2000, "bar closed: Interact as set: %x", VIEW(ACT_VIEW_SHEATHED, 0));
    SAMPLE(E);  // Tier 0's A
    CHECK(VIEW(ACT_VIEW_MB1 | ACT_VIEW_BAR, 0) == 0x2000, "bar open, east: the bar's next item (A): %x", VIEW(ACT_VIEW_MB1 | ACT_VIEW_BAR, 0));
    // the bar's actions bound: only the bar moves
    p = parse("[Actions]\nEnable=1\n[Actions.Pad]\nItemBarLeft = pad:north\nItemBarRight = pad:west\n");
    act_use_profile(&p);
    SAMPLE(N);
    CHECK(VIEW(ACT_VIEW_MB1 | ACT_VIEW_BAR, 0) == 0x5000, "bar open, north: the bar's B, and X is still X (not claimed): %x", VIEW(ACT_VIEW_MB1 | ACT_VIEW_BAR, 0));
    CHECK(VIEW(ACT_VIEW_MB1, 0) == 0x1000, "bar closed: north is X as ever: %x", VIEW(ACT_VIEW_MB1, 0));
    SAMPLE(S);  // Tier 0's B: no longer the bar's
    CHECK(VIEW(ACT_VIEW_MB1 | ACT_VIEW_BAR, 0) == 0, "bar open, south: nothing (the bar's B moved), and not Dodge's: %x", VIEW(ACT_VIEW_MB1 | ACT_VIEW_BAR, 0));
    CHECK(VIEW(ACT_VIEW_MB1, 0) == 0x4000, "bar closed: south is still Dodge: %x", VIEW(ACT_VIEW_MB1, 0));
    SAMPLE(W);
    CHECK(VIEW(ACT_VIEW_MB1 | ACT_VIEW_BAR, 0) == 0xA000, "bar open, west: the bar's A, and Y is still Y: %x", VIEW(ACT_VIEW_MB1 | ACT_VIEW_BAR, 0));
    CHECK(VIEW(ACT_VIEW_MB1, 0) == 0x8000, "bar closed: west is Y (Tier 0's): %x", VIEW(ACT_VIEW_MB1, 0));
    // the chord's X is not the bar's
    ActProfile q = parse("[Actions]\nEnable=1\n");
    act_use_profile(&q);
    CHECK(act_view(ACT_VIEW_MB1 | ACT_VIEW_BAR, 0, 1ull << ACT_CHORD_XA, false) == 0x1000, "chord with the bar open: X only");
    // ---- town and menus are not state-scoped
    CHECK(act_view(ACT_VIEW_TOWN, 0, 1ull << ACT_TOWN_INTERACT, false) == 0x2000 && act_view(ACT_VIEW_MB2, 0, 1ull << ACT_MENU_CANCEL, false) == 0x4000, "town and menus as ever");
    #undef SAMPLE
    #undef VIEW
}

// The Simple layout: Crouch is its own row and never clashes with Dodge, always with Interact
static void test_crouch_rows(void) {
    int crouch = row_named("Crouch"), dodge = row_named("Dodge"), interact = row_named("Interact"), left = row_named("ItemBarLeft"),
        right = row_named("ItemBarRight"), attacka = row_named("AttackA");
    CHECK(crouch >= 0 && left >= 0 && right >= 0 && act_row(crouch)->head == ACT_CROUCH, "rows exist");
    ActProfile p = parse("[Actions]\nEnable=1\n[Actions.Pad]\nCrouch = pad:south\nDodge = pad:south\nInteract = pad:south\nItemBarLeft = pad:south\nAttackA = pad:south\n");
    CHECK(!act_rows_clash(&p, 0, crouch, dodge), "Crouch and Dodge on one input is the game's B");
    CHECK(act_rows_clash(&p, 0, crouch, interact), "Crouch and Interact clash");
    CHECK(!act_rows_clash(&p, 0, left, attacka) && !act_rows_clash(&p, 0, left, dodge), "the bar's actions are a state of their own");
    CHECK(act_rows_clash(&p, 0, left, right) == false, "(different inputs)");
    p = parse("[Actions]\nEnable=1\n[Actions.Pad]\nItemBarLeft = pad:south\nItemBarRight = pad:south\n");
    CHECK(act_rows_clash(&p, 0, left, right), "the bar's two clash with each other");
}

int main(void) {
    act_set_log(quiet);
    test_registry();
    test_ini();
    test_validate();
    test_golden();
    test_rebind2();
    test_weapon();
    test_chord_alt();
    test_town();
    test_scopes();
    test_crouch_rows();
    test_interact();
    test_all_buttons();
    test_script();
    test_save();
    test_rows();
    test_claim();
    test_scheme_layout();
    test_weapon_r();
    test_gate();
    printf("test_actions: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
