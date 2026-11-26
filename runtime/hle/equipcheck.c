// Another hunter's equipment this game can't use (a mod's new weapon type: Magnet Spike, piece kind 12 = weapon class
// 5, the class GU leaves empty, seen 2026-11-23). Two places crashed when such a hunter joined:
//
// 1. f_46e964 puts one piece (weapon, armor, talisman, Palico gear) on a hunter's model; f_46ec84 first checks it:
//    the piece's kind (+4 & 0x1f) must fit the hunter (Palico gear only on a Prowler, 0x4d4 == 0xf, hunter gear only
//    on a hunter), its id (+6, and the look of +8) must be in this game's data (the lookup 0x33c208 knows kinds 1-24;
//    1-5 armor, 6 talismans, 7-21 weapons, 22-24 Palico gear), and an armor piece must pass its data's checks. A piece
//    that fails is swapped for an empty one of the same kind (ids 0), put on by calling f_46e964 again. An unknown
//    armor id is fine (an empty armor piece passes), but for a kind with no data, or gear of the wrong kind for the
//    model, the empty one fails as well and the game called itself forever: the stack ran out the moment that hunter
//    showed up in a Hub (abort 2001-0114 with 0x46e9dc all over the stack). The check reads nothing else of the
//    piece, so an empty one that fails always fails again. Now (`add r6, sp, #4` at 0x46e998, the first instruction
//    of the swap) an empty weapon that fails becomes a Great Sword instead (STANDIN_ID) and is put on; anything else
//    is left off (the function returns).
// 2. f_475a68 makes a hunter's object for a quest from its record (0x560 bytes, weapon class at +0x4d4, the weapon at
//    +0x18) through a jump table by class (0x475b18); class 5's entry, and any class above 15, give no object, and
//    f_474cf0 calls through the null it gets back the moment the quest loads. The mod points class 5 at the Great
//    Sword's entry (its moves are built on the Great Sword's); here (`ldrb r0, [r8, #0x4d4]` at 0x475afc) a class
//    whose entry gives no object becomes the Great Sword in the record, and so does a weapon of that class.
//
// The hunter shows and plays as a Great Sword user on this game; their own game still has the real weapon. Only when
// the mod isn't here: with it, the pieces pass and its table has class 5. tools/gen.py PATCHES.
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "../hle.h"
#include "../rt.h"

#define FACTORY_TABLE 0x475b18u  // f_475a68's jump table by class (offsets from the table)
#define FACTORY_NONE  0x475db4u  // ... its "no object" return
#define KIND_GS       7u         // piece kind of the Great Sword (class 0)
#define STANDIN_ID    1u         // the first Great Sword

static uint32_t g_kind, g_id, g_look;  // the piece that failed before the empty one
static int g_logged;

static int class_has_hunter(uint32_t cls) {
    return cls <= 15u && FACTORY_TABLE + LD32(FACTORY_TABLE + 4u * cls) != FACTORY_NONE;
}

static void make_standin(uint32_t piece) {
    ST16(piece + 4u, (LD16(piece + 4u) & 0x8000u) | KIND_GS);
    ST16(piece + 6u, STANDIN_ID);
    for (uint32_t o = 8u; o < 0x28u; o += 2u) ST16(piece + o, 0);
    ST32(piece + 0x28u, 0);
}

// make_standin's piece, as the record still holds it (the game sets +0x28 again: a pointer to the weapon's data)
static bool is_standin(uint32_t piece) {
    if ((LD16(piece + 4u) & 0x1fu) != KIND_GS || LD16(piece + 6u) != STANDIN_ID) return false;
    for (uint32_t o = 8u; o < 0x28u; o += 2u)
        if (LD16(piece + o)) return false;
    return true;
}

