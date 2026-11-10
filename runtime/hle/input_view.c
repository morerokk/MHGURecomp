// Game-action rebinding, the game side (runtime/actions.h).
//
// The engine builds two views of the pad from one word (sFestaPad::update f_5ddf1c, once per frame): f_5de064 takes
// the pad's held word (r1) and calls a translator through the button object's vtable: f_5de4b0 for mb1 (the
// gameplay view: the Player copy, the town avatar, the camera) and f_5de4f8 for mb2 (the ~320 menu readers), and
// derives cur / prev / trig / rel from what comes back. Both are pure functions of r1, so they are hooked: r1 becomes
// act_view(), the original runs, and the engine's own edge logic sees the rebound word (chords and combos come out
// right for free). The hooks are installed only with [Actions] Enable=1.
//
// The actions come from the host (runtime/hle/input.c): the set active when the game polled the pad this frame.
// The context (field / town / menu) and the weapon class come from the engine's own code running: the player's
// update f_4a79cc, the town avatar's f_48d1f0, and the 15 class mappers ([[player]+0x22C], one per weapon type).
// The gameplay word is built for one of: a hunt with the weapon out (A is AttackA), a hunt with it away (A is
// Interact), a town (the Town* actions).
//
// Pointer feed (Pointer=1, runtime-only probe P1 of the research): before f_5de064 the host's pointer is written to
// sPad+0x68DC / +0x68E0 and the pad's held word gets bit 24 while it is down; f_5de064 copies both into the button
// object's pointer block, and the 21 UI readers of that block (if any of them does anything) see a mouse.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../actions.h"
#include "../config.h"
#include "../hle.h"
#include "input.h"
#include "input_view.h"

#define FN_MB_UPDATE   0x5de064u  // (button object) cur / prev / trig / rel and the pointer block from the pad's word
#define FN_TR_MB1      0x5de4b0u  // (button object, r1 = engine word) -> the gameplay word
#define FN_TR_MB2      0x5de4f8u  // (button object, r1 = engine word) -> the menu word
#define FN_PLAYER      0x4a79ccu  // the hunter's update (r0 = the player)
#define FN_TOWN        0x48d1f0u  // the town avatar's update
#define FN_PL_IS_LOCAL 0x47bcd0u  // (player) 1: ours
#define FN_KC_R        0x85f93cu  // () the R-role mask the game tests: R (mb1 0x40), or ZR (0x800) with the second Control Type
#define FN_PAD_INDEX   0xdad28cu  // (sPad) the index of the pad slot in use
#define G_SPAD         0x1a3cbfcu // GOT entry: **sPad
#define G_OPTIONS      0x1a97f78u // options object *
#define OPT_DASH_ROLE  0x446eu    // byte: 0 R is the dash button, else ZR
#define MB1_OFF        0x6b98u    // button objects inside sPad
#define MB2_OFF        0x6bf0u
#define PAD_STRIDE     0xb50u
#define PAD_HELD       0x1d0u
#define PAD_PTR_X      0x68dcu
#define PAD_PTR_Y      0x68e0u
#define PL_ACT_ST      0x2506u    // u16: the hunter's current action (act_st, act_no; hle/movelog.c): act_st 0x10 a
#define PL_ACT_NO      0x250au    // weapon move, 1 a common one
#define PL_DRAWN       0x114cu    // u8: 1 the weapon is out, 0 put away (constant over idle, walk, roll and attacks of
                                  // either state: compared in memory snapshots of a Hammer hunt, 2026-11-10)
#define CLS_PROWLER    14         // (no weapon to put away: its A button attacks and gathers)

// The callers of the R-role getter (return addresses; f_XXXXXX = main + 0x200000) that are the weapon's R-action
// (guard, charge, aim, special), found with the PadRemap mod (getter log + play) and the research's P16: the
// 15 weapon mappers and their state functions, and these sites of the input update (f_47ca28 builds the intent word,
// f_47e774 and f_4a79cc convert the events). The dash tests (f_47ca28 +0x80 .. +0x1fc, f_47e774's second) are not here.
static const uint32_t kcr_weapon_sites[] = {0x47e7b0, 0x4a7bb0, 0x47d480, 0x47d508, 0x47d530, 0x47d564, 0x47cc58, 0x47d2fc, 0x47d3f8};
#define MAPPER_LO 0x1360000u  // the weapon classes' code: mappers and states of the 15 classes
#define MAPPER_HI 0x13f0000u
static const uint32_t mappers[ACT_WEAPONS] = {0x1360058, 0x1369554, 0x136f404, 0x137731c, 0x137eb58, 0x138c920, 0x1399114,
                                              0x13a03a8, 0x13aa60c, 0x13b1dc8, 0x13baef0, 0x13c2d54, 0x13cbd88, 0x13d42b0,
                                              0x13e426c};

