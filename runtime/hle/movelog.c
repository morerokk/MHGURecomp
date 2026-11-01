// (debug) MHGU_LOG_MOVES=1: logs the player's actions and command table changes, for mapping a weapon's moves to its
// tables.
// A move picks the table of follow-up commands (input -> next action, frame window) it allows; the tables are the
// per-style rPlCmdTblList in arc\player\quest\w<weapon>_m<style>.arc (and com_m<style>.arc for the common moves).
// The game sets one through two functions (player, id, start frame[, mode]): id 1000 + n is common table n, 2000 + n
// the weapon's table n, 0 and 9999 none. The player keeps the id at +0x24ac and the table at +0x24a8.
// Actions (act_st, act_no, act_param, as in the commands' rPlBaseCmd: act_st 0x0001 common, 0x0010 weapon; bit 15
// set on some) start through the player's vtable +0x2f8 (0x4ab670, "do"; it first lets the weapon class remap them,
// vtable +0x718), which the command handling calls for a matched command, or 0x4831e0 ("set", e.g. back to idle);
// both logged.
// MHGU_CONTROL's "plact <act_st hex> <act_no> [param]" starts that action on the player (through "do"), and
// "plsweep <act_st hex> <first> <last>" each in turn, logging how long the player stays in it (for finding one).
// Every change of the player's action is logged too ("is"), also those started some other way than "do" / "set".
// MHGU_PLSNAP=<file>: the player's memory (PLSNAP_BYTES) every update for PLSNAP_UPDATES updates from a knockback
// (1:85 / 1:86) or the control file's "plsnap [updates]", appended to the file: per update a header {"PSNP", frame,
// step (f32, 60 fps frames), player} and the bytes (for finding a status timer by comparing the updates).
#include <stdlib.h>

#include "../hle.h"

#define FN_SET_CMDTBL   0x4e9b7cu  // (player, id, start frame)
#define FN_SET_CMDTBL2  0x4e9c50u  // (player, id, start frame, mode)
#define FN_SET_ACTION   0x4831e0u  // (player, act_st, act_no, act_param)
#define FN_DO_ACTION    0x4ab670u  // (player, act_st, act_no, act_param)
#define FN_PL_INPUT     0x47ca28u  // (player): its input update, every update
#define PL_ACT_ST       0x2506u    // player: the current action (u16 act_st, act_no)
#define PL_ACT_NO       0x250au
#define G_SYSTEM        0x2326764u  // engine system object *: +0x68 the frame step in 60 fps frames
#define PLSNAP_BYTES    0x3800u
#define PLSNAP_UPDATES  900

static FILE *g_snap;
static volatile int g_snap_req;
static int g_snap_left;

void movelog_snap(int updates) {
    __atomic_store_n(&g_snap_req, updates > 0 ? updates : PLSNAP_UPDATES, __ATOMIC_RELEASE);
}

static volatile int g_req;  // plact / plsweep from the control file
static uint32_t g_req_st, g_req_no, g_req_param;
static volatile int g_sweep;
static uint32_t g_sweep_st, g_sweep_next, g_sweep_last, g_sweep_cur = ~0u;
static uint64_t g_sweep_start_ns, g_sweep_wait_ns;

void movelog_request(uint32_t st, uint32_t no, uint32_t param) {
    g_req_st = st, g_req_no = no, g_req_param = param;
    __atomic_store_n(&g_req, 1, __ATOMIC_RELEASE);
}

void movelog_sweep(uint32_t st, uint32_t first, uint32_t last) {
    g_sweep_st = st, g_sweep_next = first, g_sweep_last = last, g_sweep_cur = ~0u, g_sweep_wait_ns = 0;
    __atomic_store_n(&g_sweep, 1, __ATOMIC_RELEASE);
}

static void start_action(Cpu *c, uint32_t pl, uint32_t st, uint32_t no, uint32_t param) {
    uint32_t saved[16];
    for (int i = 0; i < 16; i++) saved[i] = c->r[i];
    c->r[0] = pl, c->r[1] = st, c->r[2] = no, c->r[3] = param;
    guest_call(c, FN_DO_ACTION);
    for (int i = 0; i < 16; i++) c->r[i] = saved[i];
}

