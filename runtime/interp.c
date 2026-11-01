// AArch32 interpreter (runtime/interp.h). One function per instruction class, in the order and with the semantics of
// tools/lifter.py and tools/neon.py (the comments name the same instructions), so interpreted and recompiled code
// agree; tools/test_lift.py --interp checks it against Unicorn the way it checks the lifter.
#pragma STDC FP_CONTRACT OFF
#include <math.h>
#include <stdlib.h>

#include "interp.h"
#include "neon_rt.h"
#include "rt.h"

// ---- bad accesses (see interp.h). The checks wrap every load / store of the interpreter: one compare of the address
// for the null page and the range where small floats live (0x3C000000-0x43FFFFFF either sign: 0.008 .. 500);
// everything else costs nothing. A hit is looked at in bad_access, off the common path.
__thread uint32_t g_ip_pc, g_ip_last;
__thread Cpu *g_ip_cpu;
#include "kernel.h"

static __attribute__((noinline, cold)) void bad_access(uint32_t a, int size, int wr) {
    if (!g_ip_pc) return;
    if (a < 0x10000u) {
        forge_guest_fault(g_ip_pc, a, size, wr, "null page");
        return;
    }
    if (a >= HEAP_BASE && a - HEAP_BASE < kernel_heap_size()) return;  // (heap: mapped)
    if (!kernel_mapped(a, 1)) forge_guest_fault(g_ip_pc, a, size, wr, "float bit pattern used as an address");
}

static inline __attribute__((always_inline)) void *gchk(uint32_t a, int size, int wr) {
    if (__builtin_expect((a < 0x10000u) | (((a & 0x7FFFFFFFu) - 0x3C000000u) < 0x08000000u), 0)) bad_access(a, size, wr);
    return G2H(a);
}
#undef LD8
#undef LD16
#undef LD32
#undef LD64
#undef LDF
#undef LDD
#undef ST8
#undef ST16
#undef ST32
#undef ST64
#undef STF
#undef STD
#define LD8(a)  (*(const u8a *)gchk((a), 1, 0))
#define LD16(a) (*(const u16u *)gchk((a), 2, 0))
#define LD32(a) (*(const u32u *)gchk((a), 4, 0))
#define LD64(a) (*(const u64u *)gchk((a), 8, 0))
#define LDF(a)  (*(const f32u *)gchk((a), 4, 0))
#define LDD(a)  (*(const f64u *)gchk((a), 8, 0))
#define ST8(a, x)  (*(u8a *)gchk((a), 1, 1) = (uint8_t)(x))
#define ST16(a, x) (*(u16u *)gchk((a), 2, 1) = (uint16_t)(x))
#define ST32(a, x) (*(u32u *)gchk((a), 4, 1) = (uint32_t)(x))
#define ST64(a, x) (*(u64u *)gchk((a), 8, 1) = (uint64_t)(x))
#define STF(a, x)  (*(f32u *)gchk((a), 4, 1) = (x))
#define STD(a, x)  (*(f64u *)gchk((a), 8, 1) = (x))

#define BITS(w, lo, n) (((w) >> (lo)) & ((1u << (n)) - 1u))
#define BIT(w, i) (((w) >> (i)) & 1u)

static __attribute__((noreturn)) void bad(uint32_t pc, uint32_t w, const char *what) {
    FATAL("interp: unsupported %s (%08x at %08x)\n", what, w, pc);
}

static inline uint32_t RR(Cpu *c, uint32_t pc, uint32_t n) { return n == 15 ? pc + 8 : c->r[n]; }
static inline uint32_t ror32(uint32_t v, uint32_t r) { r &= 31; return r ? (v >> r) | (v << (32 - r)) : v; }

static int cond_pass(Cpu *c, uint32_t cond) {
    switch (cond) {
    case 0: return COND_EQ;
    case 1: return COND_NE;
    case 2: return COND_CS;
    case 3: return COND_CC;
    case 4: return COND_MI;
    case 5: return COND_PL;
    case 6: return COND_VS;
    case 7: return COND_VC;
    case 8: return COND_HI;
    case 9: return COND_LS;
    case 10: return COND_GE;
    case 11: return COND_LT;
    case 12: return COND_GT;
    case 13: return COND_LE;
    default: return 1;
    }
}

// ================================================================ shifter operands
// carry: the shifter carry-out, or -1 when the shift leaves C alone
static uint32_t imm_operand(uint32_t w, int *carry) {
    uint32_t rot = BITS(w, 8, 4) * 2, v = ror32(BITS(w, 0, 8), rot);
    *carry = rot ? (int)(v >> 31) : -1;
    return v;
}

static uint32_t reg_operand(Cpu *c, uint32_t pc, uint32_t w, int *carry) {
    uint32_t m = RR(c, pc, BITS(w, 0, 4)), typ = BITS(w, 5, 2);
    *carry = -1;
    if (!BIT(w, 4)) {  // immediate shift
        uint32_t amt = BITS(w, 7, 5);
        switch (typ) {
        case 0:
            if (!amt) return m;
            *carry = (m >> (32 - amt)) & 1;
            return m << amt;
        case 1:
            if (!amt) { *carry = m >> 31; return 0; }
            *carry = (m >> (amt - 1)) & 1;
            return m >> amt;
        case 2:
            if (!amt) { *carry = m >> 31; return (uint32_t)((int32_t)m >> 31); }
            *carry = (m >> (amt - 1)) & 1;
            return (uint32_t)((int32_t)m >> amt);
        default:
            if (!amt) { *carry = m & 1; return (c->cf << 31) | (m >> 1); }  // RRX
            *carry = (m >> (amt - 1)) & 1;
            return ror32(m, amt);
        }
    }
    uint32_t s = RR(c, pc, BITS(w, 8, 4)) & 0xFF;  // register-specified shift
    if (!s) return m;
    switch (typ) {
    case 0:
        if (s < 32) { *carry = (m >> (32 - s)) & 1; return m << s; }
        *carry = s == 32 ? (int)(m & 1) : 0;
        return 0;
    case 1:
        if (s < 32) { *carry = (m >> (s - 1)) & 1; return m >> s; }
        *carry = s == 32 ? (int)(m >> 31) : 0;
        return 0;
    case 2:
        if (s < 32) { *carry = ((int32_t)m >> (s - 1)) & 1; return (uint32_t)((int32_t)m >> s); }
        *carry = m >> 31;
        return (uint32_t)((int32_t)m >> 31);
    default: {
        uint32_t r = ror32(m, s);
        *carry = r >> 31;
        return r;
    }
    }
}

// ================================================================ data processing / misc
static uint32_t data_proc(Cpu *c, uint32_t pc, uint32_t w) {
    uint32_t I = BIT(w, 25), opc = BITS(w, 21, 4), S = BIT(w, 20), rn = BITS(w, 16, 4), rd = BITS(w, 12, 4);
    int carry;
    uint32_t b = I ? imm_operand(w, &carry) : reg_operand(c, pc, w, &carry);
    uint32_t a = RR(c, pc, rn), r;
    int logical = opc <= 1 || opc == 8 || opc == 9 || opc >= 12;
    if (S && !logical) {
        switch (opc) {
        case 2: r = f_subs(c, a, b); break;
        case 3: r = f_subs(c, b, a); break;
        case 4: r = f_adds(c, a, b); break;
        case 5: r = f_adcs(c, a, b); break;
        case 6: r = f_sbcs(c, a, b); break;
        case 7: r = f_sbcs(c, b, a); break;
        case 10: f_subs(c, a, b); return pc + 4;
        default: f_adds(c, a, b); return pc + 4;  // 11: CMN
        }
        if (rd == 15) bad(pc, w, "flag-setting write to pc");
        c->r[rd] = r;
        return pc + 4;
    }
    switch (opc) {
    case 0: case 8: r = a & b; break;
    case 1: case 9: r = a ^ b; break;
    case 2: r = a - b; break;
    case 3: r = b - a; break;
    case 4: r = a + b; break;
    case 5: r = a + b + c->cf; break;
    case 6: r = a - b - 1u + c->cf; break;
    case 7: r = b - a - 1u + c->cf; break;
    case 12: r = a | b; break;
    case 13: r = b; break;
    case 14: r = a & ~b; break;
    case 15: r = ~b; break;
    default: bad(pc, w, "test without S");
    }
    if (S) {  // logical, flag-setting
        if (carry >= 0) c->cf = (uint32_t)carry;
        c->nf = r >> 31;
        c->zf = r == 0;
        if (opc == 8 || opc == 9) return pc + 4;
        if (rd == 15) bad(pc, w, "flag-setting write to pc");
    }
    if (rd == 15) return r;  // (mov pc, lr; computed jumps)
    c->r[rd] = r;
    return pc + 4;
}

static uint32_t misc(Cpu *c, uint32_t pc, uint32_t w, int *link) {
    uint32_t op = BITS(w, 21, 2), op2 = BITS(w, 4, 3), rd = BITS(w, 12, 4), rm = BITS(w, 0, 4);
    if (op2 == 0) {
        if (op & 1) {  // MSR (register)
            uint32_t mask = BITS(w, 18, 2);
            apsr_set(c, RR(c, pc, rm), mask >> 1, mask & 1);
        } else {
            c->r[rd] = apsr_get(c);  // MRS
        }
        return pc + 4;
    }
    if (op2 == 1 && op == 1) {  // BX
        uint32_t t = RR(c, pc, rm);
        if (t & 1) rt_thumb(c, pc);
        return t;
    }
    if (op2 == 1 && op == 3) { c->r[rd] = clz32(RR(c, pc, rm)); return pc + 4; }
    if (op2 == 3 && op == 1) {  // BLX (register)
        uint32_t t = RR(c, pc, rm);
        if (t & 1) rt_thumb(c, pc);
        c->r[14] = pc + 4;
        *link = 1;
        return t;
    }
    if (op2 == 5) {  // QADD / QSUB / QDADD / QDSUB
        uint32_t a = RR(c, pc, rm), b = RR(c, pc, BITS(w, 16, 4)), r;
        switch (op) {
        case 0: r = qadd32(c, a, b); break;
        case 1: r = qsub32(c, a, b); break;
        case 2: r = qadd32(c, a, qadd32(c, b, b)); break;
        default: r = qsub32(c, a, qadd32(c, b, b)); break;
        }
        c->r[rd] = r;
        return pc + 4;
    }
    if (op2 == 4) {  // CRC32 / CRC32C
        c->r[rd] = crc32_n(RR(c, pc, BITS(w, 16, 4)), RR(c, pc, rm), 1 << op, BIT(w, 9) ? rt_crc32c_tab : rt_crc32_tab);
        return pc + 4;
    }
    if (op2 == 7 && op == 1) rt_bkpt(c, (BITS(w, 8, 12) << 4) | BITS(w, 0, 4), pc);
    bad(pc, w, "misc");
}

static uint32_t halfword_mul(Cpu *c, uint32_t pc, uint32_t w) {
    uint32_t op = BITS(w, 21, 2), rd = BITS(w, 16, 4), ra = BITS(w, 12, 4), rm = BITS(w, 8, 4), rn = BITS(w, 0, 4);
    int32_t hn = (int16_t)(RR(c, pc, rn) >> (BIT(w, 5) ? 16 : 0));
    int32_t hm = (int16_t)(RR(c, pc, rm) >> (BIT(w, 6) ? 16 : 0));
    if (op == 0) {  // SMLAxy
        int64_t p = (int64_t)(hn * hm) + (int32_t)RR(c, pc, ra);
        if (p != (int32_t)p) c->qf = 1;
        c->r[rd] = (uint32_t)p;
    } else if (op == 1) {
        int32_t prod = (int32_t)(((int64_t)(int32_t)RR(c, pc, rn) * hm) >> 16);
        if (!BIT(w, 5)) {  // SMLAWy
            int64_t p = (int64_t)prod + (int32_t)RR(c, pc, ra);
            if (p != (int32_t)p) c->qf = 1;
            c->r[rd] = (uint32_t)p;
        } else {
            c->r[rd] = (uint32_t)prod;  // SMULWy
        }
    } else if (op == 2) {  // SMLALxy: rdhi = rd, rdlo = ra
        uint64_t acc = ((uint64_t)RR(c, pc, rd) << 32) | RR(c, pc, ra);
        acc += (uint64_t)(int64_t)(hn * hm);
        c->r[ra] = (uint32_t)acc;
        c->r[rd] = (uint32_t)(acc >> 32);
    } else {
        c->r[rd] = (uint32_t)(hn * hm);  // SMULxy
    }
    return pc + 4;
}