static ActSnap g_snap;          // latched at the first f_5de064 of a frame
static int g_var;               // weapon variant: 0 none seen, 1..15 the class + 1
static uint64_t g_frame;        // frames the pad was updated
static uint64_t g_var_frame, g_pl_frame, g_town_frame;
static volatile bool g_dash_zr;
static volatile int g_weapon_pub = -1;
static uint32_t g_act_st, g_act_no, g_player;
static bool g_drawn;
static int g_view;              // the context the gameplay word was last built for
static bool g_log;
static uint32_t g_in[2], g_out[2];  // the words last given to / from the translators of each view
static bool g_menus, g_pointer;
static bool g_split;           // Dash and the weapon R-action are two inputs: the getter answers the virtual bit to weapon callers
static bool g_kclog;           // MHGU_ACTION_KCLOG=1: log each return address the R-role getter is called from (once)
static uint32_t g_kc_seen[256];
static int g_kc_n;
static bool g_on;      // the layer is on this frame ([Actions] Enable; the settings menu can turn it on and off)
static bool g_hooked;  // the hooks are in (from the first time it was on)

static bool sane(uint32_t a) { return a >= 0x08000000u && a < 0xF0000000u && !(a & 3); }

static uint32_t call(Cpu *c, uint32_t fn, uint32_t r0, uint32_t r1, uint32_t r2) {
    uint32_t saved[16];
    for (int i = 0; i < 16; i++) saved[i] = c->r[i];
    c->r[0] = r0, c->r[1] = r1, c->r[2] = r2;
    guest_call(c, fn);
    uint32_t v = c->r[0];
    for (int i = 0; i < 16; i++) c->r[i] = saved[i];
    return v;
}

static uint32_t spad(void) {
    uint32_t g = LD32(G_SPAD);  // (the address of the global that holds sPad: in the image)
    if (!g || (g & 3)) return 0;
    uint32_t p = LD32(g);
    return sane(p) ? p : 0;
}

bool input_view_dash_zr(void) { return g_dash_zr; }
int input_view_weapon(void) { return g_weapon_pub; }
const char *input_view_context(void) {
    if (g_frame - g_pl_frame <= 3 && g_pl_frame) return "FIELD";
    if (g_frame - g_town_frame <= 3 && g_town_frame) return "TOWN";
    return "MENU";
}
static const char *view_tag(int view) {
    return view == ACT_VIEW_MB1 ? "hunt, weapon out" : view == ACT_VIEW_SHEATHED ? "hunt, weapon away" : view == ACT_VIEW_TOWN ? "town" : "hunt";
}
void input_view_status(void) {
    uint64_t act = g_snap.hold[g_var] | g_snap.pulse[g_var];
    char names[400] = "";
    for (int i = 0; i < ACT_COUNT; i++)
        if (act >> i & 1) snprintf(names + strlen(names), sizeof names - strlen(names), "%s ", act_name(i));
    LOG("[actions] frame %llu ctx %s (%s) weapon %d (%s) player %08x act %x:%x %s dash %s active: %s\n",
        (unsigned long long)g_frame, input_view_context(), view_tag(g_view), g_weapon_pub,
        g_weapon_pub >= 0 ? act_weapon_name(g_weapon_pub) : "-", g_player, g_act_st, g_act_no, g_drawn ? "drawn" : "sheathed",
        g_dash_zr ? "ZR" : "R", names);
}

// ---- context and weapon
static void player_update(Cpu *c) {
    uint32_t pl = c->r[0];
    if (sane(pl) && call(c, FN_PL_IS_LOCAL, pl, 0, 0) == 1) {
        g_pl_frame = g_frame;
        g_player = pl, g_act_st = LD16(pl + PL_ACT_ST), g_act_no = LD16(pl + PL_ACT_NO);
        g_drawn = LD8(pl + PL_DRAWN) != 0;
    }
    fnhook_call_original(c, FN_PLAYER);
}
static void town_update(Cpu *c) {
    g_town_frame = g_frame;
    fnhook_call_original(c, FN_TOWN);
}
static void mapper_seen(Cpu *c, int cls, uint32_t fn) {
    uint32_t pl = c->r[0];
    if (sane(pl) && call(c, FN_PL_IS_LOCAL, pl, 0, 0) == 1) g_var = cls + 1, g_var_frame = g_frame, g_weapon_pub = cls;
    fnhook_call_original(c, fn);
}
#define MAPPER(n) static void mapper##n(Cpu *c) { mapper_seen(c, n, mappers[n]); }
MAPPER(0) MAPPER(1) MAPPER(2) MAPPER(3) MAPPER(4) MAPPER(5) MAPPER(6) MAPPER(7) MAPPER(8) MAPPER(9) MAPPER(10) MAPPER(11)
MAPPER(12) MAPPER(13) MAPPER(14)
static const HostFn mapper_fn[ACT_WEAPONS] = {mapper0, mapper1, mapper2, mapper3, mapper4, mapper5, mapper6, mapper7,
                                              mapper8, mapper9, mapper10, mapper11, mapper12, mapper13, mapper14};

