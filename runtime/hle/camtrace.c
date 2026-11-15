// (debug) MHGU_CAMTRACE=1: who takes the hunt camera and who gives it back. For a camera that stays somewhere else after a
// cut-in (reported: a monster smashing a ledge in the Ancestral Steppe's area 7 at high frame rates, the camera left
// looking down at the area for the rest of the hunt). The quest camera (uQuestCamera, move 0x221a90) skips its own
// update while any of these hold it, so the log names the one that never let go:
//   - its two motion camera slots (+0x230 slot 0, +0x2f0 slot 1; 0xc0 bytes: +0 active, +2 stop asked, +0xc type,
//     +0x1c owner, +0x20 frame, +0x28 last frame): play 0x21d4a0, stop 0x21d728 (owner, type) / 0x21d900 (type). A
//     slot plays an animation from a camera list (rCameraList: a monster's em*_cam, the hunter's quest\cam\quest_cam)
//     and holds its last frame until stopped (unless its type ends by itself). Monsters start and stop theirs through
//     0x249dcc / 0x249d94 (from their action commands, 0x29ef3c, and the cut-in sequencer 0x26f3a4 with monster +0xb621
//     the cut, +0xb622 the cuts left; sEnemy +0x2a1e set while one runs), the hunter through 0x4a0698;
//   - the stage camera stack (camera +0x490: +0x39c the views pushed, at most 9): push 0x2162a8 (gimmicks 0x703b50,
//     0x704984, scripts), pop 0x21635c;
//   - the hunter-driven modes: +0xa98 (kind +0xa9c), +0xacc (kind +0xac8, started by 0x229f24), +0x470 (0x229244).
// Each change is logged with the caller, and every second while anything other than the camera's own update holds
// it, a status line (+0x98: what drove the camera this frame, bit 0 its own update).
// MHGU_CAMTRACE_MONTAGE=<s>: also, that many seconds into the first hunt, the zone's first large monster starts its
// cut-in (enemy vfunc +0x1c4) with its area held at the hunter's (else the sequencer ends it at once), for testing.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"

#define FN_CAM_MOVE    0x221a90u  // uQuestCamera move (r0: the camera), every frame in a hunt
#define FN_CAM_PLAY    0x21d4a0u  // (camera, owner, type, r3, [list, index, ...]) -> 1 if it plays
#define FN_CAM_STOP    0x21d728u  // (camera, owner, type, now)
#define FN_CAM_STOPT   0x21d900u  // (camera, type, now)
#define FN_EM_PLAY     0x249dccu  // (enemy, type, target, r3, [list, index, ...])
#define FN_EM_STOP     0x249d94u  // (enemy, type, now)
#define FN_PL_PLAY     0x4a0698u  // (player, cut-in index)
#define FN_SCD_PUSH    0x2162a8u  // (stage camera, view, kind, s0 blend)
#define FN_SCD_POP     0x21635cu  // (stage camera, all, s0 blend)
#define FN_CAM_ACC     0x229f24u  // (camera, kind): the +0xacc mode
#define FN_CAM_470     0x229244u  // (camera): the +0x470 mode
#define G_SENEMY       0x1a51fe0u
#define G_SPLAYER      0x1a85b58u
#define G_GAMECONTROL  0x1a8ca48u
#define EN_INFO        0x1428u
#define OBJ_ZONE       0x1054u

static uint64_t g_t0;
static double g_montage_at = -1;
static uint32_t g_montage_en;

static double secs(void) {
    if (!g_t0) g_t0 = plat_now_ns();
    return (double)(plat_now_ns() - g_t0) / 1e9;
}

static bool large(uint32_t en) {
    uint32_t info = LD32(en + EN_INFO);
    if (!info) return false;
    uint32_t id = LD16(info + 0x1b6);
    return id != 0xffff && id < 10000;
}

static uint32_t local_player(void) {
    uint32_t gc = LD32(G_GAMECONTROL), sp = LD32(G_SPLAYER);
    uint32_t slot = gc ? LD32(gc + 0x30) : 4;
    return sp && slot < 4 ? LD32(sp + 0x1c + 4 * slot) : 0;
}