static uint32_t multiply(Cpu *c, uint32_t pc, uint32_t w) {
    uint32_t op = BITS(w, 21, 3), S = BIT(w, 20);
    uint32_t rd = BITS(w, 16, 4), ra = BITS(w, 12, 4), rm = BITS(w, 8, 4), rn = BITS(w, 0, 4);
    uint32_t a = RR(c, pc, rn), b = RR(c, pc, rm);
    if (op <= 1) {
        uint32_t r = a * b + (op ? RR(c, pc, ra) : 0);
        if (S) { c->nf = r >> 31; c->zf = r == 0; }
        c->r[rd] = r;
        return pc + 4;
    }
    if (op == 2) {  // UMAAL
        uint64_t r = (uint64_t)a * b + RR(c, pc, ra) + RR(c, pc, rd);
        c->r[ra] = (uint32_t)r;
        c->r[rd] = (uint32_t)(r >> 32);
        return pc + 4;
    }
    if (op == 3) { c->r[rd] = RR(c, pc, ra) - a * b; return pc + 4; }  // MLS
    uint64_t acc = ((uint64_t)RR(c, pc, rd) << 32) | RR(c, pc, ra), r;  // long: rdhi = rd, rdlo = ra
    switch (op) {
    case 4: r = (uint64_t)a * b; break;
    case 5: r = (uint64_t)a * b + acc; break;
    case 6: r = (uint64_t)((int64_t)(int32_t)a * (int32_t)b); break;
    default: r = (uint64_t)((int64_t)(int32_t)a * (int32_t)b) + acc; break;
    }
    if (S) { c->nf = (uint32_t)(r >> 63); c->zf = r == 0; }
    c->r[ra] = (uint32_t)r;
    c->r[rd] = (uint32_t)(r >> 32);
    return pc + 4;
}

static uint32_t sync_insn(Cpu *c, uint32_t pc, uint32_t w) {
    uint32_t rn = BITS(w, 16, 4), rt = BITS(w, 12, 4), rt2 = BITS(w, 0, 4), a = RR(c, pc, rn);
    if (!BIT(w, 23)) {  // SWP / SWPB
        if (BIT(w, 22)) c->r[rt] = __atomic_exchange_n((uint8_t *)G2H(a), (uint8_t)RR(c, pc, rt2), __ATOMIC_SEQ_CST);
        else c->r[rt] = __atomic_exchange_n((uint32_t *)G2H(a), RR(c, pc, rt2), __ATOMIC_SEQ_CST);
        return pc + 4;
    }
    uint32_t L = BIT(w, 20), sz = BITS(w, 21, 2), kind = BITS(w, 8, 2);
    if (kind == 0) {  // LDA / STL
        if (sz == 1) bad(pc, w, "lda size");
        if (L) c->r[rt] = sz == 0 ? LDA(32, uint32_t, a) : sz == 2 ? LDA(8, uint8_t, a) : LDA(16, uint16_t, a);
        else if (sz == 0) STL(32, uint32_t, a, RR(c, pc, rt2));
        else if (sz == 2) STL(8, uint8_t, a, RR(c, pc, rt2));
        else STL(16, uint16_t, a, RR(c, pc, rt2));
        return pc + 4;
    }
    if (L) {
        if (sz == 1) {  // LDREXD rt, rt+1
            uint64_t v = ldrexd(c, a);
            c->r[rt] = (uint32_t)v;
            c->r[rt + 1] = (uint32_t)(v >> 32);
        } else {
            c->r[rt] = sz == 0 ? ldrex32(c, a) : sz == 2 ? ldrex8(c, a) : ldrex16(c, a);
        }
        return pc + 4;
    }
    uint32_t rd = rt;  // status register
    if (sz == 1) c->r[rd] = strexd(c, a, ((uint64_t)RR(c, pc, rt2 + 1) << 32) | RR(c, pc, rt2));
    else if (sz == 0) c->r[rd] = strex32(c, a, RR(c, pc, rt2));
    else if (sz == 2) c->r[rd] = strex8(c, a, RR(c, pc, rt2));
    else c->r[rd] = strex16(c, a, RR(c, pc, rt2));
    return pc + 4;
}

// ================================================================ loads / stores
// P/U/W addressing: *ea the address used, return value the writeback value (or ~0 flag in *wb = 0)
static uint32_t addr_mode(uint32_t w, uint32_t base, uint32_t off, int *wb, uint32_t *wbv) {
    uint32_t P = BIT(w, 24), U = BIT(w, 23), Wb = BIT(w, 21);
    uint32_t o = U ? base + off : base - off;
    if (P) {
        *wb = (int)Wb;
        *wbv = o;
        return o;
    }
    *wb = 1;  // post-indexed always writes back
    *wbv = o;
    return base;
}

static uint32_t ldst_word_byte(Cpu *c, uint32_t pc, uint32_t w) {
    uint32_t I = BIT(w, 25), B = BIT(w, 22), L = BIT(w, 20), rn = BITS(w, 16, 4), rt = BITS(w, 12, 4);
    int carry, wb;
    uint32_t off = I ? reg_operand(c, pc, w & ~0x10u, &carry) : BITS(w, 0, 12), wbv;
    uint32_t ea = addr_mode(w, RR(c, pc, rn), off, &wb, &wbv);
    if (L) {
        uint32_t v = B ? LD8(ea) : LD32(ea);
        if (wb && rn != 15) c->r[rn] = wbv;
        if (rt == 15) return v;
        c->r[rt] = v;
    } else {
        uint32_t v = RR(c, pc, rt);
        if (B) ST8(ea, v);
        else ST32(ea, v);
        if (wb && rn != 15) c->r[rn] = wbv;
    }
    return pc + 4;
}

static uint32_t extra_ldst(Cpu *c, uint32_t pc, uint32_t w) {
    uint32_t I = BIT(w, 22), L = BIT(w, 20), rn = BITS(w, 16, 4), rt = BITS(w, 12, 4), op2 = BITS(w, 5, 2);
    uint32_t off = I ? (BITS(w, 8, 4) << 4) | BITS(w, 0, 4) : RR(c, pc, BITS(w, 0, 4)), wbv;
    int wb;
    uint32_t ea = addr_mode(w, RR(c, pc, rn), off, &wb, &wbv), v = 0;
    if (op2 == 1) {
        if (L) v = LD16(ea);
        else ST16(ea, RR(c, pc, rt));
    } else if (op2 == 2 && L) {
        v = (uint32_t)(int32_t)(int8_t)LD8(ea);
    } else if (op2 == 3 && L) {
        v = (uint32_t)(int32_t)(int16_t)LD16(ea);
    } else if (op2 == 2) {  // LDRD
        uint32_t v1 = LD32(ea), v2 = LD32(ea + 4);
        if (wb && rn != 15) c->r[rn] = wbv;
        c->r[rt] = v1;
        c->r[rt + 1] = v2;
        return pc + 4;
    } else {  // STRD
        ST32(ea, RR(c, pc, rt));
        ST32(ea + 4, RR(c, pc, rt + 1));
    }
    if (wb && rn != 15) c->r[rn] = wbv;
    if (L) c->r[rt] = v;
    return pc + 4;
}

static uint32_t ldm_stm(Cpu *c, uint32_t pc, uint32_t w) {
    uint32_t P = BIT(w, 24), U = BIT(w, 23), Wb = BIT(w, 21), L = BIT(w, 20), rn = BITS(w, 16, 4);
    uint32_t n = (uint32_t)__builtin_popcount(w & 0xFFFF);
    if (BIT(w, 22)) bad(pc, w, "ldm/stm user registers");
    if (!n) bad(pc, w, "empty register list");
    uint32_t base = RR(c, pc, rn), ea, wbv;
    if (U) { ea = P ? base + 4 : base; wbv = base + 4 * n; }
    else { ea = P ? base - 4 * n : base - 4 * n + 4; wbv = base - 4 * n; }
    if (L) {
        if (Wb) c->r[rn] = wbv;
        uint32_t next = pc + 4;
        for (uint32_t r = 0, i = 0; r < 16; r++) {
            if (!BIT(w, r)) continue;
            uint32_t v = LD32(ea + 4 * i++);
            if (r == 15) next = v;
            else c->r[r] = v;
        }
        return next;
    }
    for (uint32_t r = 0, i = 0; r < 16; r++)
        if (BIT(w, r)) ST32(ea + 4 * i++, RR(c, pc, r));
    if (Wb) c->r[rn] = wbv;
    return pc + 4;
}

// ================================================================ media
static uint32_t pack(Cpu *c, uint32_t pc, uint32_t w) {
    uint32_t op1 = BITS(w, 20, 3), op2 = BITS(w, 5, 3), rd = BITS(w, 12, 4), rn = BITS(w, 16, 4);
    uint32_t m = RR(c, pc, BITS(w, 0, 4)), mr = ror32(m, BITS(w, 10, 2) * 8);
    if (op1 == 0 && !(op2 & 1)) {  // PKHBT / PKHTB
        uint32_t imm = BITS(w, 7, 5);
        if (BIT(w, 6)) c->r[rd] = (RR(c, pc, rn) & 0xFFFF0000u) | ((uint32_t)((int32_t)m >> (imm ? imm : 31)) & 0xFFFFu);
        else c->r[rd] = (RR(c, pc, rn) & 0xFFFFu) | ((m << imm) & 0xFFFF0000u);
        return pc + 4;
    }
    if (op1 == 0 && op2 == 3) {  // SXTAB16 / SXTB16
        uint32_t x = sxtb16(mr);
        if (rn == 15) { c->r[rd] = x; return pc + 4; }
        uint32_t nn = RR(c, pc, rn);
        c->r[rd] = ((nn + x) & 0xFFFFu) | (((nn >> 16) + (x >> 16)) << 16);
        return pc + 4;
    }
    if (op1 == 0 && op2 == 5) { c->r[rd] = rt_sel(c, RR(c, pc, rn), m); return pc + 4; }
    if ((op1 & 6) == 2 && !(op2 & 1)) {  // SSAT
        uint32_t sat = BITS(w, 16, 5) + 1, imm = BITS(w, 7, 5);
        int64_t v = BIT(w, 6) ? (int64_t)((int32_t)m >> (imm ? imm : 31)) : (int64_t)(int32_t)(m << imm);
        c->r[rd] = ssat32(c, v, (int)sat);
        return pc + 4;
    }
    if ((op1 & 6) == 6 && !(op2 & 1)) {  // USAT
        uint32_t sat = BITS(w, 16, 5), imm = BITS(w, 7, 5);
        int64_t v = BIT(w, 6) ? (int64_t)((int32_t)m >> (imm ? imm : 31)) : (int64_t)(int32_t)(m << imm);
        c->r[rd] = usat32(c, v, (int)sat);
        return pc + 4;
    }
    if (op1 == 2 && op2 == 1) {  // SSAT16
        int sat = (int)BITS(w, 16, 4) + 1;
        c->r[rd] = (ssat32(c, (int16_t)m, sat) & 0xFFFFu) | (ssat32(c, (int16_t)(m >> 16), sat) << 16);
        return pc + 4;
    }
    if (op1 == 6 && op2 == 1) {  // USAT16
        int sat = (int)BITS(w, 16, 4);
        c->r[rd] = (usat32(c, (int16_t)m, sat) & 0xFFFFu) | (usat32(c, (int16_t)(m >> 16), sat) << 16);
        return pc + 4;
    }
    if (op2 == 3 && (op1 == 2 || op1 == 3 || op1 == 6 || op1 == 7)) {  // SXTB / SXTH / UXTB / UXTH (+ add)
        uint32_t x = op1 == 2 ? (uint32_t)(int32_t)(int8_t)mr : op1 == 3 ? (uint32_t)(int32_t)(int16_t)mr
                   : op1 == 6 ? (uint32_t)(uint8_t)mr : (uint32_t)(uint16_t)mr;
        c->r[rd] = rn == 15 ? x : RR(c, pc, rn) + x;
        return pc + 4;
    }
    if (op1 == 4 && op2 == 3) {  // UXTAB16 / UXTB16
        uint32_t x = uxtb16(mr);
        if (rn == 15) { c->r[rd] = x; return pc + 4; }
        uint32_t nn = RR(c, pc, rn);
        c->r[rd] = ((nn + x) & 0xFFFFu) | (((nn >> 16) + (x >> 16)) << 16);
        return pc + 4;
    }
    if (op1 == 3 && op2 == 1) { c->r[rd] = __builtin_bswap32(m); return pc + 4; }
    if (op1 == 3 && op2 == 5) { c->r[rd] = rev16_32(m); return pc + 4; }
    if (op1 == 7 && op2 == 1) { c->r[rd] = rbit32(m); return pc + 4; }
    if (op1 == 7 && op2 == 5) { c->r[rd] = revsh32(m); return pc + 4; }
    bad(pc, w, "pack");
}