// ---- the pad update: once per frame (the first of its two f_5de064 calls, mb1), latch the actions
static void mb_update(Cpu *c) {
    uint32_t mb = c->r[0], sp = spad();
    bool first = sp && mb == sp + MB1_OFF;
    if (first) {
        const ActProfile *p = act_profile();  // (replaced only on this thread: the settings menu's Save)
        g_on = act_enabled(), g_menus = p->menus, g_log = p->log, g_pointer = p->pointer;
        g_split = act_split();
    }
    if (!g_on) {  // (turned off in the settings menu: the game reads the pad as it is)
        fnhook_call_original(c, FN_MB_UPDATE);
        return;
    }
    if (first) {
        g_frame++;
        input_act_latch(&g_snap);
        if (g_var && g_frame - g_var_frame > 3) g_var = 0, g_weapon_pub = -1;  // (no hunter for a while)
        uint32_t opt = LD32(G_OPTIONS);
        g_dash_zr = sane(opt) && LD8(opt + OPT_DASH_ROLE) != 0;
    }
    if (g_pointer && sp && g_snap.ptr_valid) {
        uint32_t idx = call(c, FN_PAD_INDEX, sp, 0, 0);
        if (idx < 8) {
            ST32(sp + PAD_PTR_X, (uint32_t)g_snap.ptr_x);
            ST32(sp + PAD_PTR_Y, (uint32_t)g_snap.ptr_y);
            uint32_t held = sp + idx * PAD_STRIDE + PAD_HELD;
            if (g_snap.ptr_down) ST32(held, LD32(held) | 0x1000000u);
        }
    }
    fnhook_call_original(c, FN_MB_UPDATE);
    if (g_log && first) {
        LOG("[actions] f%llu %s w%d mb1 in %08x out %08x cur %08x | mb2 in %08x out %08x cur %08x | act %llx v%d st %x:%x lock %x\n",
            (unsigned long long)g_frame, input_view_context(), g_weapon_pub, g_in[0], g_out[0], LD32(mb + 4), g_in[1],
            g_out[1], LD32(sp + MB2_OFF + 4), (unsigned long long)(g_snap.hold[g_var] | g_snap.pulse[g_var]), g_view, g_act_st, g_act_no,
            g_player ? LD32(g_player + 0x2540) : 0);
    }
}

// ---- the translators
// The hunter's action right now (the player's own fields: not the copy taken when its update began)
static uint32_t live_action(void) { return g_player ? ((uint32_t)LD16(g_player + PL_ACT_ST) << 16 | LD16(g_player + PL_ACT_NO)) : 0; }
static void tr_view(Cpu *c, int view, int slot, uint32_t fn) {
    uint32_t in = c->r[1];
    uint64_t active = g_snap.hold[g_var] | g_snap.pulse[g_var];
    if (view == ACT_VIEW_SHEATHED) {  // an input for Interact and for other hunt actions: Interact first (actions.h)
        static ActGate gate;
        static uint32_t base;
        uint32_t now = live_action();
        if (!gate.phase) base = now;
        bool changed = now != base && (now >> 16 & 0x10) == 0;  // (a weapon move is not an interaction)
        active = act_gate(&gate, active, act_interact_share(g_var), changed, g_frame, act_shared_frames());
    }
    uint32_t out = act_view(view, in, active, g_dash_zr);
    g_in[slot] = in, g_out[slot] = out;
    c->r[1] = out;
    fnhook_call_original(c, fn);
    // the weapon's R-action, apart from Dash: a bit no pad button gives, in the gameplay word the hunter reads
    if (view != ACT_VIEW_MB2 && view != ACT_VIEW_TOWN) c->r[0] |= act_virtual_bits(active);
}
// The context of the gameplay word: a hunter updated in the last frames (weapon out or away; the Prowler has neither),
// else the town avatar, else no one (a screen that reads the gameplay word with nobody playing: the hunt's actions)
static int gameplay_view(void) {
    if (g_pl_frame && g_frame - g_pl_frame <= 3)
        return g_weapon_pub == CLS_PROWLER ? ACT_VIEW_HUNT_ANY : g_drawn ? ACT_VIEW_MB1 : ACT_VIEW_SHEATHED;
    if (g_town_frame && g_frame - g_town_frame <= 3) return ACT_VIEW_TOWN;
    return ACT_VIEW_HUNT_ANY;
}
static void tr_mb1(Cpu *c) {
    if (g_on) tr_view(c, g_view = gameplay_view(), 0, FN_TR_MB1);
    else fnhook_call_original(c, FN_TR_MB1);
}
static void tr_mb2(Cpu *c) {
    if (g_on && g_menus) tr_view(c, ACT_VIEW_MB2, 1, FN_TR_MB2);
    else fnhook_call_original(c, FN_TR_MB2);
}

