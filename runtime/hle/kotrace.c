// (debug) MHGU_KOTRACE=1: logs every KO (stun) event of the large monsters, for comparing frame rates. Each line has
// the time since the first event, the frame step (+0x68 of the engine's system object: 2.0 at 30 fps) and the
// monster. The monster's status block is *(monster + 0x1428); its stun fields (decay: 0x2a398c, the KO itself:
// 0x2a2e18 / 0x2a2f0c, the threshold after it: 0x2a2f80):
//   +0x4d2 s16  KO the hits of this update brought (0x29db54: hit KO x part multiplier / 100, at least 1)
//   +0x5d76 s16 buildup         +0x5d78 s16 threshold       +0x5d7c f32 decay timer (60 fps frames)
//   +0x5d80 u8  KOs so far      +0x5e08 bit 0x10 stunned    +0x5e0c f32 time left stunned
//   +0x3a2..+0x3a4 u8 updates left in which the KO is held back at the threshold (also when vtable +0x39c says 1)
#include <stdlib.h>

#include "../hle.h"

#define FN_KO_HIT    0x29db54u  // (monster, ?, part, KO value): a hit's KO into +0x4d2
#define FN_KO_APPLY  0x2a2e18u  // (monster, amount): into the buildup; 1 when the KO starts
#define FN_KO_START  0x2a2f0cu  // (monster): stunned for the entry's duration
#define FN_KO_RAISE  0x2a2f80u  // (monster): the threshold for the next KO
#define FN_DECAY     0x2a398cu  // (monster): every status's decay, each update
#define G_SYSTEM     0x2326764u
#define EN_INFO      0x1428u

static uint64_t g_t0;
static struct { uint32_t en; int stunned; uint64_t since; } g_mon[10];

static uint32_t info_of(uint32_t en) { return LD32(en + EN_INFO); }
static bool large(uint32_t en) {
    uint32_t info = info_of(en);
    if (!info) return false;
    uint32_t id = LD16(info + 0x1b6);
    return id != 0xffff && id < 10000;
}
static double secs(void) {
    uint64_t now = plat_now_ns();
    if (!g_t0) g_t0 = now;
    return (double)(now - g_t0) / 1e9;
}
static float step(void) {
    uint32_t sys = LD32(G_SYSTEM);
    return sys ? LDF(sys + 0x68) : 0;
}
// (own: 1 when this game runs the monster's AI, +0x7420: the owner's buildup is the one netEmAction sends to the others)
#define KLOG(en, fmt, ...) LOG("[ko] %8.2f s step %.3f %08x own %u " fmt "\n", secs(), step(), en, LD8((en) + 0x7420u), __VA_ARGS__)

static void ko_hit(Cpu *c) {
    uint32_t en = c->r[0], type = c->r[1], part = c->r[2], value = c->r[3];
    uint32_t info = large(en) ? info_of(en) : 0;
    int16_t before = info ? (int16_t)LD16(info + 0x4d2) : 0;
    fnhook_call_original(c, FN_KO_HIT);
    if (info && value)
        KLOG(en, "hit: KO %u on part %u (type %u) -> pending %d (+%d)", value, part, type, (int16_t)LD16(info + 0x4d2),
             (int16_t)LD16(info + 0x4d2) - before);
}

static void ko_apply(Cpu *c) {
    uint32_t en = c->r[0];
    int amount = (int16_t)c->r[1];
    uint32_t info = amount && large(en) ? info_of(en) : 0;
    if (!info) { fnhook_call_original(c, FN_KO_APPLY); return; }
    int16_t b0 = (int16_t)LD16(info + 0x5d76), th = (int16_t)LD16(info + 0x5d78);
    bool stunned = LD8(info + 0x5e08) & 0x10;
    uint8_t f0 = LD8(info + 0x3a2), f1 = LD8(info + 0x3a3), f2 = LD8(info + 0x3a4);
    fnhook_call_original(c, FN_KO_APPLY);
    int16_t b1 = (int16_t)LD16(info + 0x5d76);
    if (stunned) KLOG(en, "apply %d: ignored, already stunned (buildup %d / %d)", amount, b0, th);
    else if (c->r[0] == 1) KLOG(en, "apply %d: buildup %d -> %d / %d: KO", amount, b0, b0 + amount, th);
    else if (b0 + amount >= th) KLOG(en, "apply %d: buildup %d -> %d / %d: HELD BACK at the threshold (flags %u %u %u)", amount, b0, b1, th, f0, f1, f2);
    else KLOG(en, "apply %d: buildup %d -> %d / %d", amount, b0, b1, th);
}

static void ko_start(Cpu *c) {
    uint32_t en = c->r[0];
    fnhook_call_original(c, FN_KO_START);
    if (!large(en)) return;
    uint32_t info = info_of(en);
    KLOG(en, "KO started: %.0f frames (60 fps) stunned, KO count %u", LDF(info + 0x5e0c), LD8(info + 0x5d80));
}

static void ko_raise(Cpu *c) {
    uint32_t en = c->r[0];
    uint32_t info = large(en) ? info_of(en) : 0;
    int16_t th = info ? (int16_t)LD16(info + 0x5d78) : 0;
    fnhook_call_original(c, FN_KO_RAISE);
    if (info) KLOG(en, "threshold %d -> %d (KO count %u)", th, (int16_t)LD16(info + 0x5d78), LD8(info + 0x5d80));
}

static void decay(Cpu *c) {
    uint32_t en = c->r[0];
    uint32_t info = large(en) ? info_of(en) : 0;
    int16_t b0 = info ? (int16_t)LD16(info + 0x5d76) : 0;
    fnhook_call_original(c, FN_DECAY);
    if (!info) return;
    int16_t b1 = (int16_t)LD16(info + 0x5d76);
    if (b1 < b0) KLOG(en, "decay: buildup %d -> %d / %d", b0, b1, (int16_t)LD16(info + 0x5d78));
    int s = 0;
    while (s < 10 && g_mon[s].en && g_mon[s].en != en) s++;
    if (s == 10) return;
    g_mon[s].en = en;
    int stunned = (LD8(info + 0x5e08) & 0x10) != 0;
    if (stunned && !g_mon[s].stunned) g_mon[s].since = plat_now_ns();
    if (!stunned && g_mon[s].stunned)
        KLOG(en, "KO ended after %.1f s", (double)(plat_now_ns() - g_mon[s].since) / 1e9);
    g_mon[s].stunned = stunned;
}

void kotrace_init(void) {
    const char *e = getenv("MHGU_KOTRACE");
    if (!e || !*e || !atoi(e)) return;
    fnhook_set(FN_KO_HIT, hle_register("kotrace:hit", ko_hit));
    fnhook_set(FN_KO_APPLY, hle_register("kotrace:apply", ko_apply));
    fnhook_set(FN_KO_START, hle_register("kotrace:start", ko_start));
    fnhook_set(FN_KO_RAISE, hle_register("kotrace:raise", ko_raise));
    fnhook_set(FN_DECAY, hle_register("kotrace:decay", decay));
    LOG("[ko] tracing KO (stun) events of large monsters\n");
}