static uint32_t signed_mul(Cpu *c, uint32_t pc, uint32_t w) {
    uint32_t op1 = BITS(w, 20, 3), op2 = BITS(w, 5, 3);
    uint32_t rd = BITS(w, 16, 4), ra = BITS(w, 12, 4), n = RR(c, pc, BITS(w, 0, 4)), m = RR(c, pc, BITS(w, 8, 4));
    if (op1 == 1 && op2 == 0) { c->r[rd] = sdiv32(n, m); return pc + 4; }
    if (op1 == 3 && op2 == 0) { c->r[rd] = udiv32(n, m); return pc + 4; }
    if (op1 == 0) {  // SMLAD / SMLSD / SMUAD / SMUSD
        uint32_t mm = BIT(w, 5) ? ror32(m, 16) : m;
        int32_t lo = (int32_t)(int16_t)n * (int32_t)(int16_t)mm, hi = (int32_t)(int16_t)(n >> 16) * (int32_t)(int16_t)(mm >> 16);
        int64_t p = BIT(w, 6) ? (int64_t)lo - (int64_t)hi : (int64_t)lo + (int64_t)hi;
        if (ra != 15) p += (int32_t)RR(c, pc, ra);
        if (p != (int32_t)p) c->qf = 1;
        c->r[rd] = (uint32_t)p;
        return pc + 4;
    }
    if (op1 == 4) {  // SMLALD / SMLSLD: rdhi = rd, rdlo = ra
        uint32_t mm = BIT(w, 5) ? ror32(m, 16) : m;
        int64_t lo = (int32_t)(int16_t)n * (int32_t)(int16_t)mm, hi = (int32_t)(int16_t)(n >> 16) * (int32_t)(int16_t)(mm >> 16);
        uint64_t acc = ((uint64_t)RR(c, pc, rd) << 32) | RR(c, pc, ra);
        acc += (uint64_t)(BIT(w, 6) ? lo - hi : lo + hi);
        c->r[ra] = (uint32_t)acc;
        c->r[rd] = (uint32_t)(acc >> 32);
        return pc + 4;
    }
    if (op1 == 5) {  // SMMUL / SMMLA / SMMLS
        int64_t p = (int64_t)(int32_t)n * (int32_t)m, rnd = BIT(w, 5) ? 0x80000000ll : 0;
        if (op2 <= 1) {
            if (ra == 15) c->r[rd] = (uint32_t)((uint64_t)(p + rnd) >> 32);
            else c->r[rd] = (uint32_t)((uint64_t)((int64_t)((uint64_t)RR(c, pc, ra) << 32) + p + rnd) >> 32);
            return pc + 4;
        }
        if (op2 >= 6) {
            c->r[rd] = (uint32_t)((uint64_t)((int64_t)((uint64_t)RR(c, pc, ra) << 32) - p + rnd) >> 32);
            return pc + 4;
        }
    }
    bad(pc, w, "signed multiply");
}

static uint32_t media(Cpu *c, uint32_t pc, uint32_t w) {
    uint32_t op1 = BITS(w, 20, 5), op2 = BITS(w, 5, 3), rd = BITS(w, 12, 4), rm = BITS(w, 0, 4);
    if ((op1 & 0x18) == 0 && (op1 & 3)) {  // parallel add/sub: signed (op1 1-3) and unsigned (5-7: UADD8, UQSUB8...)
        c->r[rd] = rt_parallel(c, (BITS(w, 20, 3) << 3) | op2, RR(c, pc, BITS(w, 16, 4)), RR(c, pc, rm));
        return pc + 4;
    }
    if ((op1 & 0x18) == 0x08) return pack(c, pc, w);
    if ((op1 & 0x18) == 0x10) return signed_mul(c, pc, w);
    if (op1 == 0x18 && op2 == 0) {  // USAD8 / USADA8
        uint32_t rd2 = BITS(w, 16, 4), ra2 = BITS(w, 12, 4), e = rt_usad8(RR(c, pc, rm), RR(c, pc, BITS(w, 8, 4)));
        c->r[rd2] = ra2 == 15 ? e : e + RR(c, pc, ra2);
        return pc + 4;
    }
    uint32_t lsb = BITS(w, 7, 5), msb = BITS(w, 16, 5);
    if ((op1 & 0x1E) == 0x1A && (op2 & 3) == 2) {  // SBFX
        uint32_t width = msb + 1;
        c->r[rd] = (uint32_t)((int32_t)(RR(c, pc, rm) << (32 - lsb - width)) >> (32 - width));
        return pc + 4;
    }
    if ((op1 & 0x1E) == 0x1E && (op2 & 3) == 2) {  // UBFX
        uint32_t width = msb + 1;
        c->r[rd] = (RR(c, pc, rm) >> lsb) & (uint32_t)((1ull << width) - 1);
        return pc + 4;
    }
    if ((op1 & 0x1E) == 0x1C && (op2 & 3) == 0) {  // BFC / BFI
        if (msb < lsb) bad(pc, w, "bfi msb<lsb");
        uint32_t mask = (uint32_t)(((1ull << (msb - lsb + 1)) - 1) << lsb);
        c->r[rd] = rm == 15 ? c->r[rd] & ~mask : (c->r[rd] & ~mask) | ((RR(c, pc, rm) << lsb) & mask);
        return pc + 4;
    }
    if (op1 == 0x1F && op2 == 7) rt_udf(c, pc);
    bad(pc, w, "media");
}

// ================================================================ VFP
#define SREG(v, x) (((v) << 1) | (x))
#define DREG(v, x) (((x) << 4) | (v))

static void vfp_mov64(Cpu *c, uint32_t pc, uint32_t w) {
    uint32_t rt2 = BITS(w, 16, 4), rt = BITS(w, 12, 4);
    if (BIT(w, 8)) {  // Dm <-> rt, rt2
        uint32_t m = DREG(BITS(w, 0, 4), BIT(w, 5));
        if (BIT(w, 20)) {
            uint64_t v = c->v.d[m];
            c->r[rt] = (uint32_t)v;
            c->r[rt2] = (uint32_t)(v >> 32);
        } else {
            c->v.d[m] = ((uint64_t)RR(c, pc, rt2) << 32) | RR(c, pc, rt);
        }
        return;
    }
    uint32_t m = SREG(BITS(w, 0, 4), BIT(w, 5));
    if (BIT(w, 20)) {
        uint32_t a = c->v.s[m], b = c->v.s[m + 1];
        c->r[rt] = a;
        c->r[rt2] = b;
    } else {
        c->v.s[m] = RR(c, pc, rt);
        c->v.s[m + 1] = RR(c, pc, rt2);
    }
}

static void vfp_ldst(Cpu *c, uint32_t pc, uint32_t w) {
    uint32_t P = BIT(w, 24), U = BIT(w, 23), D = BIT(w, 22), Wb = BIT(w, 21), L = BIT(w, 20);
    uint32_t rn = BITS(w, 16, 4), vd = BITS(w, 12, 4), imm8 = BITS(w, 0, 8), dbl = BIT(w, 8);
    uint32_t first = dbl ? DREG(vd, D) : SREG(vd, D);
    if (P && !Wb) {  // VLDR / VSTR
        uint32_t off = imm8 * 4, base = rn == 15 ? (pc + 8) & ~3u : c->r[rn];
        uint32_t ea = U ? base + off : base - off;
        if (L) {
            if (dbl) c->v.d[first] = LD64(ea);
            else c->v.s[first] = LD32(ea);
        } else {
            if (dbl) ST64(ea, c->v.d[first]);
            else ST32(ea, c->v.s[first]);
        }
        return;
    }
    // VLDM / VSTM (IA with optional writeback, DB with writeback)
    uint32_t count = dbl ? imm8 / 2 : imm8, total = imm8 * 4, base = RR(c, pc, rn);
    uint32_t ea = U ? base : base - total;
    for (uint32_t i = 0; i < count; i++) {
        if (dbl) {
            if (L) c->v.d[first + i] = LD64(ea + 8 * i);
            else ST64(ea + 8 * i, c->v.d[first + i]);
        } else {
            if (L) c->v.s[first + i] = LD32(ea + 4 * i);
            else ST32(ea + 4 * i, c->v.s[first + i]);
        }
    }
    if (Wb) c->r[rn] = U ? base + total : base - total;
}

static void vdup_core(Cpu *c, uint32_t w, uint32_t src);

static void vfp_mov32(Cpu *c, uint32_t pc, uint32_t w) {
    uint32_t L = BIT(w, 20), C = BIT(w, 8), A = BITS(w, 21, 3), rt = BITS(w, 12, 4), vn = BITS(w, 16, 4);
    if (!C) {
        uint32_t n = SREG(vn, BIT(w, 7));
        if (A == 0) {
            if (L) c->r[rt] = c->v.s[n];
            else c->v.s[n] = RR(c, pc, rt);
            return;
        }
        if (A == 7) {
            if (L) {
                if (rt == 15) vfp_mrs_flags(c);
                else c->r[rt] = c->fpscr;
            } else {
                c->fpscr = RR(c, pc, rt);
            }
            return;
        }
        bad(pc, w, "vmov32");
    }
    uint32_t d = DREG(vn, BIT(w, 7)), opc1 = BITS(w, 21, 2), opc2 = BITS(w, 5, 2);
    if (!L) {
        if (!BIT(w, 23)) {  // VMOV Dd[x], Rt
            if (opc1 & 2) c->v.b[d * 8 + (((opc1 & 1) << 2) | opc2)] = (uint8_t)RR(c, pc, rt);
            else if (opc2 & 1) c->v.h[d * 4 + (((opc1 & 1) << 1) | (opc2 >> 1))] = (uint16_t)RR(c, pc, rt);
            else if (!(opc2 & 3)) c->v.s[d * 2 + (opc1 & 1)] = RR(c, pc, rt);
            else bad(pc, w, "vmov scalar");
            return;
        }
        vdup_core(c, w, RR(c, pc, rt));
        return;
    }
    uint32_t U = BIT(w, 23);
    if (opc1 & 2) {
        uint8_t b = c->v.b[d * 8 + (((opc1 & 1) << 2) | opc2)];
        c->r[rt] = U ? b : (uint32_t)(int32_t)(int8_t)b;
    } else if (opc2 & 1) {
        uint16_t h = c->v.h[d * 4 + (((opc1 & 1) << 1) | (opc2 >> 1))];
        c->r[rt] = U ? h : (uint32_t)(int32_t)(int16_t)h;
    } else {
        c->r[rt] = c->v.s[d * 2 + (opc1 & 1)];
    }
}

