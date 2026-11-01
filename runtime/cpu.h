// Guest CPU state (AArch32) and helpers used by recompiled code.
//
// Guest memory is a 4 GB window at GUEST_BASE: guest address a lives at host address GUEST_BASE + a. Recompiled
// code reaches it through the LD / ST macros only, so the guest's 32-bit address arithmetic wraps exactly as on
// hardware and nothing outside the window can be touched.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __ANDROID__
#define GUEST_BASE 0x1000000000ull   // 64 GB: most Android kernels give a process 512 GB (39-bit addresses), and map
                                     // its libraries and heap near the top of that
#else
#define GUEST_BASE 0x10000000000ull  // 1 TB: claimed at startup (runtime/main.c)
#endif

typedef struct __attribute__((aligned(16))) Cpu {
    uint32_t r[16];          // 0: r0-r14 (r13 sp, r14 lr); r[15] unused (pc reads are constants)
    uint32_t pc;             // 64: target of an indirect branch, for rt_dispatch
    uint32_t nf, zf, cf, vf; // 68: APSR flags, each 0 or 1
    uint32_t qf;             // 84: sticky saturation flag
    uint32_t ge;             // 88: GE[3:0] (parallel add/sub, SEL)
    uint32_t fpscr;          // 92: guest FPSCR (rounding mode, FZ, DN, cumulative flags; NZCV of the last VCMP)
    uint32_t tpidruro;       // 96: TPIDRURO (Horizon: the thread's TLS block)
    uint32_t tpidrurw;       // 100: TPIDRURW (user read/write thread register)
    uint32_t excl_addr;      // 104: exclusive monitor address (~0: open)
    uint32_t _pad0;
    uint64_t excl_val;       // 112: value loaded by the last exclusive load
    void *thread;            // 120: runtime thread object
    uint8_t _pad[256 - 128];
    union {                  // 256: VFP / Advanced SIMD register file: s0-s31 overlay d0-d15, q0-q15 = d pairs
        uint64_t d[32];
        uint32_t s[64];
        float f[64];
        double df[32];
        uint8_t b[256];
        uint16_t h[128];
    } v;
} Cpu;

_Static_assert(offsetof(Cpu, pc) == 64, "pc");
_Static_assert(offsetof(Cpu, nf) == 68, "nf");
_Static_assert(offsetof(Cpu, tpidruro) == 96, "tpidruro");
_Static_assert(offsetof(Cpu, v) == 256, "v");

typedef void (*GuestFn)(Cpu *);

// ---- runtime services (runtime/*.c)
void rt_dispatch(Cpu *c);                 // call the function at c->pc
void rt_svc(Cpu *c, uint32_t imm, uint32_t pc);
void rt_bkpt(Cpu *c, uint32_t imm, uint32_t pc) __attribute__((noreturn));
void rt_udf(Cpu *c, uint32_t pc) __attribute__((noreturn));
void rt_thumb(Cpu *c, uint32_t pc) __attribute__((noreturn));
uint64_t rt_ticks(void);

// ---- memory
typedef uint16_t __attribute__((aligned(1), may_alias)) u16u;
typedef uint32_t __attribute__((aligned(1), may_alias)) u32u;
typedef uint64_t __attribute__((aligned(1), may_alias)) u64u;
typedef float __attribute__((aligned(1), may_alias)) f32u;
typedef double __attribute__((aligned(1), may_alias)) f64u;
typedef uint8_t __attribute__((may_alias)) u8a;

#define G2H(a) ((void *)(uintptr_t)(GUEST_BASE + (uint32_t)(a)))   // guest address -> host pointer
#define H2G(p) ((uint32_t)((uintptr_t)(p) - GUEST_BASE))          // host pointer into the window -> guest address

