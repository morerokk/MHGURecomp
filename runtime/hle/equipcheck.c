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
#include <stdint.h>

#include "../hle.h"

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
    if (!class_has_hunter(cls)) ST8(rec + 0x4d4u, 0);
    return 2;
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