static float step(void) {
    uint32_t sys = LD32(0x2326764u);
    return sys ? LDF(sys + 0x68) : 0.0f;
}

static void slot_str(char *out, size_t n, uint32_t s) {
    if (!LD8(s)) { snprintf(out, n, "-"); return; }
    snprintf(out, n, "type %u owner %08x (vt %08x) frame %.1f/%.1f%s", LD32(s + 0xc), LD32(s + 0x1c),
             LD32(s + 0x1c) ? LD32(LD32(s + 0x1c)) : 0, LDF(s + 0x20), LDF(s + 0x28), LD8(s + 2) ? " stop" : "");
}

static void montage_test(Cpu *c) {
    uint32_t sen = LD32(G_SENEMY), pl = local_player();
    if (g_montage_at < 0 || !sen || !pl) return;
    if (!g_montage_en && secs() >= g_montage_at) {
        for (int k = 0; k < 10 && !g_montage_en; k++) {
            uint32_t e = LD32(sen + 0x1c + 4u * (uint32_t)k);
            if (e && large(e)) g_montage_en = e;
        }
        if (!g_montage_en) return;
        ST8(g_montage_en + OBJ_ZONE, LD8(pl + OBJ_ZONE));
        LOG("[camtrace] %.2f test: monster %08x starts its cut-in\n", secs(), g_montage_en);
        uint32_t saved[16];
        for (int i = 0; i < 16; i++) saved[i] = c->r[i];
        c->r[0] = g_montage_en, c->r[1] = 0;
        guest_call(c, LD32(LD32(g_montage_en) + 0x1c4));
        for (int i = 0; i < 16; i++) c->r[i] = saved[i];
    } else if (g_montage_en) {
        ST8(g_montage_en + OBJ_ZONE, LD8(pl + OBJ_ZONE));
    }
}

// The breakable ledges (uKowareModel; the gimmick manager *0x1a977a0 lists them at +0x21c, count +0x27c): area +0x182,
// id +0x188, type +0x189, state +0x184 (0x713260), kind +0x185, damage +0x180 against +0x17e, cooldown +0x15c.
// MHGU_CAMTRACE_LEDGE=<area>:<id>:<cmd>:<s>[:<n>]: from s seconds into the hunt, n frames (default 1) of the command
// Diablos sends while it rams a ledge (cmd 0x18 to area 7 id 2: 0xf41778 -> 0x701ec0 -> the ledge's 0x714f9c).
#define G_GIMMICK 0x1a977a0u
#define FN_LEDGE_CMD 0x701ec0u  // (manager, area, id, cmd, [arg])
static int g_ledge_area = -1, g_ledge_id, g_ledge_cmd, g_ledge_n = 1;
static uint32_t g_on_ledge_en;  // (MHGU_CAMTRACE_LEDGE_MONSTER: the monster held on the ledge)

// 0x2a5900 (enemy): if the ledge of its area / sub-area (+0x1054 / +0x1063) is collapsing, it falls with it (status
// +0x91b). For the test the monster held on the ledge reads as being in the ledge's area and sub-area there.
#define FN_EM_LEDGE 0x2a5900u
static void em_ledge(Cpu *c) {
    uint32_t en = c->r[0];
    if (g_on_ledge_en && en == g_on_ledge_en && g_ledge_area >= 0) {
        ST8(en + 0x1054, (uint32_t)g_ledge_area);
        ST8(en + 0x1063, (uint32_t)g_ledge_id);
        // and as in a fight there: active in the hunter's zone (+0x7420), status +0x3ab bit 4 (what 0x297ddc asks of a
        // monster before it falls with the ledge)
        ST8(en + 0x7420, 1);
        uint32_t info = LD32(en + EN_INFO);
        if (info) ST8(info + 0x3ab, LD8(info + 0x3ab) | 0x10);
    }
    fnhook_call_original(c, FN_EM_LEDGE);
    uint32_t info = LD32(en + EN_INFO);
    if (info && LD8(info + 0x91b) && large(en)) {
        static uint32_t said;
        if (said != en)
            LOG("[camtrace] %.2f monster %08x falls with a ledge: to area %u (+91c %u +91d %u +1ba %u +3ab %02x +3ae %u "
                "+5ccc %u +5a0 %u | +7420 %u action %08x)\n", secs(), en, LD8(info + 0x91e), LD8(info + 0x91c),
                LD8(info + 0x91d), LD8(info + 0x1ba), LD8(info + 0x3ab), LD16(info + 0x3ae), LD8(info + 0x5ccc),
                LD8(info + 0x5a0), LD8(en + 0x7420), LD32(en + 0x73e0));
        said = en;
    }
}
static double g_ledge_at;

