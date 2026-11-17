// The quest rewards screen (after a quest: the Quest Rewards menu, "Send to the Item Box", the Reward Box grid).
// Its update is main 0x588ee8 (r0 the screen, r1 its mode 0..5): a state machine on +0x44 (2 the menu, 4 the grid
// where the rewards are picked to go to the box) with a cCursor and, at +0xc4, a cBoxCursor: x +0xd8, y +0xd9, cols
// +0xda, rows +0xdb, page +0xdc (u16). The grid's slots are u16 id + u16 count, from the array at +0x5c / +0x64 /
// +0x6c / +0x74 by mode (modes 4 and 5 use mode 0's, as the game's jump table at 0x588fb0 does). In state 4 an A on
// a slot sends it (its entry becomes 0) and the state stays, the cursor where it was.
// [Quest Rewards] HoldASend: A held in state 4 repeats like a key. The game sends the item under the cursor at the
// press, the cursor goes on to the next filled slot (SELECT_NS), then that one is sent after INITIAL_NS and the next
// every REPEAT_NS, until none is left or A is let go (a tap is one item, as before). Each send is the game's own A,
// so a full box or an item that can't go is left as the game leaves it.
#include "../config.h"
#include "../hle.h"

#define FN_REWARDS 0x588ee8u
#define PAD_ON     0x6bf4u  // sFestaPad: held / pressed this update
#define PAD_TRG    0x6bfcu
#define PAD_DIRS   0x6c34u  // its bits 0xf0 are added to PAD_TRG by 0x588ee8 (the stick's directions)
#define B_A        0x2000u
#define ST_SEND    4u       // the grid that sends to the box
// A held this long: the cursor goes to the next item. A tap must stay well under it: at 100 ms an ordinary tap on a
// controller (often 100-150 ms, and the game samples A once an update) moved the cursor on after its send, so the next
// d-pad press seemed to skip a slot (issue #386)
#define SELECT_NS  300000000ull
#define INITIAL_NS 500000000ull  // from the press to the second item's send (a key's first delay)
#define REPEAT_NS  70000000ull   // between the sends after it

static uint64_t g_a_since, g_next_ns;  // A went down in the grid (0: not held); the next send's time
static bool g_selected;                // the cursor has gone on to the second item

static uint32_t pad(void) { return LD32(LD32(0x588f24u + 0x14b03f8u)); }  // (as 0x588ee8 finds it)

// The slot at (x, y) on the page: its address, 0 when there is no such array
static uint32_t slot_at(uint32_t o, uint32_t mode, uint32_t x, uint32_t y) {
    static const uint8_t off[6] = {0x5c, 0x64, 0x6c, 0x74, 0x5c, 0x5c};
    uint32_t base = mode < 6 ? LD32(o + off[mode]) : 0, cols = LD8(o + 0xda), rows = LD8(o + 0xdb);
    return base ? base + (LD16(o + 0xdc) * rows * cols + y * cols + x) * 4 : 0;
}

// The slot at (x, y) holds an item (false too when there is no array)
static bool filled(uint32_t o, uint32_t mode, uint32_t x, uint32_t y) {
    uint32_t s = slot_at(o, mode, x, y);
    return s && LD16(s);
}

// A once, as the game takes it: A the only button pressed (no stick direction), the cursor where it is. Called again
// within an update
static void press_a(Cpu *c, uint32_t o, uint32_t mode, uint32_t p) {
    uint32_t trg = LD32(p + PAD_TRG), dirs = LD32(p + PAD_DIRS), saved[16];
    for (int i = 0; i < 16; i++) saved[i] = c->r[i];
    ST32(p + PAD_TRG, B_A);
    ST32(p + PAD_DIRS, dirs & ~0xf0u);
    c->r[0] = o, c->r[1] = mode;
    fnhook_call_original(c, FN_REWARDS);
    ST32(p + PAD_TRG, trg);
    ST32(p + PAD_DIRS, dirs);
    for (int i = 0; i < 16; i++) c->r[i] = saved[i];
}

// The cursor on the next filled slot after the one it is on (this page, row by row); false: none
static bool select_next(uint32_t o, uint32_t mode) {
    uint32_t cols = LD8(o + 0xda), rows = LD8(o + 0xdb), here = LD8(o + 0xd9) * cols + LD8(o + 0xd8);
    for (uint32_t i = here + 1; i < cols * rows; i++)
        if (filled(o, mode, i % cols, i / cols)) {
            ST8(o + 0xd8, i % cols), ST8(o + 0xd9, i / cols);
            return true;
        }
    return false;
}

// A held in the grid, once per update after the game's own
static void hold_step(Cpu *c, uint32_t o, uint32_t mode, uint32_t p, uint64_t now) {
    if (!g_selected) {
        if (now - g_a_since < SELECT_NS) return;
        g_selected = true;
        g_next_ns = g_a_since + INITIAL_NS;
        if (!select_next(o, mode)) g_next_ns = ~0ull;  // (nothing more to send)
        return;
    }
    if (now < g_next_ns) return;
    if (filled(o, mode, LD8(o + 0xd8), LD8(o + 0xd9))) press_a(c, o, mode, p);
    if (LD32(o + 0x44) != ST_SEND || !select_next(o, mode)) g_next_ns = ~0ull;
    else g_next_ns = now + REPEAT_NS;
}

static void rewards(Cpu *c) {
    uint32_t o = c->r[0], mode = c->r[1], p = pad();
    uint64_t now = plat_now_ns();
    bool held = config_live_quest_hold_send() && LD32(o + 0x44) == ST_SEND && (LD32(p + PAD_ON) & B_A);
    if (!held) g_a_since = 0;
    else if (!g_a_since && (LD32(p + PAD_TRG) & B_A)) g_a_since = now, g_selected = false;  // (a hold begins with a press in the
                                                                                          // grid: the A that chose Send to the
                                                                                          // Item Box, held on, is none)
    fnhook_call_original(c, FN_REWARDS);
    if (g_a_since && LD32(o + 0x44) == ST_SEND) hold_step(c, o, mode, p, now);
}

void rewardbox_init(void) { fnhook_set(FN_REWARDS, hle_register("rewardbox:update", rewards)); }
