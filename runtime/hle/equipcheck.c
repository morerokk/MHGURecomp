// A hunter's equipment piece the game can't use. f_46e964 puts one piece (weapon, armor, talisman, Palico gear) on a
// hunter's model; f_46ec84 first checks it: the piece's kind (+4 & 0x1f) must fit the hunter (Palico gear only on a
// Prowler, 0x4d4 == 0xf, hunter gear only on a hunter), its id (+6, and the look of +8) must be in this game's data,
// and an armor piece must pass its data's checks. A piece that fails is swapped for an empty one of the same kind (ids
// 0) and that one is put on instead, by calling f_46e964 again. An unknown armor id is fine (an empty armor piece
// passes), but when the empty one fails as well the game calls itself forever: a piece of a kind this game has no
// data for (the lookup 0x33c208 knows kinds 1-24; 7-21 weapons, 22-24 Palico gear), as a mod's new weapon type would
// be, or gear of the wrong kind for the model (Palico gear on a hunter, hunter gear on a Prowler). The stack runs
// out and the game crashes the moment that hunter shows up (seen 2026-11-23 when a third hunter joined a Hub: an
// abort 2001-0114 with 0x46e9dc all over the stack). The check doesn't look at anything else of the piece, so an
// empty one that fails always fails again: here the empty one is left off instead (`add r6, sp, #4` at 0x46e998, the
// first instruction of the swap, goes to the function's return), and the hunter shows without that piece.
// tools/gen.py PATCHES.
#include <stdint.h>

#include "../hle.h"

static uint32_t g_kind, g_id, g_look;  // the piece that failed before the empty one
static int g_logged;

// 1: the piece r5 failed the check and is already empty (leave it off)
int equipcheck_swap(Cpu *c) {
    uint32_t piece = c->r[5];
    uint32_t kind = LD16(piece + 4u) & 0x1fu, id = LD16(piece + 6u), look = LD16(piece + 8u);
    if (id || look) {
        g_kind = kind, g_id = id, g_look = look;
        return 0;
    }
    if (g_logged < 8) {
        g_logged++;
        LOG("[equip] a hunter's piece of kind %u (id %u, look %u) can't be on them (weapon class %u), nor an empty one "
            "of its kind: left off\n", g_kind == kind ? g_kind : kind, g_kind == kind ? g_id : 0, g_kind == kind ? g_look : 0,
            LD8(c->r[4] + 0x4d4u));
    }
    g_kind = 0x20;
    return 1;
}