// The records equipcheck_swap made Great Sword users, with the class they have (for their weapon icon)
#define MAX_STANDINS 8
static struct { uint32_t rec, cls; } g_standin[MAX_STANDINS];
static void remember_standin(uint32_t rec, uint32_t cls) {
    int k = -1;
    for (int i = 0; i < MAX_STANDINS && k < 0; i++)
        if (g_standin[i].rec == rec) k = i;
    for (int i = 0; i < MAX_STANDINS && k < 0; i++)
        if (!g_standin[i].rec) k = i;
    if (k < 0) k = 0;  // (full: the first goes)
    g_standin[k].rec = rec, g_standin[k].cls = cls;
}

// The piece r5 failed the check (r4 = the hunter's record). 0: swap it for an empty one (the game's way), 1: leave it
// off, 2: it is a Great Sword now, put it on
int equipcheck_swap(Cpu *c) {
    uint32_t piece = c->r[5], rec = c->r[4];
    uint32_t kind = LD16(piece + 4u) & 0x1fu, id = LD16(piece + 6u), look = LD16(piece + 8u);
    if (id || look) {
        g_kind = kind, g_id = id, g_look = look;
        return 0;
    }
    uint32_t cls = LD8(rec + 0x4d4u);
    int standin = kind >= 7u && kind <= 21u && cls != 0xfu;
    if (g_logged < 8) {
        g_logged++;
        LOG("[equip] a hunter's piece of kind %u (id %u, look %u) can't be on them (weapon class %u), nor an empty one "
            "of its kind: %s\n", kind, g_kind == kind ? g_id : 0, g_kind == kind ? g_look : 0, cls,
            standin ? "a Great Sword stands in" : "left off");
    }
    g_kind = 0x20;
    if (!standin) return 1;
    make_standin(piece);
    if (!class_has_hunter(cls)) {
        ST8(rec + 0x4d4u, 0);
        remember_standin(rec, cls);
    }
    return 2;
}

// ---- the weapon icon of such a hunter: a red X (disclosure: a hub host should see that this hunter uses a weapon this
// game doesn't have, not a Great Sword or nothing).
//
// UI icons come from 0x760918(manager, kind r1, value r2, rect r3): kind 4 is the weapon type, value the class; it
// writes the icon's rect in the HD icon sheet eng\GUI\99_texture\HD_cmn_icon_GSM_NOMIP (512 x 1024): x0, y0, x1, y1
// as floats (hammer, class 2: 56, 324, 84, 352). A class without an icon (5) gave an empty rect, so the Join Hub list
// (cUIONetworkRoomList's row 0x7dd5dc: the hub's advertise data +0x64, raw, through 0x76e6a8(ui, slot, element 7,
// kind 4, class)) and the hub's details showed nothing. Every class this game has no hunter for now gets the sheet's X
// (the cell below the smiling Felyne) instead. In a Hub the player list (cUIOPlayerInfoPanel, 0x80dfb8) takes the
// class from the hunter's record, which equipcheck_swap made the Great Sword's: equipcheck_panel_class gives it the
// class the hunter really has. The X is made red on its GUI element (0x74700c: colour) once 0x76e6a8 has put it on,
// and white again when a known weapon's icon goes on that element. With the mod installed its class has a hunter, so
// its own icon shows. Seen 2026-11-26 with Magnet Spike v0.1.0-beta.5, both ways (the mod's hunter hosting / joining).
#define FN_ICON_RECT  0x760918u  // (manager, kind, value, float rect[4])
#define FN_ICON_SET   0x76e6a8u  // (ui, slot, element, kind, [sp] value): the lookup, then 0x746f84 puts it on
#define ICON_WEAPON   4u
static const float X_RECT[4] = {243.0f, 846.0f, 291.0f, 894.0f};  // (one texel left of the cell: its right
                                                                   // edge sampled the next icon's)

static bool unknown_class(uint32_t v) { return v < 32u && !class_has_hunter(v); }

static void icon_rect(Cpu *c) {
    uint32_t kind = c->r[1], v = c->r[2], out = c->r[3];
    if (kind == ICON_WEAPON && unknown_class(v)) {
        for (int i = 0; i < 4; i++) STF(out + 4u * i, X_RECT[i]);
        return;
    }
    fnhook_call_next(c, FN_ICON_RECT);
}