#define LD8(a)  (*(const u8a *)G2H(a))
#define LD16(a) (*(const u16u *)G2H(a))
#define LD32(a) (*(const u32u *)G2H(a))
#define LD64(a) (*(const u64u *)G2H(a))
#define LDF(a)  (*(const f32u *)G2H(a))
#define LDD(a)  (*(const f64u *)G2H(a))
#define ST8(a, x)  (*(u8a *)G2H(a) = (uint8_t)(x))
#define ST16(a, x) (*(u16u *)G2H(a) = (uint16_t)(x))
#define ST32(a, x) (*(u32u *)G2H(a) = (uint32_t)(x))
#define ST64(a, x) (*(u64u *)G2H(a) = (uint64_t)(x))
#define STF(a, x)  (*(f32u *)G2H(a) = (x))
#define STD(a, x)  (*(f64u *)G2H(a) = (x))

// Function-entry profiler (runtime/fnprof.c, MHGU_FNPROF): one predictable branch per call when off.
extern volatile int g_fnprof;
void fnprof_hit(uint32_t addr, uint32_t lr, uint32_t r0);
// Function hooks (runtime/dispatch.c, fnhook_set): a bit per word of the code region below 0x03000000, set at the
// entry of a hooked function; fnhook_enter runs the hook and returns 1 when the function itself must not run.
extern uint8_t g_fnhook[];
int fnhook_enter(struct Cpu *c, uint32_t addr);
#define FN_ENTER(a) do {                                                                         \
        if (__builtin_expect(g_fnprof, 0)) fnprof_hit(a, c->r[14], c->r[0]);                     \
        if (__builtin_expect((g_fnhook[(a) >> 5] >> (((a) >> 2) & 7)) & 1, 0) && fnhook_enter(c, a)) return; \
    } while (0)

// ---- exclusive monitor (emulated with compare-and-swap on the value the exclusive load saw)
#define EXCL(N, T)                                                                         \
    static inline uint32_t ldrex##N(Cpu *c, uint32_t a) {                                  \
        T v = __atomic_load_n((T *)G2H(a), __ATOMIC_ACQUIRE);                             \
        c->excl_addr = a;                                                                  \
        c->excl_val = v;                                                                   \
        return (uint32_t)v;                                                                \
    }                                                                                      \
    static inline uint32_t strex##N(Cpu *c, uint32_t a, uint32_t v) {                      \
        if (c->excl_addr != a) return 1;                                                   \
        c->excl_addr = ~0u;                                                                \
        T expected = (T)c->excl_val;                                                       \
        return !__atomic_compare_exchange_n((T *)G2H(a), &expected, (T)v, 0,              \
                                            __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);           \
    }