static void ledges(Cpu *c) {
    uint32_t mgr = LD32(G_GIMMICK);
    if (!mgr) return;
    uint32_t n = LD32(mgr + 0x27c);
    static struct { uint32_t k, sig; } seen[32];
    for (uint32_t i = 0; i < n && i < 32; i++) {
        uint32_t k = LD32(mgr + 0x21c + 4 * i);
        if (!k) continue;
        uint32_t sig = LD8(k + 0x184) | LD8(k + 0x185) << 8 | (uint32_t)LD16(k + 0x180) << 16;
        if (seen[i].k != k || seen[i].sig != sig) {
            LOG("[camtrace] %.2f ledge %08x area %u id %u type %u: state %u kind %u damage %d/%d cooldown %.1f\n", secs(), k,
                LD8(k + 0x182), LD8(k + 0x188), LD8(k + 0x189), LD8(k + 0x184), LD8(k + 0x185), (int16_t)LD16(k + 0x180),
                (int16_t)LD16(k + 0x17e), LDF(k + 0x15c));
            if (seen[i].k != k) {  // its pieces (+0x158: +0x4c units' holders, +0x50 count; holder +8 the unit)
                uint32_t m = LD32(k + 0x158), cnt = m ? LD32(m + 0x50) : 0;
                if (m) LOG("[camtrace]   ledge %08x model %08x at %.0f %.0f %.0f, %u pieces\n", k, m, LDF(m + 0x40),
                           LDF(m + 0x44), LDF(m + 0x48), cnt);
                for (uint32_t p = 0; p < cnt && p < 8; p++) {
                    uint32_t u = LD32(LD32(LD32(m + 0x4c) + 4 * p) + 8);
                    if (u) LOG("[camtrace]   piece %u %08x at %.0f %.0f %.0f\n", p, u, LDF(u + 0x40), LDF(u + 0x44),
                               LDF(u + 0x48));
                }
            }
            seen[i].k = k, seen[i].sig = sig;
        }
    }
    // MHGU_CAMTRACE_LEDGE_HUNTER=1: from then on the hunter's area / sub-area bytes (+0x1054 / +0x1063) read as the
    // ledge's, which the type 6 ledge's cut-in asks for (0x713d00), with the hunter still where it is.
    uint32_t pl = local_player();
    // =2: instead, the hunter (in the ledge's area already) stands on the ledge's spot from 1 s before the command
    // until 0.2 s after, so the game works out its sub-area itself.
    static int pin_mode = -1;
    if (pin_mode < 0) { const char *h = getenv("MHGU_CAMTRACE_LEDGE_HUNTER"); pin_mode = h ? atoi(h) : 0; }
    if (pin_mode == 1 && pl && g_ledge_area >= 0 && secs() >= g_ledge_at - 0.5) {
        ST8(pl + 0x1054, (uint32_t)g_ledge_area);
        ST8(pl + 0x1063, (uint32_t)g_ledge_id);
    }
    if (pin_mode == 2 && pl && g_ledge_area >= 0 && secs() >= g_ledge_at - 1.0 && secs() < g_ledge_at + 0.2) {
        for (uint32_t i = 0; i < n; i++) {
            uint32_t l = LD32(mgr + 0x21c + 4 * i);
            if (!l || LD8(l + 0x182) != (uint32_t)g_ledge_area || LD8(l + 0x188) != (uint32_t)g_ledge_id) continue;
            STF(pl + 0x40, LDF(l + 0x5c)), STF(pl + 0x44, LDF(l + 0x58) + 50.0f), STF(pl + 0x48, LDF(l + 0x60));
            static bool said;
            if (!said) said = true, LOG("[camtrace] %.2f test: hunter onto the ledge (area %u sub %u now)\n", secs(),
                                        LD8(pl + 0x1054), LD8(pl + 0x1063));
        }
    }
    if (pin_mode == 2 && pl && secs() >= g_ledge_at && secs() < g_ledge_at + 0.01)
        LOG("[camtrace] %.2f hunter area %u sub %u at %.0f %.0f %.0f\n", secs(), LD8(pl + 0x1054), LD8(pl + 0x1063),
            LDF(pl + 0x40), LDF(pl + 0x44), LDF(pl + 0x48));
    // MHGU_CAMTRACE_LEDGE_MONSTER=1: for the second before the command, the zone's first large monster stands on the
    // ledge (its position, area and sub-area), as one that rams it from there; when it collapses under a monster
    // (0x2a5900 sees the ledge falling in the monster's area / sub-area) the monster falls with it.
    static int on_ledge = -1;
    if (on_ledge < 0) on_ledge = getenv("MHGU_CAMTRACE_LEDGE_MONSTER") != NULL;
    if (g_on_ledge_en && secs() >= g_ledge_at + 2.0) g_on_ledge_en = 0;
    if (on_ledge && g_ledge_area >= 0 && secs() >= g_ledge_at - 1.0 && secs() < g_ledge_at + 0.05) {
        uint32_t sen = LD32(G_SENEMY), en = 0, k = 0;
        for (int i = 0; sen && i < 10 && !en; i++) {
            uint32_t e = LD32(sen + 0x1c + 4u * (uint32_t)i);
            if (e && large(e)) en = e;
        }
        for (uint32_t i = 0; i < n && !k; i++) {
            uint32_t l = LD32(mgr + 0x21c + 4 * i);
            if (l && LD8(l + 0x182) == (uint32_t)g_ledge_area && LD8(l + 0x188) == (uint32_t)g_ledge_id) k = l;
        }
        if (en && k) {
            static bool said;
            if (!said) {
                said = true;
                LOG("[camtrace] %.2f test: monster %08x onto ledge %08x at %.0f %.0f %.0f\n", secs(), en, k, LDF(k + 0x5c),
                    LDF(k + 0x58), LDF(k + 0x60));
            }
            STF(en + 0x40, LDF(k + 0x5c)), STF(en + 0x44, LDF(k + 0x58)), STF(en + 0x48, LDF(k + 0x60));
            g_on_ledge_en = en;
        }
    }
    // MHGU_CAMTRACE_SCAN=<s>: from s seconds on, the hunter is put on a grid of points around where it stands
    // (+-6000, 400 apart, 6 frames each) and each point's sub-area logged, then put back: where the ledges' sub-areas are.
    static double scan_at = -2;
    if (scan_at == -2) { const char *e = getenv("MHGU_CAMTRACE_SCAN"); scan_at = e ? atof(e) : -1; }
    if (scan_at >= 0 && pl && secs() >= scan_at) {
        static int idx = -1, fr;
        static float x0, y0, z0;
        enum { N = 31 };
        if (idx < 0) idx = 0, fr = 0, x0 = LDF(pl + 0x40), y0 = LDF(pl + 0x44), z0 = LDF(pl + 0x48);
        if (idx < N * N) {
            if (fr == 5) {
                uint32_t sub = LD8(pl + 0x1063);
                if (sub) LOG("[camtrace] scan: area %u sub %u at %.0f %.0f\n", LD8(pl + 0x1054), sub, LDF(pl + 0x40), LDF(pl + 0x48));
                idx++, fr = 0;
            }
            if (idx < N * N) {
                STF(pl + 0x40, x0 + 400.0f * (float)(idx % N - N / 2));
                STF(pl + 0x44, y0 + 300.0f);
                STF(pl + 0x48, z0 + 400.0f * (float)(idx / N - N / 2));
                fr++;
            } else {
                STF(pl + 0x40, x0), STF(pl + 0x44, y0), STF(pl + 0x48, z0);
                LOG("[camtrace] scan done\n");
            }
        }
    }
    // MHGU_CAMTRACE_HUNTER_AT=<x>:<z>:<s>: at s seconds the hunter is put at x, z (300 above where it stands), once.
    static double put_at = -2;
    static float put_x, put_z;
    if (put_at == -2) {
        const char *e = getenv("MHGU_CAMTRACE_HUNTER_AT");
        if (!e || sscanf(e, "%f:%f:%lf", &put_x, &put_z, &put_at) != 3) put_at = -1;
    }
    if (put_at >= 0 && pl && secs() >= put_at) {
        put_at = -1;
        STF(pl + 0x40, put_x), STF(pl + 0x44, LDF(pl + 0x44) + 300.0f), STF(pl + 0x48, put_z);
        LOG("[camtrace] %.2f test: hunter put at %.0f %.0f\n", secs(), put_x, put_z);
    }
    if (pl) {  // the hunter's area and sub-area (the ledges' "id"), as they change
        static uint32_t last = ~0u;
        uint32_t now = LD8(pl + 0x1054) | LD8(pl + 0x1063) << 8;
        if (now != last)
            LOG("[camtrace] %.2f hunter in area %u sub %u at %.0f %.0f %.0f\n", secs(), now & 0xff, now >> 8, LDF(pl + 0x40),
                LDF(pl + 0x44), LDF(pl + 0x48));
        last = now;
    }
    if (g_ledge_area >= 0 && g_ledge_n > 0 && secs() >= g_ledge_at) {
        g_ledge_n--;
        uint32_t saved[16];
        for (int i = 0; i < 16; i++) saved[i] = c->r[i];
        c->r[13] -= 8;
        ST32(c->r[13], 0);
        c->r[0] = mgr, c->r[1] = (uint32_t)g_ledge_area, c->r[2] = (uint32_t)g_ledge_id, c->r[3] = (uint32_t)g_ledge_cmd;
        guest_call(c, FN_LEDGE_CMD);
        for (int i = 0; i < 16; i++) c->r[i] = saved[i];
        if (!g_ledge_n) LOG("[camtrace] %.2f test: ledge commands sent\n", secs());
    }
}