static void vfp_other(Cpu *c, uint32_t pc, uint32_t w, uint32_t dbl, uint32_t d, uint32_t m, uint32_t vd, uint32_t vm,
                      uint32_t D, uint32_t M) {
    uint32_t opc2 = BITS(w, 16, 4), opc3 = BITS(w, 6, 2);
    if (!(opc3 & 1)) {  // VMOV immediate
        uint32_t imm8 = (BITS(w, 16, 4) << 4) | BITS(w, 0, 4), sign = imm8 >> 7, b6 = (imm8 >> 6) & 1;
        if (dbl) {
            uint64_t exp = ((uint64_t)(b6 ^ 1) << 10) | ((uint64_t)(b6 ? 0xFF : 0) << 2) | ((imm8 >> 4) & 3);
            c->v.d[d] = ((uint64_t)sign << 63) | (exp << 52) | ((uint64_t)(imm8 & 15) << 48);
        } else {
            uint32_t exp = ((b6 ^ 1) << 7) | ((b6 ? 0x1Fu : 0) << 2) | ((imm8 >> 4) & 3);
            c->v.s[d] = (sign << 31) | (exp << 23) | ((imm8 & 15) << 19);
        }
        return;
    }
    switch (opc2) {
    case 0:
        if (opc3 == 1) { if (dbl) c->v.d[d] = c->v.d[m]; else c->v.s[d] = c->v.s[m]; }
        else if (dbl) c->v.df[d] = fabs(c->v.df[m]);
        else c->v.f[d] = fabsf(c->v.f[m]);
        return;
    case 1:
        if (opc3 == 1) { if (dbl) c->v.d[d] = c->v.d[m] ^ 0x8000000000000000ull; else c->v.s[d] = c->v.s[m] ^ 0x80000000u; }
        else if (dbl) c->v.df[d] = sqrt(c->v.df[m]);
        else c->v.f[d] = sqrtf(c->v.f[m]);
        return;
    case 2: case 3: {  // VCVTB / VCVTT
        if (dbl) bad(pc, w, "vcvt half double");
        uint32_t sd = SREG(vd, D), sm = SREG(vm, M), top = BIT(w, 7);
        if (opc2 == 2) c->v.f[sd] = h2f((uint16_t)(c->v.s[sm] >> (top ? 16 : 0)));
        else if (top) c->v.s[sd] = (c->v.s[sd] & 0xFFFFu) | ((uint32_t)f2h(c->v.f[sm]) << 16);
        else c->v.s[sd] = (c->v.s[sd] & 0xFFFF0000u) | f2h(c->v.f[sm]);
        return;
    }
    case 4: case 5:  // VCMP(E)
        if (dbl) vfp_cmp_d(c, c->v.df[d], opc2 == 5 ? 0.0 : c->v.df[m]);
        else vfp_cmp_f(c, c->v.f[d], opc2 == 5 ? 0.0f : c->v.f[m]);
        return;
    case 6:  // VRINTR / VRINTZ
        if (opc3 == 3) { if (dbl) c->v.df[d] = trunc(c->v.df[m]); else c->v.f[d] = truncf(c->v.f[m]); }
        else if (dbl) c->v.df[d] = vfp_round_mode(c, c->v.df[m]);
        else c->v.f[d] = (float)vfp_round_mode(c, c->v.f[m]);
        return;
    case 7:
        if (opc3 == 1) {  // VRINTX
            if (dbl) c->v.df[d] = vfp_round_mode(c, c->v.df[m]);
            else c->v.f[d] = (float)vfp_round_mode(c, c->v.f[m]);
        } else if (dbl) {
            c->v.f[SREG(vd, D)] = (float)c->v.df[m];
        } else {
            c->v.df[DREG(vd, D)] = (double)c->v.f[m];
        }
        return;
    case 8: {  // VCVT int -> float
        uint32_t s = c->v.s[SREG(vm, M)];
        if (dbl) c->v.df[d] = BIT(w, 7) ? (double)(int32_t)s : (double)s;
        else c->v.f[d] = BIT(w, 7) ? (float)(int32_t)s : (float)s;
        return;
    }
    case 10: case 11: case 14: case 15: {  // VCVT fixed point
        uint32_t U = BIT(w, 16), to_fixed = BIT(w, 18), size = BIT(w, 7) ? 32 : 16;
        int fbits = (int)size - (int)((BITS(w, 0, 4) << 1) | BIT(w, 5));
        if (dbl) {
            uint32_t reg = DREG(vd, D);
            if (to_fixed) {
                c->v.d[reg] = U ? vcvt_fx_u(c->v.df[reg], fbits, (int)size) : vcvt_fx_s(c->v.df[reg], fbits, (int)size);
                return;
            }
            uint32_t raw = (uint32_t)c->v.d[reg];
            double src = size == 16 ? (U ? (double)(uint16_t)raw : (double)(int16_t)raw) : (U ? (double)raw : (double)(int32_t)raw);
            c->v.df[reg] = ldexp(src, -fbits);
            return;
        }
        uint32_t reg = SREG(vd, D);
        if (to_fixed) {
            uint32_t v = U ? vcvt_fx_u((double)c->v.f[reg], fbits, (int)size) : vcvt_fx_s((double)c->v.f[reg], fbits, (int)size);
            if (size == 16) v = U ? (v & 0xFFFFu) : (uint32_t)(int32_t)(int16_t)v;
            c->v.s[reg] = v;
            return;
        }
        uint32_t raw = c->v.s[reg];
        double src = size == 16 ? (U ? (double)(uint16_t)raw : (double)(int16_t)raw) : (U ? (double)raw : (double)(int32_t)raw);
        c->v.f[reg] = (float)ldexp(src, -fbits);
        return;
    }
    case 12: case 13: {  // VCVT(R) float -> int
        double src = dbl ? c->v.df[m] : (double)c->v.f[m];
        if (!BIT(w, 7)) src = vfp_round_mode(c, src);
        c->v.s[SREG(vd, D)] = opc2 == 13 ? vcvt_s32_f64(src) : vcvt_u32_f64(src);
        return;
    }
    }
    bad(pc, w, "vfp other");
}

static void vfp_dp(Cpu *c, uint32_t pc, uint32_t w) {
    uint32_t dbl = BIT(w, 8), D = BIT(w, 22), N = BIT(w, 7), M = BIT(w, 5);
    uint32_t vd = BITS(w, 12, 4), vn = BITS(w, 16, 4), vm = BITS(w, 0, 4);
    uint32_t d = dbl ? DREG(vd, D) : SREG(vd, D), n = dbl ? DREG(vn, N) : SREG(vn, N), m = dbl ? DREG(vm, M) : SREG(vm, M);
    uint32_t opc1 = (BIT(w, 23) << 2) | BITS(w, 20, 2), o6 = BIT(w, 6);
    if (opc1 == 7) { vfp_other(c, pc, w, dbl, d, m, vd, vm, D, M); return; }
    if (dbl) {
        double a = c->v.df[n], b = c->v.df[m], r = c->v.df[d];
        switch (opc1) {
        case 0: r = o6 ? r - (double)(a * b) : r + (double)(a * b); break;  // VMLA / VMLS
        case 1: r = o6 ? -r - (double)(a * b) : -r + (double)(a * b); break;  // VNMLA / VNMLS
        case 2: r = o6 ? -(double)(a * b) : a * b; break;
        case 3: r = o6 ? a - b : a + b; break;
        case 4: if (o6) bad(pc, w, "vfp dp"); r = a / b; break;
        case 5: r = o6 ? fma(-a, b, -r) : fma(a, b, -r); break;  // VFNMA / VFNMS
        case 6: r = o6 ? fma(-a, b, r) : fma(a, b, r); break;  // VFMS / VFMA
        }
        c->v.df[d] = r;
        return;
    }
    float a = c->v.f[n], b = c->v.f[m], r = c->v.f[d];
    switch (opc1) {
    case 0: r = o6 ? r - (float)(a * b) : r + (float)(a * b); break;
    case 1: r = o6 ? -r - (float)(a * b) : -r + (float)(a * b); break;
    case 2: r = o6 ? -(float)(a * b) : a * b; break;
    case 3: r = o6 ? a - b : a + b; break;
    case 4: if (o6) bad(pc, w, "vfp dp"); r = a / b; break;
    case 5: r = o6 ? fmaf(-a, b, -r) : fmaf(a, b, -r); break;
    case 6: r = o6 ? fmaf(-a, b, r) : fmaf(a, b, r); break;
    }
    c->v.f[d] = r;
}

static void vfp(Cpu *c, uint32_t pc, uint32_t w) {
    uint32_t op = BITS(w, 25, 3);
    if (op == 6) {
        if (BITS(w, 21, 4) == 2) vfp_mov64(c, pc, w);
        else vfp_ldst(c, pc, w);
        return;
    }
    if (BITS(w, 24, 4) == 0xE) {
        if (BIT(w, 4)) vfp_mov32(c, pc, w);
        else vfp_dp(c, pc, w);
        return;
    }
    bad(pc, w, "vfp");
}

static void vfp_v8(Cpu *c, uint32_t pc, uint32_t w) {  // VSEL, VMAXNM / VMINNM, VRINTA/N/P/M, VCVTA/N/P/M
    uint32_t dbl = BIT(w, 8), D = BIT(w, 22), N = BIT(w, 7), M = BIT(w, 5);
    uint32_t vd = BITS(w, 12, 4), vn = BITS(w, 16, 4), vm = BITS(w, 0, 4);
    uint32_t d = dbl ? DREG(vd, D) : SREG(vd, D), n = dbl ? DREG(vn, N) : SREG(vn, N), m = dbl ? DREG(vm, M) : SREG(vm, M);
    if (!BIT(w, 23)) {  // VSEL
        uint32_t cc = BITS(w, 20, 2);
        int t = cc == 0 ? COND_EQ : cc == 1 ? COND_VS : cc == 2 ? COND_GE : COND_GT;
        if (dbl) c->v.d[d] = t ? c->v.d[n] : c->v.d[m];
        else c->v.s[d] = t ? c->v.s[n] : c->v.s[m];
        return;
    }
    if (BITS(w, 20, 2) == 0) {
        if (dbl) c->v.df[d] = BIT(w, 6) ? vminnm_d(c->v.df[n], c->v.df[m]) : vmaxnm_d(c->v.df[n], c->v.df[m]);
        else c->v.f[d] = BIT(w, 6) ? vminnm_f(c->v.f[n], c->v.f[m]) : vmaxnm_f(c->v.f[n], c->v.f[m]);
        return;
    }
    uint32_t rm = BITS(w, 16, 2);
    if (BITS(w, 18, 4) == 0xE && BITS(w, 6, 2) == 1) {  // VRINTA/N/P/M
        if (dbl) {
            double x = c->v.df[m];
            c->v.df[d] = rm == 0 ? round(x) : rm == 1 ? nearbyint(x) : rm == 2 ? ceil(x) : floor(x);
        } else {
            float x = c->v.f[m];
            c->v.f[d] = rm == 0 ? roundf(x) : rm == 1 ? nearbyintf(x) : rm == 2 ? ceilf(x) : floorf(x);
        }
        return;
    }
    if (BITS(w, 18, 4) == 0xF && BIT(w, 6)) {  // VCVTA/N/P/M to integer
        double x = dbl ? c->v.df[m] : (double)c->v.f[m];
        x = rm == 0 ? round(x) : rm == 1 ? nearbyint(x) : rm == 2 ? ceil(x) : floor(x);
        c->v.s[SREG(vd, D)] = BIT(w, 7) ? vcvt_s32_f64(x) : vcvt_u32_f64(x);
        return;
    }
    bad(pc, w, "vfp v8");
}

