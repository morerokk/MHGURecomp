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
                         "MenuConfirm = key:space\n");
    int n = act_validate(&p);
    CHECK(strstr(logbuf, "conflict: Dodge and AttackA"), "conflict in one view: %s", logbuf);
    CHECK(!strstr(logbuf, "MenuConfirm and Dodge"), "no conflict across views");
    CHECK(strstr(logbuf, "ItemUse uses Escape"), "reserved key warned: %s", logbuf);
    CHECK(strstr(logbuf, "AttackA is the same game input as"), "shared-input note");
    CHECK(n >= 3, "count %d", n);
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
    CHECK(w == 0x200, "R is not the dash role with ZR: unmanaged, passes %x", w);
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

int main(void) {
    act_set_log(quiet);
    test_registry();
    test_ini();
    test_validate();
    test_golden();
    test_rebind2();
    test_weapon();
    test_chord_alt();
    printf("test_actions: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