static void cam_move(Cpu *c) {
    uint32_t cam = c->r[0];
    montage_test(c);
    ledges(c);
    fnhook_call_original(c, FN_CAM_MOVE);
    uint32_t sen = LD32(G_SENEMY), scd = cam + 0x490;
    struct { uint32_t a98, a9c, acc, ac8, m470, s0, s1, stack, f4f8; } now = {
        LD8(cam + 0xa98), LD32(cam + 0xa9c), LD8(cam + 0xacc), LD8(cam + 0xac8), LD8(cam + 0x470), LD8(cam + 0x230),
        LD8(cam + 0x2f0), LD32(scd + 0x39c), LD8(cam + 0x4f8) | LD8(cam + 0x4f9) << 8};
    static struct { uint32_t a98, a9c, acc, ac8, m470, s0, s1, stack, f4f8; } last = {~0u};
    static uint64_t last_status;
    uint32_t mask = LD32(cam + 0x98);
    bool changed = memcmp(&now, &last, sizeof now) != 0;
    bool held = now.a98 || now.acc || now.m470 || now.s0 || now.s1 || now.stack || !(mask & 1);
    uint64_t t = plat_now_ns();
    if (changed || (held && t - last_status > 1000000000ull)) {
        last_status = t;
        char s0[96], s1[96];
        slot_str(s0, sizeof s0, cam + 0x230);
        slot_str(s1, sizeof s1, cam + 0x2f0);
        LOG("[camtrace] %.2f camera %08x drive %03x | slot0 %s | slot1 %s | stage views %u (%u/%u) | a98 %u:%u acc %u:%u "
            "470 %u | sEnemy cut-in %u | step %.3f\n",
            secs(), cam, mask & 0xfff, s0, s1, now.stack, now.f4f8 & 0xff, now.f4f8 >> 8, now.a98, now.a9c, now.acc, now.ac8,
            now.m470, sen ? LD8(sen + 0x2a1e) : 0, step());
        memcpy(&last, &now, sizeof now);
    }
}