// ================================================================ coprocessor / svc
static void cp15(Cpu *c, uint32_t pc, uint32_t w) {
    uint32_t rt = BITS(w, 12, 4);
    if (BITS(w, 25, 3) == 7 && BIT(w, 4)) {  // MRC / MCR
        uint32_t L = BIT(w, 20), opc1 = BITS(w, 21, 3), crn = BITS(w, 16, 4), crm = BITS(w, 0, 4), opc2 = BITS(w, 5, 3);
        if (L) {
            if (opc1 == 0 && crn == 13 && crm == 0 && opc2 == 3) c->r[rt] = c->tpidruro;
            else if (opc1 == 0 && crn == 13 && crm == 0 && opc2 == 2) c->r[rt] = c->tpidrurw;
            else if (opc1 == 0 && crn == 14 && crm == 0 && opc2 == 0) c->r[rt] = 19200000u;
            else bad(pc, w, "mrc p15");
            return;
        }
        if (opc1 == 0 && crn == 13 && crm == 0 && opc2 == 2) { c->tpidrurw = RR(c, pc, rt); return; }
        if (crn == 7) { __atomic_thread_fence(__ATOMIC_SEQ_CST); return; }
        bad(pc, w, "mcr p15");
    }
    if (BITS(w, 21, 7) == 0x62) {  // MRRC / MCRR
        uint32_t opc1 = BITS(w, 4, 4), crm = BITS(w, 0, 4), rt2 = BITS(w, 16, 4);
        if (BIT(w, 20) && crm == 14 && opc1 <= 1) {  // CNTPCT / CNTVCT
            uint64_t t = rt_ticks();
            c->r[rt] = (uint32_t)t;
            c->r[rt2] = (uint32_t)(t >> 32);
            return;
        }
    }
    bad(pc, w, "cp15");
}

static uint32_t coproc_svc(Cpu *c, uint32_t pc, uint32_t w) {
    if (BITS(w, 24, 4) == 0xF) {
        rt_svc(c, BITS(w, 0, 24), pc);
        return pc + 4;
    }
    uint32_t cp = BITS(w, 8, 4);
    if (cp == 10 || cp == 11) vfp(c, pc, w);
    else if (cp == 15) cp15(c, pc, w);
    else bad(pc, w, "coprocessor");
    return pc + 4;
}

// ================================================================ Advanced SIMD
#define QREG(v, x) (((x) << 4) | (v))

static inline uint64_t lget(Cpu *c, uint32_t d, uint32_t i, uint32_t size) {
    switch (size) {
    case 8: return c->v.b[d * 8 + i];
    case 16: return c->v.h[d * 4 + i];
    case 32: return c->v.s[d * 2 + i];
    default: return c->v.d[d + i];
    }
}
static inline void lset(Cpu *c, uint32_t d, uint32_t i, uint32_t size, uint64_t v) {
    switch (size) {
    case 8: c->v.b[d * 8 + i] = (uint8_t)v; break;
    case 16: c->v.h[d * 4 + i] = (uint16_t)v; break;
    case 32: c->v.s[d * 2 + i] = (uint32_t)v; break;
    default: c->v.d[d + i] = v; break;
    }
}
static inline int64_t sx(uint64_t v, uint32_t size) { return size == 64 ? (int64_t)v : (int64_t)(v << (64 - size)) >> (64 - size); }
static inline uint64_t lmask(uint32_t size) { return size == 64 ? ~0ull : (1ull << size) - 1; }
static inline float fin(Cpu *c, uint32_t d, uint32_t i) { return nftz(c->v.f[d * 2 + i]); }

static void three_same(Cpu *c, uint32_t pc, uint32_t w) {
    uint32_t A = BITS(w, 8, 4), B = BIT(w, 4), C = BITS(w, 20, 2), U = BIT(w, 24), Q = BIT(w, 6);
    uint32_t d = QREG(BITS(w, 12, 4), BIT(w, 22)), n = QREG(BITS(w, 16, 4), BIT(w, 7)), m = QREG(BITS(w, 0, 4), BIT(w, 5));
    uint32_t nd = Q ? 2 : 1, b21 = BIT(w, 21);
    if (A == 1 && B == 1) {  // bitwise
        uint64_t t[2];
        for (uint32_t i = 0; i < nd; i++) {
            uint64_t a = c->v.d[n + i], b = c->v.d[m + i], r = c->v.d[d + i];
            switch ((U << 2) | C) {
            case 0: t[i] = a & b; break;
            case 1: t[i] = a & ~b; break;
            case 2: t[i] = a | b; break;
            case 3: t[i] = a | ~b; break;
            case 4: t[i] = a ^ b; break;
            case 5: t[i] = (r & a) | (~r & b); break;  // VBSL
            case 6: t[i] = (a & b) | (r & ~b); break;  // VBIT
            default: t[i] = (r & b) | (a & ~b); break;  // VBIF
            }
        }
        for (uint32_t i = 0; i < nd; i++) c->v.d[d + i] = t[i];
        return;
    }
    uint32_t size = 8u << C;
    // float lanes
    int fop = -1;  // 0 add 1 sub 2 mul 3 mla 4 mls 5 abd 6 max 7 min 8 recps 9 rsqrts
    if (A == 0xD && !B && !U) { if (BIT(w, 20)) bad(pc, w, "f16"); fop = b21 ? 1 : 0; }
    else if (A == 0xD && B && U && !b21) fop = 2;
    else if (A == 0xD && B && !U) fop = b21 ? 4 : 3;
    else if (A == 0xD && !B && U && b21) fop = 5;
    else if (A == 0xF && !B && !U) fop = b21 ? 7 : 6;
    else if (A == 0xF && B && !U) fop = b21 ? 9 : 8;
    if (fop >= 0) {
        uint32_t k = Q ? 4 : 2;
        float t[4];
        for (uint32_t i = 0; i < k; i++) {
            float a = fin(c, n, i), b = fin(c, m, i), e;
            switch (fop) {
            case 0: e = a + b; break;
            case 1: e = a - b; break;
            case 2: e = a * b; break;
            case 3: e = fin(c, d, i) + a * b; break;
            case 4: e = fin(c, d, i) - a * b; break;
            case 5: e = fabsf(a - b); break;
            case 6: e = vmax_f(a, b); break;
            case 7: e = vmin_f(a, b); break;
            case 8: e = vrecps_f(a, b); break;
            default: e = vrsqrts_f(a, b); break;
            }
            t[i] = nftz(e);
        }
        for (uint32_t i = 0; i < k; i++) c->v.f[d * 2 + i] = t[i];
        return;
    }
    if ((A == 0xD && !B && U && !b21) || (A == 0xF && !B && U)) {  // VPADD / VPMAX / VPMIN float
        float a0 = fin(c, n, 0), a1 = fin(c, n, 1), b0 = fin(c, m, 0), b1 = fin(c, m, 1), t0, t1;
        if (A == 0xD) { t0 = nftz(a0 + a1); t1 = nftz(b0 + b1); }
        else if (b21) { t0 = nftz(vmin_f(a0, a1)); t1 = nftz(vmin_f(b0, b1)); }
        else { t0 = nftz(vmax_f(a0, a1)); t1 = nftz(vmax_f(b0, b1)); }
        c->v.f[d * 2] = t0;
        c->v.f[d * 2 + 1] = t1;
        return;
    }
    if (A == 0xE && !B) {  // VCEQ / VCGE / VCGT float
        uint32_t k = Q ? 4 : 2, t[4];
        if (!U && b21) bad(pc, w, "vc float");
        for (uint32_t i = 0; i < k; i++) {
            float a = fin(c, n, i), b = fin(c, m, i);
            int r = !U ? a == b : !b21 ? a >= b : a > b;
            t[i] = r ? 0xFFFFFFFFu : 0;
        }
        for (uint32_t i = 0; i < k; i++) c->v.s[d * 2 + i] = t[i];
        return;
    }
    if ((A == 0xB && B && !U) || (A == 0xA && size < 64)) {  // VPADD / VPMAX / VPMIN integer
        uint32_t per = 64 / size;
        uint64_t vals[16], t[8];
        for (uint32_t i = 0; i < per; i++) { vals[i] = lget(c, n, i, size); vals[per + i] = lget(c, m, i, size); }
        for (uint32_t i = 0; i < per; i++) {
            uint64_t a = vals[2 * i], b = vals[2 * i + 1];
            bool gt = U ? a > b : sx(a, size) > sx(b, size);
            t[i] = A == 0xB ? (a + b) & lmask(size) : (B ? !gt : gt) ? a : b;
        }
        for (uint32_t i = 0; i < per; i++) lset(c, d, i, size, t[i]);
        return;
    }
    uint32_t cnt = (Q ? 128 : 64) / size;
    uint64_t t[16], mk = lmask(size);
    for (uint32_t i = 0; i < cnt; i++) {
        uint64_t a = lget(c, n, i, size), b = lget(c, m, i, size), r;
        int64_t sa = sx(a, size), sb = sx(b, size);
        if (A == 8 && !B) r = U ? a - b : a + b;  // VADD / VSUB
        else if (A == 9 && B && !U) r = a * b;  // VMUL
        else if (A == 9 && !B) r = U ? lget(c, d, i, size) - a * b : lget(c, d, i, size) + a * b;  // VMLA / VMLS
        else if (A == 8 && B) r = U ? (a == b ? mk : 0) : ((a & b) ? mk : 0);  // VCEQ / VTST
        else if (A == 3) r = (U ? (B ? a >= b : a > b) : (B ? sa >= sb : sa > sb)) ? mk : 0;  // VCGT / VCGE
        else if (A == 6) r = U ? ((B ? a < b : a > b) ? a : b) : ((B ? sa < sb : sa > sb) ? a : b);  // VMAX / VMIN
        else if ((A == 0 || A == 2) && B) {  // VQADD / VQSUB
            if (size == 64) bad(pc, w, "vqadd 64");
            int64_t hi = U ? (int64_t)((1ull << size) - 1) : (int64_t)((1ull << (size - 1)) - 1);
            int64_t lo = U ? 0 : -(int64_t)(1ull << (size - 1));
            int64_t ca = U ? (int64_t)a : sa, cb = U ? (int64_t)b : sb, s = A == 0 ? ca + cb : ca - cb;
            if (s > hi) { s = hi; c->fpscr |= 1u << 27; }
            if (s < lo) { s = lo; c->fpscr |= 1u << 27; }
            r = (uint64_t)s;
        } else if (A == 4 && !B) {  // VSHL (register): value m, shift n
            int s = (int8_t)a;
            uint64_t v = b;
            if (s >= 0) r = s >= (int)size ? 0 : v << s;
            else if (U) r = -s >= (int)size ? 0 : v >> -s;
            else r = (uint64_t)(sx(v, size) >> (-s >= (int)size ? (int)size - 1 : -s));
        } else if (A == 1 && !B) {  // VRHADD
            r = U ? (a + b + 1) >> 1 : (uint64_t)((sa + sb + 1) >> 1);
        } else if (A == 0 && !B) {  // VHADD
            r = U ? (a + b) >> 1 : (uint64_t)((sa + sb) >> 1);
        } else if (A == 7 && !B) {  // VABD
            r = U ? (a > b ? a - b : b - a) : (sa > sb ? a - b : b - a);
        } else {
            bad(pc, w, "neon 3same");
        }
        t[i] = r & mk;
    }
    for (uint32_t i = 0; i < cnt; i++) lset(c, d, i, size, t[i]);
}