static void pl_input(Cpu *c) {
    uint32_t pl = c->r[0];
    uint64_t now = plat_now_ns();
    static uint32_t last_act = ~0u;
    uint32_t act = (uint32_t)LD16(pl + PL_ACT_ST) << 16 | LD16(pl + PL_ACT_NO);
    if (act != last_act) {
        LOG("[moves] %llu pl %08x is %04x:%03u (%.3f s)\n", (unsigned long long)g_present_count, pl, act >> 16,
            act & 0xffff, now / 1e9);
        last_act = act;
    }
    static uint32_t last_rounds = ~0u;  // the stun (0x40:026): its rounds left (byte 7 of +4; runtime/hle/mashrate.c)
    uint32_t rounds = act == 0x40001au ? LD8(pl + 7) : ~0u;
    if (rounds != last_rounds && rounds != ~0u)
        LOG("[moves] %llu pl %08x stun: %u round(s) left (%.3f s)\n", (unsigned long long)g_present_count, pl, rounds,
            now / 1e9);
    last_rounds = rounds;
    int req = __atomic_exchange_n(&g_snap_req, 0, __ATOMIC_ACQ_REL);
    if (req && g_snap && g_snap_left <= 0) {
        g_snap_left = req;
        LOG("[moves] %llu pl %08x: %d snapshots\n", (unsigned long long)g_present_count, pl, req);
    }
    if (g_snap && g_snap_left > 0) {
        uint32_t sys = LD32(G_SYSTEM), frame = (uint32_t)g_present_count;
        float step = sys ? LDF(sys + 0x68) : 0;
        fwrite("PSNP", 1, 4, g_snap), fwrite(&frame, 4, 1, g_snap), fwrite(&step, 4, 1, g_snap), fwrite(&pl, 4, 1, g_snap);
        fwrite(G2H(pl), 1, PLSNAP_BYTES, g_snap);
        if (--g_snap_left == 0) fflush(g_snap);
    }
    if (__atomic_load_n(&g_sweep, __ATOMIC_ACQUIRE)) {
        uint32_t st = LD16(pl + PL_ACT_ST), no = LD16(pl + PL_ACT_NO);
        if (g_sweep_cur != ~0u && (st != g_sweep_st || no != g_sweep_cur || now - g_sweep_start_ns > 15000000000ull)) {
            LOG("[moves] sweep %04x:%03u lasted %.2f s%s\n", g_sweep_st, g_sweep_cur, (now - g_sweep_start_ns) / 1e9,
                st == g_sweep_st && no == g_sweep_cur ? " (still in it)" : "");
            g_sweep_cur = ~0u;
            g_sweep_wait_ns = now + 1500000000ull;
        } else if (g_sweep_cur == ~0u && now >= g_sweep_wait_ns) {
            if (g_sweep_next > g_sweep_last) {
                g_sweep = 0;
                LOG("[moves] sweep done\n");
            } else {
                g_sweep_cur = g_sweep_next++;
                g_sweep_start_ns = now;
                start_action(c, pl, g_sweep_st, g_sweep_cur, 0);
            }
        }
    }
    if (__atomic_load_n(&g_req, __ATOMIC_ACQUIRE)) {
        g_req = 0;
        LOG("[moves] plact %08x %04x:%03u param %u\n", pl, g_req_st, g_req_no, g_req_param);
        start_action(c, pl, g_req_st, g_req_no, g_req_param);
    }
    fnhook_call_original(c, FN_PL_INPUT);
}

static void log_table(uint32_t pl, uint32_t id, uint32_t frame) {
    static uint32_t last_pl, last_id = ~0u;
    if (pl == last_pl && id == last_id) return;
    last_pl = pl, last_id = id;
    if (id >= 2000 && id != 9999) LOG("[moves] %llu pl %08x weapon table %u (frame %u)\n", (unsigned long long)g_present_count, pl, id - 2000, frame);
    else if (id >= 1000 && id != 9999) LOG("[moves] %llu pl %08x common table %u (frame %u)\n", (unsigned long long)g_present_count, pl, id - 1000, frame);
    else LOG("[moves] %llu pl %08x no table (%u)\n", (unsigned long long)g_present_count, pl, id);
}

static void set_cmdtbl(Cpu *c) {
    log_table(c->r[0], c->r[1], c->r[2]);
    fnhook_call_original(c, FN_SET_CMDTBL);
}

static void log_action(Cpu *c, const char *how) {
    LOG("[moves] %llu pl %08x %s %04x:%03u param %u\n", (unsigned long long)g_present_count, c->r[0], how,
        c->r[1] & 0xffff, c->r[2] & 0xffff, c->r[3] & 0xffff);
}

static void set_action(Cpu *c) {
    log_action(c, "set");
    fnhook_call_original(c, FN_SET_ACTION);
}

static void do_action(Cpu *c) {
    log_action(c, "do");
    if ((c->r[1] & 0xffff) == 1 && ((c->r[2] & 0xffff) == 85 || (c->r[2] & 0xffff) == 86)) movelog_snap(0);
    fnhook_call_original(c, FN_DO_ACTION);
}

static void set_cmdtbl2(Cpu *c) {
    log_table(c->r[0], c->r[1], c->r[2]);
    fnhook_call_original(c, FN_SET_CMDTBL2);
}

void movelog_init(void) {
    const char *e = getenv("MHGU_LOG_MOVES");
    if (!e || !*e || *e == '0') return;
    fnhook_set(FN_SET_CMDTBL, hle_register("movelog:set_cmdtbl", set_cmdtbl));
    fnhook_set(FN_SET_CMDTBL2, hle_register("movelog:set_cmdtbl2", set_cmdtbl2));
    fnhook_set(FN_SET_ACTION, hle_register("movelog:set_action", set_action));
    fnhook_set(FN_DO_ACTION, hle_register("movelog:do_action", do_action));
    fnhook_set(FN_PL_INPUT, hle_register("movelog:pl_input", pl_input));
    const char *snap = getenv("MHGU_PLSNAP");
    if (snap && *snap && !(g_snap = fopen(snap, "ab"))) LOG("[moves] can't open %s\n", snap);
    LOG("[moves] logging command table changes\n");
}