// The R-role getter: with the weapon R-action split from Dash, the weapon's callers are told the virtual bit instead
static bool kcr_weapon_caller(uint32_t lr) {
    lr &= ~1u;
    if (lr >= MAPPER_LO && lr < MAPPER_HI) return true;
    for (unsigned i = 0; i < sizeof kcr_weapon_sites / sizeof *kcr_weapon_sites; i++)
        if (kcr_weapon_sites[i] == lr) return true;
    return false;
}
static void kc_r(Cpu *c) {
    uint32_t lr = c->r[14];
    fnhook_call_original(c, FN_KC_R);
    if (g_kclog) {
        bool seen = false;
        for (int i = 0; i < g_kc_n; i++) seen |= g_kc_seen[i] == lr;
        if (!seen && g_kc_n < 256) {
            g_kc_seen[g_kc_n++] = lr;
            LOG("[actions] kcr caller %08x: %s (mask %x)\n", lr, kcr_weapon_caller(lr) ? "weapon R-action" : "dash / other", c->r[0]);
        }
    }
    if (g_on && g_split && kcr_weapon_caller(lr)) c->r[0] = ACT_VBIT_WEAPON_R;
}

static void log_line(const char *line) { LOG("%s\n", line); }

void input_view_init(void) {
    char path[1100];
    config_path(path, sizeof path);
    act_set_log(log_line);
    act_init(path);
    input_view_apply();
}

// Hooks the game the first time the layer is on: at startup with [Actions] Enable=1, or when the settings menu turns it
// on (game thread, as Forge's plugins hook while the game runs). Turned off later, the hooks stay and pass through.
void input_view_apply(void) {
    if (!act_enabled() || g_hooked) return;
    const ActProfile *p = act_profile();
    g_log = p->log, g_menus = p->menus, g_pointer = p->pointer;
    g_kclog = getenv("MHGU_ACTION_KCLOG") != NULL;
    // (a hook something else put there first would be replaced, not chained: the layer stays off then; mods that hook
    // these functions later chain behind ours, as Forge does for any hook already there)
    static const uint32_t needed[] = {FN_MB_UPDATE, FN_TR_MB1, FN_TR_MB2, FN_PLAYER, FN_TOWN};
    for (unsigned i = 0; i < sizeof needed / sizeof *needed; i++)
        if (fnhook_get(needed[i])) {
            LOG("[actions] %08x is hooked already: the action layer stays off\n", needed[i]);
            return;
        }
    fnhook_set(FN_MB_UPDATE, hle_register("inputview:mb_update", mb_update));
    fnhook_set(FN_TR_MB1, hle_register("inputview:tr_mb1", tr_mb1));
    fnhook_set(FN_TR_MB2, hle_register("inputview:tr_mb2", tr_mb2));
    fnhook_set(FN_PLAYER, hle_register("inputview:player", player_update));
    fnhook_set(FN_TOWN, hle_register("inputview:town", town_update));
    for (int i = 0; i < ACT_WEAPONS; i++) {
        char name[32];
        snprintf(name, sizeof name, "inputview:mapper%d", i);
        if (!fnhook_get(mappers[i])) fnhook_set(mappers[i], hle_register(name, mapper_fn[i]));
    }
    if (!fnhook_get(FN_KC_R)) fnhook_set(FN_KC_R, hle_register("inputview:kc_r", kc_r));
    else LOG("[actions] %08x is hooked already: the weapon R-action stays one input with Dash\n", FN_KC_R);
    g_hooked = true;
    LOG("[actions] translators hooked\n");
}
