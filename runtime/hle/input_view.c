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
//
// Pointer feed (Pointer=1, runtime-only probe P1 of the research): before f_5de064 the host's pointer is written to
// sPad+0x68DC / +0x68E0 and the pad's held word gets bit 24 while it is down; f_5de064 copies both into the button
// object's pointer block, and the 21 UI readers of that block (if any of them does anything) see a mouse.
#include <stdio.h>
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
#define PL_ACT_NO      0x250au    // weapon move, 1 a common one; the drawn / sheathed discriminator is in these (P3)

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
static bool g_log;
static uint32_t g_in[2], g_out[2];  // the words last given to / from the translators of each view
static bool g_menus, g_pointer;

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
void input_view_status(void) {
    uint64_t act = g_snap.hold[g_var] | g_snap.pulse[g_var];
    char names[400] = "";
    for (int i = 0; i < ACT_COUNT; i++)
        if (act >> i & 1) snprintf(names + strlen(names), sizeof names - strlen(names), "%s ", act_name(i));
    LOG("[actions] frame %llu ctx %s weapon %d (%s) player %08x act %x:%x dash %s active: %s\n", (unsigned long long)g_frame,
        input_view_context(), g_weapon_pub, g_weapon_pub >= 0 ? act_weapon_name(g_weapon_pub) : "-", g_player, g_act_st,
        g_act_no, g_dash_zr ? "ZR" : "R", names);
}

// ---- context and weapon
static void player_update(Cpu *c) {
    uint32_t pl = c->r[0];
    if (sane(pl) && call(c, FN_PL_IS_LOCAL, pl, 0, 0) == 1) {
        g_pl_frame = g_frame;
        g_player = pl, g_act_st = LD16(pl + PL_ACT_ST), g_act_no = LD16(pl + PL_ACT_NO);
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
        LOG("[actions] f%llu %s w%d mb1 in %08x out %08x cur %08x | mb2 in %08x out %08x cur %08x | act %llx\n",
            (unsigned long long)g_frame, input_view_context(), g_weapon_pub, g_in[0], g_out[0], LD32(mb + 4), g_in[1],
            g_out[1], LD32(sp + MB2_OFF + 4), (unsigned long long)(g_snap.hold[g_var] | g_snap.pulse[g_var]));
    }
}

// ---- the translators
static void tr_view(Cpu *c, int view, uint32_t fn) {
    uint32_t in = c->r[1];
    uint32_t out = act_view(view, in, g_snap.hold[g_var] | g_snap.pulse[g_var], g_dash_zr);
    g_in[view] = in, g_out[view] = out;
    c->r[1] = out;
    fnhook_call_original(c, fn);
}
static void tr_mb1(Cpu *c) { tr_view(c, ACT_VIEW_MB1, FN_TR_MB1); }
static void tr_mb2(Cpu *c) {
    if (g_menus) tr_view(c, ACT_VIEW_MB2, FN_TR_MB2);
    else fnhook_call_original(c, FN_TR_MB2);
}

static void log_line(const char *line) { LOG("%s\n", line); }

void input_view_init(void) {
    char path[1100];
    config_path(path, sizeof path);
    act_set_log(log_line);
    act_init(path);
    if (!act_enabled()) return;
    const ActProfile *p = act_profile();
    g_log = p->log, g_menus = p->menus, g_pointer = p->pointer;
    fnhook_set(FN_MB_UPDATE, hle_register("inputview:mb_update", mb_update));
    fnhook_set(FN_TR_MB1, hle_register("inputview:tr_mb1", tr_mb1));
    fnhook_set(FN_TR_MB2, hle_register("inputview:tr_mb2", tr_mb2));
    fnhook_set(FN_PLAYER, hle_register("inputview:player", player_update));
    fnhook_set(FN_TOWN, hle_register("inputview:town", town_update));
    for (int i = 0; i < ACT_WEAPONS; i++) {
        char name[32];
        snprintf(name, sizeof name, "inputview:mapper%d", i);
        fnhook_set(mappers[i], hle_register(name, mapper_fn[i]));
    }
    LOG("[actions] translators hooked\n");
}
