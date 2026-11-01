// The mouse wheel turns the item bar directly: one item left or right per notch, without holding L (host_input.c).
//
// The player keeps the selected pouch slot at +0x3298 (u16: 0x28 the empty "(No Item)" place, else the slot: in a test
// save 0 Potion, 1 Mega Potion). Its item bar handler (0x4f7f38, r0 = the player; called every update from 0x49baf4)
// returns early unless f_47bcd0(player) == 1, f_47b918(player) == 0 and vfunc +0x230(player, 1) == 1 (the player may
// pick an item now). Then, while the bar is open (L held: vfunc +0x25c(player, 1)) or for the player's actions 0x43 /
// 0x44 (vfunc +0x2e8, "action pressed": an item to the left / right without L), it moves the selection: actions 0xa /
// 0x44 right, 9 / 0x43 left: +0x3298 = f_4880fc(player, slot, 0 right / 1 left), which skips the empty slots (and
// stops at the empty place, the bar's left end), and sets bit 1 (right) or 2 (left) of +0x253c for the bar's turn
// animation. The handler is hooked: after it, a notch of
// the wheel does the same, past the same checks. One notch per update, at most a few waiting; notches while the
// handler doesn't run (towns, menus, cutscenes) or while the player may not pick an item are dropped.
#include <pthread.h>

#include "../hle.h"
#include "itembar.h"

#define FN_ITEM_BAR      0x4f7f38u  // the player's item bar handler (r0 = the player)
#define FN_PL_IS_LOCAL   0x47bcd0u  // (player) 1: the handler goes on
#define FN_PL_BUSY       0x47b918u  // (player) 0: the handler goes on
#define FN_ITEM_NEXT     0x4880fcu  // (player, slot, 0 right / 1 left): the next slot holding an item
#define VF_CAN_SELECT    0x230u     // vfunc (player, 1) 1: may pick an item now
#define PL_ITEM_SLOT     0x3298u    // u16: the selected pouch slot
#define PL_ITEM_FLAGS    0x253cu    // bit 1: turned right, bit 2: turned left (the bar's animation)
#define MAX_WAITING      4
#define ACTIVE_NS        200000000ull  // notches older than the handler's last run by this much are dropped

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_steps;                  // notches waiting: < 0 left, > 0 right
static volatile uint64_t g_seen_ns;  // when the handler last ran

void itembar_scroll(int steps) {
    uint64_t seen = g_seen_ns;
    if (!seen || plat_now_ns() - seen >= ACTIVE_NS) return;  // (no item bar now)
    pthread_mutex_lock(&g_lock);
    g_steps += steps;
    if (g_steps > MAX_WAITING) g_steps = MAX_WAITING;
    if (g_steps < -MAX_WAITING) g_steps = -MAX_WAITING;
    pthread_mutex_unlock(&g_lock);
}

static uint32_t call(Cpu *c, uint32_t fn, uint32_t r0, uint32_t r1, uint32_t r2) {
    uint32_t saved[16];
    for (int i = 0; i < 16; i++) saved[i] = c->r[i];
    c->r[0] = r0, c->r[1] = r1, c->r[2] = r2;
    guest_call(c, fn);
    uint32_t v = c->r[0];
    for (int i = 0; i < 16; i++) c->r[i] = saved[i];
    return v;
}

static bool sane(uint32_t a) { return a >= 0x08000000u && a < 0xF0000000u && !(a & 3); }

static void item_bar(Cpu *c) {
    uint32_t pl = c->r[0];
    fnhook_call_original(c, FN_ITEM_BAR);
    uint64_t now = plat_now_ns(), seen = g_seen_ns;
    g_seen_ns = now;
    pthread_mutex_lock(&g_lock);
    if (seen && now - seen >= ACTIVE_NS) g_steps = 0;  // (scrolled while it didn't run)
    int dir = g_steps > 0 ? 1 : g_steps < 0 ? -1 : 0;
    pthread_mutex_unlock(&g_lock);
    if (!dir || !sane(pl) || call(c, FN_PL_IS_LOCAL, pl, 0, 0) != 1) return;  // (another player's: not ours)
    uint32_t vt = LD32(pl);
    uint32_t can_select = vt ? LD32(vt + VF_CAN_SELECT) : 0;  // (vtables are in main's image: not heap addresses)
    bool may = call(c, FN_PL_BUSY, pl, 0, 0) == 0 && can_select && call(c, can_select, pl, 1, 0) == 1;
    pthread_mutex_lock(&g_lock);
    g_steps = may ? g_steps - dir : 0;  // (not now: dropped, not kept for when the attack is over)
    pthread_mutex_unlock(&g_lock);
    if (!may) return;
    uint32_t slot = call(c, FN_ITEM_NEXT, pl, LD16(pl + PL_ITEM_SLOT), dir > 0 ? 0 : 1);
    ST16(pl + PL_ITEM_SLOT, slot);
    ST32(pl + PL_ITEM_FLAGS, LD32(pl + PL_ITEM_FLAGS) | (dir > 0 ? 2u : 4u));
}

void itembar_init(void) {
    fnhook_set(FN_ITEM_BAR, hle_register("itembar:item_bar", item_bar));
}