static void cam_play(Cpu *c) {
    uint32_t owner = c->r[1], type = c->r[2], list = LD32(c->r[13]), index = LD32(c->r[13] + 4), lr = c->r[14];
    fnhook_call_original(c, FN_CAM_PLAY);
    LOG("[camtrace] %.2f play type %u owner %08x list %08x cut %u -> %s (lr %08x)\n", secs(), type, owner, list, index,
        c->r[0] ? "playing" : "refused", lr);
}

static void cam_stop(Cpu *c) {
    LOG("[camtrace] %.2f stop type %u owner %08x%s (lr %08x)\n", secs(), c->r[2], c->r[1], c->r[3] ? " now" : "", c->r[14]);
    fnhook_call_original(c, FN_CAM_STOP);
}

static void cam_stop_type(Cpu *c) {
    LOG("[camtrace] %.2f stop type %u, any owner%s (lr %08x)\n", secs(), c->r[1], c->r[2] ? " now" : "", c->r[14]);
    fnhook_call_original(c, FN_CAM_STOPT);
}

static void em_play(Cpu *c) {
    uint32_t en = c->r[0];
    LOG("[camtrace] %.2f monster %08x (vt %08x, action %08x, cut-in %u left %d) plays type %u cut %u (lr %08x)\n", secs(), en,
        LD32(en), LD32(en + 0x73e0), LD8(en + 0xb621), (int8_t)LD8(en + 0xb622), c->r[1], LD32(c->r[13] + 4), c->r[14]);
    fnhook_call_original(c, FN_EM_PLAY);
}