// A hunter's record (r9 at 0x80dfb8, `ldrb r0, [sb, #0x4d4]`): its weapon class for the Hub's player list. The game
// fills the record from the network again a moment after equipcheck_swap (the real class and weapon are back then);
// until that, the class of the hunter it stood a Great Sword in for. Called every frame for every row.
uint32_t equipcheck_panel_class(uint32_t rec) {
    uint32_t cls = LD8(rec + 0x4d4u);
    for (int i = 0; i < MAX_STANDINS; i++) {
        if (g_standin[i].rec != rec) continue;
        if (cls == 0 && is_standin(rec + 0x18u)) return g_standin[i].cls;
        g_standin[i].rec = 0;  // (its own data again, or another hunter in this record)
    }
    return cls;
}

// The GUI element an icon goes on (0x746f84's lookup: ui+0x24 the GUI, its slot list, the instance's elements at
// +0xd8), 0 if none
#define FN_GUI_COLOR 0x74700cu  // (GUI, slot, element, colour 0xAABBGGRR)
#define RED          0xFF2020FFu
#define WHITE        0xFFFFFFFFu
static bool sane(uint32_t p) { return p >= 0x08000000u && !(p & 3u); }
static uint32_t element_of(uint32_t gui, uint32_t slot, uint32_t elem) {
    if (!sane(gui)) return 0;
    uint32_t n = LD32(gui + 0x298u), list = slot < n ? LD32(gui + 0xf8u) + 4u * slot : LD32(gui + 0x294u) + 4u * (slot - n);
    uint32_t inst = sane(list) ? LD32(list) : 0, elems = sane(inst) ? LD32(inst + 0xd8u) : 0;
    return sane(elems) ? LD32(elems + 4u * elem) : 0;
}

// The elements made red: white again when a known weapon's icon goes on one (a list row shows another hub)
#define MAX_RED 32
static uint32_t g_red[MAX_RED];

static void icon_set(Cpu *c) {
    uint32_t ui = c->r[0], slot = c->r[1], elem = c->r[2], kind = c->r[3], v = LD32(c->r[13]);
    fnhook_call_next(c, FN_ICON_SET);
    if (kind != ICON_WEAPON) return;
    uint32_t gui = LD32(ui + 0x24u), obj = element_of(gui, slot, elem);
    if (!obj) return;
    int k = -1;
    for (int i = 0; i < MAX_RED; i++)
        if (g_red[i] == obj) k = i;
    uint32_t color;
    if (unknown_class(v)) {
        if (k < 0) {
            for (int i = 0; i < MAX_RED && k < 0; i++)
                if (!g_red[i]) k = i;
            g_red[k < 0 ? 0 : k] = obj;  // (full: the first goes)
        }
        color = RED;
    } else if (k >= 0) {
        color = WHITE;
        g_red[k] = 0;
    } else {
        return;
    }
    uint32_t saved[4];
    memcpy(saved, c->r, sizeof saved);
    c->r[0] = gui, c->r[1] = slot, c->r[2] = elem, c->r[3] = color;
    guest_call(c, FN_GUI_COLOR);
    memcpy(c->r, saved, sizeof saved);
}

void equipcheck_init(void) {
    fnhook_set_outer(FN_ICON_RECT, hle_register("equipcheck:icon_rect", icon_rect));
    fnhook_set_outer(FN_ICON_SET, hle_register("equipcheck:icon_set", icon_set));
}

// f_475a68's class (r8 = the hunter's record): one this game makes no hunter for becomes the Great Sword
uint32_t equipcheck_class(uint32_t rec) {
    uint32_t cls = LD8(rec + 0x4d4u);
    if (class_has_hunter(cls)) return cls;
    uint32_t weapon = rec + 0x18u, kind = LD16(weapon + 4u) & 0x1fu;
    LOG("[equip] a hunter's weapon class %u has no hunter in this game (their weapon: kind %u id %u): a Great Sword "
        "stands in\n", cls, kind, LD16(weapon + 6u));
    if (kind != KIND_GS) make_standin(weapon);
    ST8(rec + 0x4d4u, 0);
    return 0;
}