EXCL(8, uint8_t)
EXCL(16, uint16_t)
EXCL(32, uint32_t)
static inline uint64_t ldrexd(Cpu *c, uint32_t a) {
    uint64_t v = __atomic_load_n((uint64_t *)G2H(a), __ATOMIC_ACQUIRE);
    c->excl_addr = a;
    c->excl_val = v;
    return v;
}
static inline uint32_t strexd(Cpu *c, uint32_t a, uint64_t v) {
    if (c->excl_addr != a) return 1;
    c->excl_addr = ~0u;
    uint64_t expected = c->excl_val;
    return !__atomic_compare_exchange_n((uint64_t *)G2H(a), &expected, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
#define LDA(N, T, a) ((uint32_t)__atomic_load_n((T *)G2H(a), __ATOMIC_ACQUIRE))
#define STL(N, T, a, v) __atomic_store_n((T *)G2H(a), (T)(v), __ATOMIC_RELEASE)

// ---- flags
#define COND_EQ (c->zf)
#define COND_NE (!c->zf)
#define COND_CS (c->cf)
#define COND_CC (!c->cf)
#define COND_MI (c->nf)
#define COND_PL (!c->nf)
#define COND_VS (c->vf)
#define COND_VC (!c->vf)
#define COND_HI (c->cf && !c->zf)
#define COND_LS (!c->cf || c->zf)
#define COND_GE (c->nf == c->vf)
#define COND_LT (c->nf != c->vf)
#define COND_GT (!c->zf && c->nf == c->vf)
#define COND_LE (c->zf || c->nf != c->vf)

static inline uint32_t f_adds(Cpu *c, uint32_t a, uint32_t b) {
    uint32_t r = a + b;
    c->nf = r >> 31; c->zf = r == 0; c->cf = r < a; c->vf = ((a ^ r) & (b ^ r)) >> 31;
    return r;
}
static inline uint32_t f_subs(Cpu *c, uint32_t a, uint32_t b) {
    uint32_t r = a - b;
    c->nf = r >> 31; c->zf = r == 0; c->cf = a >= b; c->vf = ((a ^ b) & (a ^ r)) >> 31;
    return r;
}
static inline uint32_t f_adcs(Cpu *c, uint32_t a, uint32_t b) {
    uint64_t w = (uint64_t)a + b + c->cf;
    uint32_t r = (uint32_t)w;
    c->nf = r >> 31; c->zf = r == 0; c->cf = (uint32_t)(w >> 32); c->vf = ((a ^ r) & (b ^ r)) >> 31;
    return r;
}
static inline uint32_t f_sbcs(Cpu *c, uint32_t a, uint32_t b) { return f_adcs(c, a, ~b); }
static inline uint32_t f_nz(Cpu *c, uint32_t r) { c->nf = r >> 31; c->zf = r == 0; return r; }

// ---- shifts (register-specified amounts: ARM uses the bottom byte; LSL/LSR by 32+ give 0, ASR saturates)
static inline uint32_t sh_lsl(uint32_t v, uint32_t s) { s &= 0xFF; return s >= 32 ? 0 : v << s; }
static inline uint32_t sh_lsr(uint32_t v, uint32_t s) { s &= 0xFF; return s >= 32 ? 0 : v >> s; }
static inline uint32_t sh_asr(uint32_t v, uint32_t s) { s &= 0xFF; return (uint32_t)((int32_t)v >> (s >= 32 ? 31 : s)); }
static inline uint32_t sh_ror(uint32_t v, uint32_t s) { s &= 31; return s ? (v >> s) | (v << (32 - s)) : v; }
// ... and with the shifter carry-out (flag-setting logical instructions)
static inline uint32_t shc_lsl(Cpu *c, uint32_t v, uint32_t s) {
    s &= 0xFF;
    if (!s) return v;
    if (s < 32) { c->cf = (v >> (32 - s)) & 1; return v << s; }
    c->cf = s == 32 ? v & 1 : 0;
    return 0;
}
static inline uint32_t shc_lsr(Cpu *c, uint32_t v, uint32_t s) {
    s &= 0xFF;
    if (!s) return v;
    if (s < 32) { c->cf = (v >> (s - 1)) & 1; return v >> s; }
    c->cf = s == 32 ? v >> 31 : 0;
    return 0;
}
static inline uint32_t shc_asr(Cpu *c, uint32_t v, uint32_t s) {
    s &= 0xFF;
    if (!s) return v;
    if (s < 32) { c->cf = ((int32_t)v >> (s - 1)) & 1; return (uint32_t)((int32_t)v >> s); }
    c->cf = v >> 31;
    return (uint32_t)((int32_t)v >> 31);
}
static inline uint32_t shc_ror(Cpu *c, uint32_t v, uint32_t s) {
    s &= 0xFF;
    if (!s) return v;
    uint32_t r = sh_ror(v, s);
    c->cf = r >> 31;
    return r;
}
#define ROR32(v, r) ((uint32_t)(v) >> ((r) & 31) | (uint32_t)(v) << ((32 - ((r) & 31)) & 31))

// ---- misc integer
static inline uint32_t clz32(uint32_t v) { return v ? __builtin_clz(v) : 32; }
static inline uint32_t rbit32(uint32_t v) { return __builtin_bitreverse32(v); }
static inline uint32_t rev16_32(uint32_t v) { return ((v & 0x00ff00ffu) << 8) | ((v >> 8) & 0x00ff00ffu); }
static inline uint32_t revsh32(uint32_t v) { return (uint32_t)(int32_t)(int16_t)(((v & 0xFF) << 8) | ((v >> 8) & 0xFF)); }
static inline uint32_t udiv32(uint32_t a, uint32_t b) { return b ? a / b : 0; }
static inline uint32_t sdiv32(uint32_t a, uint32_t b) {
    if (!b) return 0;
    if ((int32_t)a == INT32_MIN && (int32_t)b == -1) return a;
    return (uint32_t)((int32_t)a / (int32_t)b);
}
static inline uint32_t ssat32(Cpu *c, int64_t v, int n) {  // signed saturate to n bits (1..32)
    int64_t hi = ((int64_t)1 << (n - 1)) - 1, lo = -((int64_t)1 << (n - 1));
    if (v > hi) { c->qf = 1; return (uint32_t)hi; }
    if (v < lo) { c->qf = 1; return (uint32_t)lo; }
    return (uint32_t)v;
}
static inline uint32_t usat32(Cpu *c, int64_t v, int n) {  // unsigned saturate to n bits (0..31)
    int64_t hi = ((int64_t)1 << n) - 1;
    if (v > hi) { c->qf = 1; return (uint32_t)hi; }
    if (v < 0) { c->qf = 1; return 0; }
    return (uint32_t)v;
}
static inline uint32_t qadd32(Cpu *c, uint32_t a, uint32_t b) { return ssat32(c, (int64_t)(int32_t)a + (int32_t)b, 32); }
static inline uint32_t qsub32(Cpu *c, uint32_t a, uint32_t b) { return ssat32(c, (int64_t)(int32_t)a - (int32_t)b, 32); }
static inline uint32_t sxtb16(uint32_t v) { return ((uint32_t)(int32_t)(int8_t)v & 0xFFFF) | ((uint32_t)(int16_t)(int8_t)(v >> 16) << 16); }
static inline uint32_t uxtb16(uint32_t v) { return v & 0x00FF00FFu; }

// APSR as a word (MRS) and back (MSR APSR_nzcvq / _g)
static inline uint32_t apsr_get(Cpu *c) {
    return (c->nf << 31) | (c->zf << 30) | (c->cf << 29) | (c->vf << 28) | (c->qf << 27) | ((c->ge & 15) << 16) | 0x10;
}
static inline void apsr_set(Cpu *c, uint32_t v, int nzcvq, int g) {
    if (nzcvq) { c->nf = v >> 31; c->zf = (v >> 30) & 1; c->cf = (v >> 29) & 1; c->vf = (v >> 28) & 1; c->qf = (v >> 27) & 1; }
    if (g) c->ge = (v >> 16) & 15;
}

// CRC32 / CRC32C (ARMv8): acc updated with the low `bytes` bytes of v, reflected polynomial tables in simd_int.c
extern const uint32_t rt_crc32_tab[256], rt_crc32c_tab[256];
static inline uint32_t crc32_n(uint32_t acc, uint32_t v, int bytes, const uint32_t *tab) {
    for (int i = 0; i < bytes; i++) acc = tab[(acc ^ (v >> (8 * i))) & 0xFF] ^ (acc >> 8);
    return acc;
}

// Parallel add / subtract and friends (rare in game code: out of line, runtime/simd_int.c)
uint32_t rt_parallel(Cpu *c, uint32_t op, uint32_t a, uint32_t b);
uint32_t rt_sel(Cpu *c, uint32_t a, uint32_t b);
uint32_t rt_usad8(uint32_t a, uint32_t b);

// ---- VFP (scalar floating point), runtime/vfp.h
#include "vfp.h"
