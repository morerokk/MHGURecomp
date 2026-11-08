// Two-button inputs (the Hunting Horn's X+A, the Lance's X+A+R...) at high frame rates. The player's input update
// (0x47ca28, every weapon) holds a press for a moment to see whether another button joins it: it sets bit 0 of the
// player's input bits (+0x1444) and a countdown (+0x1448 = 2.0, 0x47d558), takes 1.0 off the countdown each update
// (0x47ca84: `vmov.f32 s2, #-1.0`, then `vadd`) and gives the press on its own when it reaches 0. Two updates: 67 ms at
// the Switch's 30 updates a second, 17 ms at 120 fps, 8 ms at 240, so both buttons had to go down almost together.
// The patched constant takes s = step / 2 updates off instead (step: the engine's frame step in 60 fps frames, s = 1
// at 30 fps; at most 1, below 30 fps as the original): the window lasts 67 ms at any frame rate (tools/gen.py PATCHES,
// "Two-button input window"). [Controls] ComboInputWindow = Strict keeps 1.0 per update.
//
// Three buttons (R+X+A: the Lance's charge, the Gunlance's Wyvern's Fire) at high frame rates. The window ends on the
// second press and resolves what it has (0x47cb44): the third button had to come in that same update, 33 ms wide on
// the Switch and 7 ms at 150 fps, or R+X (a guard thrust) came out. The longer window made it worse: R pressed just
// before X and A used to be alone and held by then. And a mod that reports held buttons as pressed every update (Better
// Inputs, for its hold-to-repeat: the pad's new-press word, 0x5de064) ended every window after one update, and turned
// R held for a while + X into R+X at once. So for the local player, between 30 and any higher frame rate, in Lenient:
// combowin_begin (0x47ca38) gives the code that decides (up to 0x47cc58, combowin_end puts the word back) a corrected
// new-press word: R / ZR is new only when it really went down, a running window sees only real presses (a held button
// repeats once per window: 67 ms, as on the Switch), and a press that makes two of R, X and A, with the third not
// held, waits for it, one 30 fps update at most and with the window stopped. A lone press isn't delayed.
#include "../config.h"
#include "../hle.h"

#define G_SYSTEM  0x2326764u  // engine system object *: +0x68 the frame step in 60 fps frames
#define G_OPTIONS 0x1a97f78u  // options object *: +0x446e 0: R is the modifier button, else ZR (0x85f93c)
#define PL_LOCAL  0x13ccu     // player: bit 0 the local player's
#define PL_HELD   0x13f4u     // player: u16 buttons held
#define PL_NEW    0x13f8u     // player: u16 buttons newly pressed
#define PL_FLAGS  0x1444u     // player: bit 0 a window is running, bits 13+ the presses it holds
#define PAD_R     0x40u
#define PAD_ZR    0x800u
#define PAD_FACE  0x1b0u      // A, B, X, Y
#define PAD_XA    0x120u
#define WAIT_STEP 2.0f        // the longest a press waits for the third button: one 30 fps update, in 60 fps frames

static _Thread_local bool g_stop;      // this update's window doesn't count down
static _Thread_local uint32_t g_pl;    // the player whose new-press word is changed until combowin_end ...
static _Thread_local uint16_t g_word;  // ... and the word

void combowin_begin(uint32_t pl) {
    static uint32_t last_pl;
    static uint16_t last_held, waiting;  // (waiting: presses kept from the code that decides)
    static float waited;
    if (!(LD8(pl + PL_LOCAL) & 1)) return;
    uint32_t sys = LD32(G_SYSTEM);
    float step = sys ? LDF(sys + 0x68) : 0.0f;
    uint16_t held = LD16(pl + PL_HELD);
    if (pl != last_pl || config_live_combo_window() == COMBO_STRICT || !(step > 0.0f && step < 2.0f)) {
        last_pl = pl, last_held = held, waiting = 0, waited = 0.0f;
        return;
    }
    uint32_t opt = LD32(G_OPTIONS);
    uint16_t mod = opt && LD8(opt + 0x446e) ? PAD_ZR : PAD_R;
    uint16_t attack = mod | PAD_FACE, three = mod | PAD_XA;
    uint16_t word = LD16(pl + PL_NEW);
    uint32_t flags = LD32(pl + PL_FLAGS);
    uint16_t real = word & ~last_held;
    last_held = held;
    bool window = flags & 1;
    uint16_t saved = window ? (flags >> 13) & 0xfff : 0;
    uint16_t press = word & attack & (real | (uint16_t)~mod);
    if (window) press &= real;
    if (waiting) {
        waiting |= real & three;
        waited += step;
        if (((held | saved | waiting) & three) == three || (real & attack & ~three) || (waiting & ~held) || waited >= WAIT_STEP) {
            press |= waiting;
            waiting = 0, waited = 0.0f;
        } else {
            press = 0;
            g_stop = true;
        }
    } else {
        uint16_t got = (saved | press) & three;
        if (__builtin_popcount(got) == 2 && (press & three & real) && !(press & ~three) && !(three & ~got & held)) {
            waiting = press;
            press = 0;
            g_stop = true;
        }
    }
    g_pl = pl, g_word = word;
    ST16(pl + PL_NEW, (word & ~attack) | press);
}

void combowin_end(uint32_t pl) {
    if (g_pl == pl) ST16(pl + PL_NEW, g_word);
    g_pl = 0, g_stop = false;
}

float combowin_tick(void) {
    if (g_stop) return 0.0f;
    if (config_live_combo_window() == COMBO_STRICT) return 1.0f;
    uint32_t sys = LD32(G_SYSTEM);
    float s = sys ? LDF(sys + 0x68) * 0.5f : 1.0f;
    return s > 0.0f && s < 1.0f ? s : 1.0f;
}