static void modified_imm(Cpu *c, uint32_t pc, uint32_t w) {
    uint32_t Q = BIT(w, 6), d = QREG(BITS(w, 12, 4), BIT(w, 22)), cmode = BITS(w, 8, 4), op = BIT(w, 5);
    uint64_t imm8 = (BIT(w, 24) << 7) | (BITS(w, 16, 3) << 4) | BITS(w, 0, 4), v, v64;
    int kind;  // 0 mov 1 orr 2 bic
    if (cmode < 8) {
        v = imm8 << (8 * (cmode >> 1));
        v64 = v | (v << 32);
        kind = cmode & 1;
    } else if (cmode < 12) {
        v = imm8 << (8 * ((cmode >> 1) & 1));
        v64 = v | (v << 16) | (v << 32) | (v << 48);
        kind = cmode & 1;
    } else if (cmode < 14) {
        v = cmode == 12 ? (imm8 << 8 | 0xFF) : (imm8 << 16 | 0xFFFF);
        v64 = v | (v << 32);
        kind = 0;
    } else if (cmode == 14) {
        v64 = 0;
        for (int i = 0; i < 8; i++) v64 |= (op ? ((imm8 >> i) & 1 ? 0xFFull : 0) : imm8) << (8 * i);
        op = 0;
        kind = 0;
    } else {
        if (op) bad(pc, w, "vmov f64 imm");
        uint64_t b6 = (imm8 >> 6) & 1, exp = ((b6 ^ 1) << 7) | ((b6 ? 0x1F : 0) << 2) | ((imm8 >> 4) & 3);
        v = ((imm8 >> 7) << 31) | (exp << 23) | ((imm8 & 15) << 19);
        v64 = v | (v << 32);
        kind = 0;
    }
    if (op && kind == 0) v64 = ~v64;  // VMVN
    if (op && kind == 1) kind = 2;
    for (uint32_t i = 0; i < (Q ? 2u : 1u); i++) {
        if (kind == 0) c->v.d[d + i] = v64;
        else if (kind == 1) c->v.d[d + i] |= v64;
        else c->v.d[d + i] &= ~v64;
    }
}

static uint64_t mul_wide(uint64_t a, uint64_t b, uint32_t size, int sgn) {
    uint64_t x = sgn ? (uint64_t)sx(a, size) : a, y = sgn ? (uint64_t)sx(b, size) : b;
    return x * y;
}

static void three_diff(Cpu *c, uint32_t pc, uint32_t w, uint32_t U) {  // VMULL / VMLAL / VMLSL
    uint32_t A = BITS(w, 8, 4), size = 8u << BITS(w, 20, 2);
    uint32_t d = QREG(BITS(w, 12, 4), BIT(w, 22)), n = QREG(BITS(w, 16, 4), BIT(w, 7)), m = QREG(BITS(w, 0, 4), BIT(w, 5));
    if (A != 0xC && A != 8 && A != 0xA) bad(pc, w, "neon 3diff");
    uint32_t wide = size * 2, cnt = 64 / size;
    uint64_t t[8];
    for (uint32_t i = 0; i < cnt; i++) {
        uint64_t p = mul_wide(lget(c, n, i, size), lget(c, m, i, size), size, !U);
        if (A == 8) p = lget(c, d, i, wide) + p;
        else if (A == 0xA) p = lget(c, d, i, wide) - p;
        t[i] = p & lmask(wide);
    }
    for (uint32_t i = 0; i < cnt; i++) lset(c, d, i, wide, t[i]);
}

static void two_scalar(Cpu *c, uint32_t pc, uint32_t w, uint32_t Q) {  // by-scalar forms (Q is bit 24)
    uint32_t A = BITS(w, 8, 4), size = 8u << BITS(w, 20, 2);
    uint32_t d = QREG(BITS(w, 12, 4), BIT(w, 22)), n = QREG(BITS(w, 16, 4), BIT(w, 7)), m, idx;
    if (size == 16) { m = BITS(w, 0, 3); idx = (BIT(w, 5) << 1) | BIT(w, 3); }
    else { m = BITS(w, 0, 4); idx = BIT(w, 5); }
    uint32_t F = A & 1, op = A >> 2, cnt = (Q ? 128 : 64) / size;
    if (A == 0 || A == 1 || A == 4 || A == 5 || A == 8 || A == 9) {
        if (F) {
            float s = nftz(c->v.f[m * 2 + idx]), t[4];
            for (uint32_t i = 0; i < cnt; i++) {
                float prod = fin(c, n, i) * s;
                t[i] = nftz(op == 0 ? fin(c, d, i) + prod : op == 1 ? fin(c, d, i) - prod : prod);
            }
            for (uint32_t i = 0; i < cnt; i++) c->v.f[d * 2 + i] = t[i];
            return;
        }
        uint64_t s = lget(c, m, idx, size), t[8];
        for (uint32_t i = 0; i < cnt; i++) {
            uint64_t prod = lget(c, n, i, size) * s;
            t[i] = (op == 0 ? lget(c, d, i, size) + prod : op == 1 ? lget(c, d, i, size) - prod : prod) & lmask(size);
        }
        for (uint32_t i = 0; i < cnt; i++) lset(c, d, i, size, t[i]);
        return;
    }
    if (A == 2 || A == 6 || A == 0xA) {  // VMLAL / VMLSL / VMULL by scalar (U = bit 24)
        uint32_t wide = size * 2, k = 64 / size;
        uint64_t t[4], s = lget(c, m, idx, size);
        for (uint32_t i = 0; i < k; i++) {
            uint64_t p = mul_wide(lget(c, n, i, size), s, size, !Q);
            if (A == 2) p = lget(c, d, i, wide) + p;
            else if (A == 6) p = lget(c, d, i, wide) - p;
            t[i] = p & lmask(wide);
        }
        for (uint32_t i = 0; i < k; i++) lset(c, d, i, wide, t[i]);
        return;
    }
    bad(pc, w, "neon scalar");
}

static void vdup_core(Cpu *c, uint32_t w, uint32_t src) {
    uint32_t Q = BIT(w, 21), d = QREG(BITS(w, 16, 4), BIT(w, 7)), be = (BIT(w, 22) << 1) | BIT(w, 5);
    uint32_t size = be == 0 ? 32 : be == 1 ? 16 : be == 2 ? 8 : 0;
    if (!size) bad(0, w, "vdup size");
    for (uint32_t i = 0; i < (Q ? 128 : 64) / size; i++) lset(c, d, i, size, src);
}

static void two_misc(Cpu *c, uint32_t pc, uint32_t w) {
    uint32_t size = 8u << BITS(w, 18, 2), A = BITS(w, 16, 2), op = BITS(w, 7, 4), Q = BIT(w, 6);
    uint32_t d = QREG(BITS(w, 12, 4), BIT(w, 22)), m = QREG(BITS(w, 0, 4), BIT(w, 5)), nd = Q ? 2 : 1;
    uint32_t cnt = (Q ? 128 : 64) / size;
    uint64_t t[16], mk = lmask(size);
    if (A == 0 && op == 0xB) {  // VMVN
        for (uint32_t i = 0; i < nd; i++) t[i] = ~c->v.d[m + i];
        for (uint32_t i = 0; i < nd; i++) c->v.d[d + i] = t[i];
        return;
    }
    if (A == 2 && (op == 2 || op == 3)) {  // VUZP / VZIP
        uint64_t a[16], b[16], r[32];
        for (uint32_t i = 0; i < cnt; i++) { a[i] = lget(c, d, i, size); b[i] = lget(c, m, i, size); }
        for (uint32_t i = 0; i < cnt; i++) {
            if (op == 3) { r[2 * i] = a[i]; r[2 * i + 1] = b[i]; }
            else {
                uint32_t e = 2 * i, o = 2 * i + 1;
                r[i] = e < cnt ? a[e] : b[e - cnt];
                r[cnt + i] = o < cnt ? a[o] : b[o - cnt];
            }
        }
        for (uint32_t i = 0; i < cnt; i++) { lset(c, d, i, size, r[i]); lset(c, m, i, size, r[cnt + i]); }
        return;
    }
    if (A == 2 && (op == 4 || op == 5) && !BIT(w, 6)) {  // VMOVN
        uint32_t k = 64 / size;
        for (uint32_t i = 0; i < k; i++) t[i] = lget(c, m, i, size * 2) & mk;
        for (uint32_t i = 0; i < k; i++) lset(c, d, i, size, t[i]);
        return;
    }
    if (A == 2 && op == 1) {  // VTRN
        for (uint32_t i = 0; i < cnt; i += 2) {
            uint64_t x = lget(c, d, i + 1, size), y = lget(c, m, i, size);
            lset(c, d, i + 1, size, y);
            lset(c, m, i, size, x);
        }
        return;
    }
    if (A == 3 && BITS(w, 9, 2) == 3 && size == 32) {  // VCVT float <-> integer
        uint32_t opb = BITS(w, 7, 2), k = Q ? 4 : 2;
        for (uint32_t i = 0; i < k; i++) {
            uint32_t s = c->v.s[m * 2 + i];
            t[i] = opb == 0 ? f2u((float)(int32_t)s) : opb == 1 ? f2u((float)s) : opb == 2 ? vcvt_s32_f32(u2f(s)) : vcvt_u32_f32(u2f(s));
        }
        for (uint32_t i = 0; i < k; i++) c->v.s[d * 2 + i] = (uint32_t)t[i];
        return;
    }
    for (uint32_t i = 0; i < cnt; i++) {
        uint64_t a = lget(c, m, i, size), r;
        int64_t sa = sx(a, size);
        if (A == 0 && op == 0xA && size == 8) r = (uint64_t)__builtin_popcount((uint32_t)a);  // VCNT
        else if (A == 0 && op <= 2) {  // VREV64 / VREV32 / VREV16
            uint32_t per = (op == 0 ? 64 : op == 1 ? 32 : 16) / size;
            r = lget(c, m, (i / per) * per + (per - 1 - i % per), size);
        } else if (A == 1 && (op == 6 || op == 7) && size == 32 && BIT(w, 10)) r = op == 6 ? a & 0x7FFFFFFFu : a ^ 0x80000000u;
        else if (A == 1 && (op == 6 || op == 7) && !BIT(w, 10)) r = op == 6 ? (uint64_t)(sa < 0 ? -sa : sa) : (uint64_t)-sa;
        else if (A == 3 && BITS(w, 8, 3) == 5 && size == 32) {  // VRECPE / VRSQRTE float
            if (!BIT(w, 8)) bad(pc, w, "vrecpe int");
            r = f2u(BIT(w, 7) ? vrsqrte_f(u2f((uint32_t)a)) : vrecpe_f(u2f((uint32_t)a)));
        } else if (A == 1 && op <= 4 && BIT(w, 10) && size == 32) {  // VCGT / VCGE / VCEQ / VCLE / VCLT #0 (float)
            float f = nftz(u2f((uint32_t)a));
            int x = op == 0 ? f > 0.0f : op == 1 ? f >= 0.0f : op == 2 ? f == 0.0f : op == 3 ? f <= 0.0f : f < 0.0f;
            r = x ? 0xFFFFFFFFu : 0;
        } else if (A == 1 && op <= 4 && !BIT(w, 10)) {  // ... #0 (integer)
            int x = op == 0 ? sa > 0 : op == 1 ? sa >= 0 : op == 2 ? sa == 0 : op == 3 ? sa <= 0 : sa < 0;
            r = x ? mk : 0;
        } else {
            bad(pc, w, "neon 2misc");
        }
        t[i] = r & mk;
    }
    for (uint32_t i = 0; i < cnt; i++) lset(c, d, i, size, t[i]);
}