static void em_stop(Cpu *c) {
    uint32_t en = c->r[0];
    LOG("[camtrace] %.2f monster %08x (action %08x) stops type %u%s (lr %08x)\n", secs(), en, LD32(en + 0x73e0), c->r[1],
        c->r[2] ? " now" : "", c->r[14]);
    fnhook_call_original(c, FN_EM_STOP);
}

static void pl_play(Cpu *c) {
    LOG("[camtrace] %.2f hunter %08x plays cut-in %u (lr %08x)\n", secs(), c->r[0], c->r[1], c->r[14]);
    fnhook_call_original(c, FN_PL_PLAY);
}

static void scd_push(Cpu *c) {
    uint32_t scd = c->r[0], lr = c->r[14], kind = c->r[2];
    float blend = c->v.f[0];
    fnhook_call_original(c, FN_SCD_PUSH);
    LOG("[camtrace] %.2f stage view pushed (kind %u, blend %.1f) -> %u views%s (lr %08x)\n", secs(), kind, (double)blend,
        LD32(scd + 0x39c), c->r[0] ? "" : " (refused)", lr);
}

static void scd_pop(Cpu *c) {
    uint32_t scd = c->r[0], lr = c->r[14], all = c->r[1];
    fnhook_call_original(c, FN_SCD_POP);
    LOG("[camtrace] %.2f stage view popped%s -> %u views (lr %08x)\n", secs(), all == 1 ? " (all)" : "", LD32(scd + 0x39c), lr);
}

static void cam_acc(Cpu *c) {
    LOG("[camtrace] %.2f camera mode acc kind %u asked (lr %08x)\n", secs(), c->r[1], c->r[14]);
    fnhook_call_original(c, FN_CAM_ACC);
}

static void cam_470(Cpu *c) {
    LOG("[camtrace] %.2f camera mode 470 asked (lr %08x)\n", secs(), c->r[14]);
    fnhook_call_original(c, FN_CAM_470);
}

// The area loader (0x20fc1c, r0 its object, state +0xdc) and sGameControl's area bytes (+0x48 stage, +0x49 area,
// +0x4a.. +0x4f), logged as they change. MHGU_CAMTRACE_START_AREA=<n>: the hunt's first area load goes to area n.
#define FN_AREA_LOAD 0x20fc1cu
static int g_start_area = -1;