static void shift_imm(Cpu *c, uint32_t pc, uint32_t w) {  // VSHR / VSRA / VSHL / VSHLL / VSHRN / VMOVL
    uint32_t U = BIT(w, 24), L = BIT(w, 7), imm6 = BITS(w, 16, 6), A = BITS(w, 8, 4), Q = BIT(w, 6);
    uint32_t d = QREG(BITS(w, 12, 4), BIT(w, 22)), m = QREG(BITS(w, 0, 4), BIT(w, 5)), size, shift;
    if (L) { size = 64; shift = imm6; }
    else {
        size = imm6 >= 32 ? 32 : imm6 >= 16 ? 16 : imm6 >= 8 ? 8 : 0;
        if (!size) bad(pc, w, "neon shift size");
        shift = imm6 - size;
    }
    uint64_t t[16], mk = lmask(size);
    uint32_t cnt = (Q ? 128 : 64) / size;
    if (A == 0 || A == 1) {  // VSHR / VSRA
        uint32_t amt = L ? 64 - imm6 : size - shift;
        for (uint32_t i = 0; i < cnt; i++) {
            uint64_t a = lget(c, m, i, size);
            uint64_t s = U ? (amt < size ? a >> amt : 0) : (uint64_t)(sx(a, size) >> (amt < size - 1 ? amt : size - 1));
            t[i] = ((A == 1 ? lget(c, d, i, size) : 0) + s) & mk;
        }
        for (uint32_t i = 0; i < cnt; i++) lset(c, d, i, size, t[i]);
        return;
    }
    if (A == 5 && !U) {  // VSHL immediate
        uint32_t amt = L ? imm6 : shift;
        for (uint32_t i = 0; i < cnt; i++) t[i] = (lget(c, m, i, size) << amt) & mk;
        for (uint32_t i = 0; i < cnt; i++) lset(c, d, i, size, t[i]);
        return;
    }
    if (A == 10 && !L && !Q) {  // VSHLL / VMOVL
        uint32_t wide = size * 2, k = 64 / size;
        for (uint32_t i = 0; i < k; i++) {
            uint64_t a = lget(c, m, i, size);
            t[i] = ((U ? a : (uint64_t)sx(a, size)) << shift) & lmask(wide);
        }
        for (uint32_t i = 0; i < k; i++) lset(c, d, i, wide, t[i]);
        return;
    }
    if (A == 8 && !U && !L && !BIT(w, 6)) {  // VSHRN
        uint32_t wide = size * 2, amt = wide - imm6, k = 64 / size;
        for (uint32_t i = 0; i < k; i++) t[i] = (lget(c, m, i, wide) >> amt) & mk;
        for (uint32_t i = 0; i < k; i++) lset(c, d, i, size, t[i]);
        return;
    }
    bad(pc, w, "neon shift");
}

static void neon_dp(Cpu *c, uint32_t pc, uint32_t w) {
    uint32_t U = BIT(w, 24);
    if (!BIT(w, 23)) { three_same(c, pc, w); return; }
    if (BITS(w, 19, 3) == 0 && !BIT(w, 7) && BIT(w, 4)) { modified_imm(c, pc, w); return; }
    if (BITS(w, 20, 2) != 3 && !BIT(w, 4)) {
        if (!BIT(w, 6)) three_diff(c, pc, w, U);
        else two_scalar(c, pc, w, U);
        return;
    }
    uint32_t d = QREG(BITS(w, 12, 4), BIT(w, 22)), m = QREG(BITS(w, 0, 4), BIT(w, 5));
    if (U && BITS(w, 20, 2) == 3 && BITS(w, 16, 4) == 0 && BITS(w, 8, 3) == 3 && !BIT(w, 4)) {  // AES
        switch (BITS(w, 6, 2)) {
        case 0: neon_aese(c, (int)d, (int)m); break;
        case 1: neon_aesd(c, (int)d, (int)m); break;
        case 2: neon_aesmc(c, (int)d, (int)m); break;
        default: neon_aesimc(c, (int)d, (int)m); break;
        }
        return;
    }
    if (BITS(w, 20, 2) == 3 && !U && !BIT(w, 4)) {  // VEXT
        uint32_t Q = BIT(w, 6), n = QREG(BITS(w, 16, 4), BIT(w, 7)), imm = BITS(w, 8, 4), nb = Q ? 16 : 8;
        uint8_t src[32], t[16];
        for (uint32_t i = 0; i < nb; i++) { src[i] = c->v.b[n * 8 + i]; src[nb + i] = c->v.b[m * 8 + i]; }
        for (uint32_t i = 0; i < nb; i++) t[i] = src[imm + i];
        for (uint32_t i = 0; i < nb; i++) c->v.b[d * 8 + i] = t[i];
        return;
    }
    if (U && BITS(w, 20, 2) == 3 && BITS(w, 10, 2) != 3 && !BIT(w, 4) && BITS(w, 8, 4) <= 0xB) {
        two_misc(c, pc, w);
        return;
    }
    if (U && BITS(w, 20, 2) == 3 && BITS(w, 10, 2) == 2) {  // VTBL / VTBX
        uint32_t n = QREG(BITS(w, 16, 4), BIT(w, 7)), length = BITS(w, 8, 2) + 1, tbx = BIT(w, 6);
        uint8_t tab[32], t[8];
        for (uint32_t i = 0; i < length; i++) memcpy(tab + 8 * i, &c->v.d[(n + i) % 32], 8);
        for (uint32_t i = 0; i < 8; i++) {
            uint8_t idx = c->v.b[m * 8 + i];
            t[i] = idx < 8 * length ? tab[idx] : tbx ? c->v.b[d * 8 + i] : 0;
        }
        for (uint32_t i = 0; i < 8; i++) c->v.b[d * 8 + i] = t[i];
        return;
    }
    if (U && BITS(w, 20, 2) == 3 && BITS(w, 7, 5) == 0x18) {  // VDUP (scalar)
        uint32_t Q = BIT(w, 6), imm4 = BITS(w, 16, 4), size, idx;
        if (imm4 & 1) { size = 8; idx = imm4 >> 1; }
        else if (imm4 & 2) { size = 16; idx = imm4 >> 2; }
        else if (imm4 & 4) { size = 32; idx = imm4 >> 3; }
        else bad(pc, w, "vdup scalar size");
        uint64_t v = lget(c, m, idx, size);
        for (uint32_t i = 0; i < (Q ? 128 : 64) / size; i++) lset(c, d, i, size, v);
        return;
    }
    if ((BIT(w, 4) && !BIT(w, 7)) || (BIT(w, 7) && BIT(w, 4))) { shift_imm(c, pc, w); return; }
    bad(pc, w, "neon dp");
}

static void neon_ldst(Cpu *c, uint32_t pc, uint32_t w) {
    uint32_t Abit = BIT(w, 23), L = BIT(w, 21), rn = BITS(w, 16, 4), rm = BITS(w, 0, 4);
    uint32_t vd = QREG(BITS(w, 12, 4), BIT(w, 22)), ea = RR(c, pc, rn), nbytes;
    if (!Abit) {
        uint32_t typ = BITS(w, 8, 4), size = 8u << BITS(w, 6, 2), nelem, per_elem, step;
        switch (typ) {  // (elements per structure, d registers per element, register step between elements)
        case 7: nelem = 1; per_elem = 1; step = 1; break;
        case 10: nelem = 1; per_elem = 2; step = 1; break;
        case 6: nelem = 1; per_elem = 3; step = 1; break;
        case 2: nelem = 1; per_elem = 4; step = 1; break;
        case 8: nelem = 2; per_elem = 1; step = 1; break;
        case 9: nelem = 2; per_elem = 1; step = 2; break;
        case 3: nelem = 2; per_elem = 2; step = 2; break;
        case 4: nelem = 3; per_elem = 1; step = 1; break;
        case 5: nelem = 3; per_elem = 1; step = 2; break;
        case 0: nelem = 4; per_elem = 1; step = 1; break;
        case 1: nelem = 4; per_elem = 1; step = 2; break;
        default: bad(pc, w, "neon ldst type");
        }
        if (nelem == 1) {  // VLD1 / VST1: consecutive registers, contiguous memory
            for (uint32_t i = 0; i < per_elem; i++) {
                uint32_t r = (vd + i) % 32;
                if (L) c->v.d[r] = LD64(ea + 8 * i);
                else ST64(ea + 8 * i, c->v.d[r]);
            }
            nbytes = 8 * per_elem;
        } else {
            uint32_t esz = size / 8, per = 64 / size;
            for (uint32_t k = 0; k < per * per_elem; k++)
                for (uint32_t j = 0; j < nelem; j++) {
                    uint32_t r = (vd + j * step + k / per) % 32, a = ea + (k * nelem + j) * esz;
                    if (L) lset(c, r, k % per, size, size == 8 ? LD8(a) : size == 16 ? LD16(a) : size == 32 ? LD32(a) : LD64(a));
                    else {
                        uint64_t v = lget(c, r, k % per, size);
                        if (size == 8) ST8(a, v);
                        else if (size == 16) ST16(a, v);
                        else if (size == 32) ST32(a, v);
                        else ST64(a, v);
                    }
                }
            nbytes = 8 * nelem * per_elem;
        }
    } else {
        uint32_t size_bits = BITS(w, 10, 2), nelem = BITS(w, 8, 2) + 1;
        if (size_bits == 3) {  // load to all lanes
            if (!L) bad(pc, w, "vst all lanes");
            uint32_t size = 8u << BITS(w, 6, 2), T = BIT(w, 5), esz = size / 8;
            uint32_t inc = T && nelem > 1 ? 2 : 1, nregs = nelem == 1 ? (T ? 2 : 1) : 1;
            for (uint32_t j = 0; j < nelem; j++) {
                uint32_t a = ea + j * esz;
                uint64_t v = size == 8 ? LD8(a) : size == 16 ? LD16(a) : size == 32 ? LD32(a) : LD64(a);
                for (uint32_t ri = 0; ri < (nelem > 1 ? 1 : nregs); ri++) {
                    uint32_t r = nelem > 1 ? (vd + j * inc) % 32 : (vd + ri) % 32;
                    for (uint32_t k = 0; k < 64 / size; k++) lset(c, r, k, size, v);
                }
            }
            nbytes = nelem * esz;
        } else {
            uint32_t size = 8u << size_bits, idx_bits = BITS(w, 4, 4), idx = idx_bits >> (size_bits + 1), inc = 1, esz = size / 8;
            if (size == 16 && (idx_bits & 2)) inc = 2;
            if (size == 32 && (idx_bits & 4)) inc = 2;
            for (uint32_t j = 0; j < nelem; j++) {
                uint32_t r = (vd + j * inc) % 32, a = ea + j * esz;
                if (L) lset(c, r, idx, size, size == 8 ? LD8(a) : size == 16 ? LD16(a) : LD32(a));
                else {
                    uint64_t v = lget(c, r, idx, size);
                    if (size == 8) ST8(a, v);
                    else if (size == 16) ST16(a, v);
                    else ST32(a, v);
                }
            }
            nbytes = nelem * esz;
        }
    }
    if (rm == 13) c->r[rn] = ea + nbytes;
    else if (rm != 15) c->r[rn] = ea + RR(c, pc, rm);
}

// ================================================================ top level
static uint32_t uncond(Cpu *c, uint32_t pc, uint32_t w) {
    if (BITS(w, 25, 7) == 0x7D) rt_thumb(c, pc);  // BLX imm: switches to Thumb
    if (BITS(w, 25, 7) == 0x79) { neon_dp(c, pc, w); return pc + 4; }
    if (BITS(w, 24, 8) == 0xF4 && !BIT(w, 20)) { neon_ldst(c, pc, w); return pc + 4; }
    if ((w & 0xFFFFFF00u) == 0xF57FF000u) {
        uint32_t op = BITS(w, 4, 4);
        if (op == 1) { c->excl_addr = ~0u; return pc + 4; }  // CLREX
        if (op == 4 || op == 5) { __atomic_thread_fence(__ATOMIC_SEQ_CST); return pc + 4; }  // DSB / DMB
        if (op == 6) return pc + 4;  // ISB
    }
    if (BITS(w, 26, 6) == 0x3D && (BITS(w, 20, 3) == 1 || BITS(w, 20, 3) == 5) && BITS(w, 12, 4) == 15) return pc + 4;  // PLD
    if (BITS(w, 24, 8) == 0xFE && BITS(w, 9, 3) == 5 && !BIT(w, 4)) { vfp_v8(c, pc, w); return pc + 4; }
    if (BITS(w, 20, 12) == 0xF10) return pc + 4;  // CPS / SETEND
    bad(pc, w, "unconditional");
}

static uint32_t dp_misc(Cpu *c, uint32_t pc, uint32_t w, int *link) {
    uint32_t op = BITS(w, 20, 5);
    if (!BIT(w, 25)) {
        uint32_t b4 = BIT(w, 4), b7 = BIT(w, 7);
        if ((op & 0x19) == 0x10) {  // 10xx0: misc / halfword multiply
            if (b7 && !b4) return halfword_mul(c, pc, w);
            if (!b7) return misc(c, pc, w, link);
        }
        if (b4 && b7) {
            if (BITS(w, 5, 2) == 0) return BIT(w, 24) ? sync_insn(c, pc, w) : multiply(c, pc, w);
            return extra_ldst(c, pc, w);
        }
        return data_proc(c, pc, w);
    }
    if (op == 0x10) {  // MOVW
        c->r[BITS(w, 12, 4)] = (BITS(w, 16, 4) << 12) | BITS(w, 0, 12);
        return pc + 4;
    }
    if (op == 0x14) {  // MOVT
        uint32_t rd = BITS(w, 12, 4);
        c->r[rd] = (c->r[rd] & 0xFFFFu) | (((BITS(w, 16, 4) << 12) | BITS(w, 0, 12)) << 16);
        return pc + 4;
    }
    if ((op & 0x1B) == 0x12) {  // MSR imm / hints
        if (op == 0x12 && BITS(w, 16, 4) == 0) return pc + 4;
        int carry;
        uint32_t mask = BITS(w, 18, 2);
        apsr_set(c, imm_operand(w, &carry), mask >> 1, mask & 1);
        return pc + 4;
    }
    return data_proc(c, pc, w);
}

uint32_t interp_exec(Cpu *c, uint32_t pc, uint32_t w, int *link) {
    uint32_t cond = w >> 28;
    *link = 0;
    if (cond == 15) return uncond(c, pc, w);
    if (cond != 14 && !cond_pass(c, cond)) return pc + 4;
    switch (BITS(w, 25, 3)) {
    case 0: case 1: return dp_misc(c, pc, w, link);
    case 2: return ldst_word_byte(c, pc, w);
    case 3: return BIT(w, 4) ? media(c, pc, w) : ldst_word_byte(c, pc, w);
    case 4: return ldm_stm(c, pc, w);
    case 5: {
        uint32_t t = pc + 8 + (uint32_t)((int32_t)(w << 8) >> 6);
        if (BIT(w, 24)) { c->r[14] = pc + 4; *link = 1; }
        return t;
    }
    default: return coproc_svc(c, pc, w);
    }
}

// ================================================================ running interpreted functions
#define MAX_RANGES 512
static struct { uint32_t lo, hi; } ranges[MAX_RANGES];
static volatile int nranges;
static pthread_mutex_t range_lock = PTHREAD_MUTEX_INITIALIZER;

void interp_add_range(uint32_t lo, uint32_t hi) {
    pthread_mutex_lock(&range_lock);
    for (int i = 0; i < nranges; i++) {  // (one touching or overlapping it grows: ranges only ever grow)
        if (lo <= ranges[i].hi && hi >= ranges[i].lo) {
            if (lo < ranges[i].lo) __atomic_store_n(&ranges[i].lo, lo, __ATOMIC_RELEASE);
            if (hi > ranges[i].hi) __atomic_store_n(&ranges[i].hi, hi, __ATOMIC_RELEASE);
            pthread_mutex_unlock(&range_lock);
            return;
        }
    }
    if (nranges == MAX_RANGES) FATAL("interp: too many code ranges\n");
    ranges[nranges].lo = lo;
    ranges[nranges].hi = hi;
    __atomic_store_n(&nranges, nranges + 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&range_lock);
}

// Asked after every interpreted instruction: the range of the last hit first (the next instruction is nearly always
// in the same function). With a code mod's patched functions there are a couple of hundred ranges to scan otherwise.
static volatile int owns_last;
int interp_owns(uint32_t pc) {
    int n = __atomic_load_n(&nranges, __ATOMIC_ACQUIRE), last = owns_last;
    if (last < n && pc >= ranges[last].lo && pc < ranges[last].hi) return 1;
    for (int i = 0; i < n; i++)
        if (pc >= ranges[i].lo && pc < ranges[i].hi) {
            owns_last = i;
            return 1;
        }
    return 0;
}

// The instruction at pc. The first two words of a hooked game function may be a Forge hook's jump to its detour
// (written over them in guest memory): there the hook's kept words are the function's own code (fnhook_keep_entry).
static uint32_t code_word(uint32_t pc) {
    uint32_t w;
    if (pc >= 4 && pc < HLE_BASE && (((g_fnhook[pc >> 5] >> ((pc >> 2) & 7)) & 1) ||
                                     ((g_fnhook[(pc - 4) >> 5] >> (((pc - 4) >> 2) & 7)) & 1)) &&
        fnhook_entry_word(pc, &w))
        return w;
    return LD32(pc);
}

// Calls out of the interpreted code run the callee through rt_dispatch (recompiled code returns by returning, not
// through r14): a call continues after the call instruction, a jump out (a tail call) returns to the r14 the jump
// left, which ends the run when that is the caller of the run. The run also ends at its return address when that is
// interpreted code itself, with the stack back where it started (a deeper call returning to the same place has it
// lower): a plugin's detour calling its hook's original through the trampoline (rt_dispatch, so a run of its own),
// where the function, interpreted too, returns into the detour - the detour's own run goes on from there.
// (debug) A guest word whose changes by interpreted code are logged with the instruction (the control file's
// "interpwatch": host_input.c).
volatile uint32_t g_interp_watch;
static volatile int watch_left;
void interp_watch(uint32_t addr, int n) { watch_left = n, g_interp_watch = addr; }
// (debug) A guest range whose reads by interpreted code (LDR / LDRB / LDRH / LDRSH / LDRSB with an immediate or a
// register offset, VLDR) are logged, once per instruction ("interpread": host_input.c).
static volatile uint32_t read_lo, read_hi;
static uint32_t read_seen[256];
static int read_nseen;
void interp_read_watch(uint32_t lo, uint32_t hi) { read_nseen = 0, read_lo = lo, read_hi = hi; }
static void read_check(const Cpu *c, uint32_t pc, uint32_t w) {
    uint32_t imm;
    if ((w & 0x0E100000u) == 0x04100000u && !(w & 0x02000000u)) imm = w & 0xFFFu;  // LDR / LDRB immediate
    else if ((w & 0x0E100010u) == 0x06100000u && !(w & 0xFF0u)) imm = c->r[w & 15];  // LDR / LDRB register
    else if ((w & 0x0E5000B0u) == 0x005000B0u) imm = ((w >> 4) & 0xF0u) | (w & 0xFu);  // LDRH / LDRSH / LDRSB imm
    else if ((w & 0x0E500F90u) == 0x00100090u && (w & 0x60u)) imm = c->r[w & 15];  // LDRH / LDRSH / LDRSB register
    else if ((w & 0x0F300E00u) == 0x0D100A00u) imm = (w & 0xFFu) << 2;  // VLDR
    else return;
    if ((w >> 28) == 0xF) return;
    uint32_t rn = (w >> 16) & 15, base = rn == 15 ? pc + 8 : c->r[rn];
    uint32_t ea = (w & (1u << 24)) ? ((w & (1u << 23)) ? base + imm : base - imm) : base;
    if (ea < read_lo || ea >= read_hi) return;
    for (int i = 0; i < read_nseen; i++)
        if (read_seen[i] == pc) return;
    if (read_nseen < 256) read_seen[read_nseen++] = pc;
    LOG("[readwatch] %08x reads %08x (+%x) = %08x (r0 %08x lr %08x)\n", pc, ea, ea - read_lo, LD32(ea & ~3u), c->r[0], c->r[14]);
}

static void interp_run_inner(Cpu *c);
// (debug) MHGU_INTERP_STATS=1: every 5 s, the instructions the interpreter ran a second (all of them, and per function
// a run started in, its callees included) and how often each was entered: what interpreted code costs.
static int stats_on = -1;
static __thread uint64_t t_insns;
static __thread int t_depth;
static struct { uint32_t entry; uint64_t insns, calls; } stats[512];
static uint64_t stats_total, stats_since, stats_ns;
static pthread_mutex_t stats_lock = PTHREAD_MUTEX_INITIALIZER;
static void stats_add(uint32_t entry, uint64_t n, bool top, uint64_t ns) {
    pthread_mutex_lock(&stats_lock);
    unsigned h = (entry >> 2) % 512, k = 0;
    while (k < 512 && stats[h].entry && stats[h].entry != entry) h = (h + 1) % 512, k++;
    if (k < 512) stats[h].entry = entry, stats[h].insns += n, stats[h].calls++;
    if (top) stats_total += n, stats_ns += ns;
    uint64_t now = plat_now_ns();
    if (!stats_since) stats_since = now;
    if (now - stats_since >= 5000000000ull) {
        double s = (now - stats_since) / 1e9;
        LOG("[interp] %.1f M instructions/s, %.1f ms/s in the interpreter\n", stats_total / s / 1e6, stats_ns / s / 1e6);
        for (int r = 0; r < 12; r++) {
            int best = -1;
            for (int i = 0; i < 512; i++)
                if (stats[i].entry && stats[i].insns && (best < 0 || stats[i].insns > stats[best].insns)) best = i;
            if (best < 0) break;
            LOG("[interp]   %08x %8.0f calls/s %10.0f instructions/s\n", stats[best].entry, stats[best].calls / s,
                stats[best].insns / s);
            stats[best].insns = 0;
        }
        memset(stats, 0, sizeof stats);
        stats_total = stats_ns = 0, stats_since = now;
    }
    pthread_mutex_unlock(&stats_lock);
}

void interp_run(Cpu *c) {
    if (stats_on < 0) stats_on = getenv("MHGU_INTERP_STATS") != NULL;
    if (!stats_on) {
        interp_run_inner(c);
    } else {
        uint32_t entry = c->pc;
        if (code_word(entry) == 0xE51FF004u) entry = LD32(entry + 4);  // (dispatch_interpret's stub: its function)
        uint64_t before = t_insns, t0 = plat_now_ns();
        t_depth++;
        interp_run_inner(c);
        t_depth--;
        stats_add(entry, t_insns - before, t_depth == 0, plat_now_ns() - t0);
    }
    g_ip_pc = 0;  // (back in recompiled code or the host: a fault there is not the plugin's)
}

static void interp_run_inner(Cpu *c) {
    g_ip_cpu = c;
    uint32_t ret = c->r[14], pc = c->pc, sp = c->r[13];
    uint64_t n = 0;  // (MHGU_INTERP_STATS: added to t_insns when the run ends)
    for (;;) {
        int link;
        g_ip_pc = g_ip_last = pc;
        uint32_t wa = g_interp_watch, before = wa ? LD32(wa) : 0;
        if (read_hi) read_check(c, pc, code_word(pc));
        n++;
        uint32_t next = interp_exec(c, pc, code_word(pc), &link);
        if (wa && LD32(wa) != before && watch_left > 0) {
            watch_left--;
            LOG("[watch] %08x: %08x -> %08x by %08x (r0 %08x lr %08x)\n", wa, before, LD32(wa), pc, c->r[0], c->r[14]);
        }
        if (next == ret && !link && c->r[13] >= sp) break;
        if (interp_owns(next)) {
            pc = next;
            continue;
        }
        if (link) {
            c->pc = next;
            g_ip_pc = 0;
            rt_dispatch(c);
            pc += 4;
            continue;
        }
        if (next == ret) break;
        uint32_t lr = c->r[14];
        c->pc = next;
        g_ip_pc = 0;
        rt_dispatch(c);
        if (lr == ret || !interp_owns(lr)) break;
        pc = lr;
    }
    if (stats_on > 0) t_insns += n;
}