static void area_load(Cpu *c) {
    uint32_t obj = c->r[0], gc = LD32(G_GAMECONTROL);
    static uint32_t last_state = ~0u, last_bytes;
    uint32_t st = LD32(obj + 0xdc), bytes = gc ? LD32(gc + 0x48) : 0, more = gc ? LD32(gc + 0x4c) : 0;
    if (st != last_state || bytes != last_bytes)
        LOG("[camtrace] %.2f area loader %08x state %u | stage %u area %u +4a %02x +4b %02x +4c %02x +4d %02x +4e %02x +4f %02x\n",
            secs(), obj, st, bytes & 0xff, (bytes >> 8) & 0xff, (bytes >> 16) & 0xff, bytes >> 24, more & 0xff,
            (more >> 8) & 0xff, (more >> 16) & 0xff, more >> 24);
    last_state = st, last_bytes = bytes;
    fnhook_call_original(c, FN_AREA_LOAD);
}

// The hunt's first area: the town's move 0x8bdfd0 (r0 the town area, +0xcc its kind: 2 departing on a quest) copies
// town +0xc9 (the start area the quest's random start picked) to sGameControl +0x49.
#define FN_TOWN_MOVE 0x8bdfd0u
static void town_move(Cpu *c) {
    uint32_t t = c->r[0];
    if (LD32(t + 0xcc) == 2) {
        LOG("[camtrace] %.2f departing: start area %u%s\n", secs(), LD8(t + 0xc9), g_start_area >= 0 ? ", replaced" : "");
        if (g_start_area >= 0) ST8(t + 0xc9, (uint32_t)g_start_area);
    }
    fnhook_call_original(c, FN_TOWN_MOVE);
}

void camtrace_init(void) {
    const char *e = getenv("MHGU_CAMTRACE");
    if (!e || *e != '1') return;
    const char *m = getenv("MHGU_CAMTRACE_MONTAGE");
    if (m && *m) g_montage_at = atof(m);
    const char *l = getenv("MHGU_CAMTRACE_LEDGE");
    if (l && *l && sscanf(l, "%d:%d:%i:%lf:%d", &g_ledge_area, &g_ledge_id, &g_ledge_cmd, &g_ledge_at, &g_ledge_n) < 4)
        g_ledge_area = -1;
    const char *sa = getenv("MHGU_CAMTRACE_START_AREA");
    if (sa && *sa) g_start_area = atoi(sa);
    fnhook_set(FN_AREA_LOAD, hle_register("camtrace:area_load", area_load));
    fnhook_set(FN_EM_LEDGE, hle_register("camtrace:em_ledge", em_ledge));
    fnhook_set(FN_TOWN_MOVE, hle_register("camtrace:town_move", town_move));
    fnhook_set(FN_CAM_MOVE, hle_register("camtrace:cam_move", cam_move));
    fnhook_set(FN_CAM_PLAY, hle_register("camtrace:cam_play", cam_play));
    fnhook_set(FN_CAM_STOP, hle_register("camtrace:cam_stop", cam_stop));
    fnhook_set(FN_CAM_STOPT, hle_register("camtrace:cam_stop_type", cam_stop_type));
    fnhook_set(FN_EM_PLAY, hle_register("camtrace:em_play", em_play));
    fnhook_set(FN_EM_STOP, hle_register("camtrace:em_stop", em_stop));
    fnhook_set(FN_PL_PLAY, hle_register("camtrace:pl_play", pl_play));
    fnhook_set(FN_SCD_PUSH, hle_register("camtrace:scd_push", scd_push));
    fnhook_set(FN_SCD_POP, hle_register("camtrace:scd_pop", scd_pop));
    fnhook_set(FN_CAM_ACC, hle_register("camtrace:cam_acc", cam_acc));
    fnhook_set(FN_CAM_470, hle_register("camtrace:cam_470", cam_470));
    LOG("[camtrace] tracing the hunt camera's takeovers%s\n", g_montage_at >= 0 ? " (with a test cut-in)" : "");
}
