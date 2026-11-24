// The JIT's x86-64 code (runtime/interp_jit.h has the design). Blocks: rbx the Cpu, r15 GUEST_BASE, r14 &g_ip_pc,
// r13 &g_ip_last, r12 &g_code_gen; eax / ecx / edx / r8-r11 scratch; the exit takes eax (next pc) and edx (site).
// ================================================================ x86-64 encoding

enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };
#ifdef _WIN32
enum { ARG0 = RCX, ARG1 = RDX, ARG2 = R8, ARG3 = R9 };
#else
enum { ARG0 = RDI, ARG1 = RSI, ARG2 = RDX, ARG3 = RCX };
#endif
enum { CC_O, CC_NO, CC_B, CC_AE, CC_E, CC_NE, CC_BE, CC_A, CC_S, CC_NS, CC_P, CC_NP, CC_L, CC_GE, CC_LE, CC_G };
enum { X_ADD, X_OR, X_ADC, X_SBB, X_AND, X_SUB, X_XOR, X_CMP };
enum { X_ROL, X_ROR, X_RCL, X_RCR, X_SHL, X_SHR, X_SAR = 7 };

typedef struct { int base, index; int32_t disp; } Mem;
static Mem MB(int base, int32_t disp) { return (Mem){base, -1, disp}; }
static Mem MBI(int base, int index, int32_t disp) { return (Mem){base, index, disp}; }
#define GR(n) MB(RBX, 4 * (int)(n))
#define FL(off) MB(RBX, (off))
#define GMEM(d) MBI(R15, RAX, (d))  // guest memory at eax + d
enum { F_N = 68, F_Z = 72, F_C = 76, F_V = 80 };

static void rex(Em *e, int w, int r, int x, int b, int force) {
    int v = 0x40 | (w ? 8 : 0) | (r & 8 ? 4 : 0) | (x & 8 ? 2 : 0) | (b & 8 ? 1 : 0);
    if (v != 0x40 || force) b1(e, v);
}
static void modrm_m(Em *e, int reg, Mem m) {
    int b = m.base & 7, mod = m.disp == 0 && b != 5 ? 0 : m.disp >= -128 && m.disp <= 127 ? 1 : 2;
    if (m.index < 0) {
        b1(e, mod << 6 | (reg & 7) << 3 | b);
        if (b == 4) b1(e, 0x24);
    } else {
        b1(e, mod << 6 | (reg & 7) << 3 | 4);
        b1(e, (m.index & 7) << 3 | b);
    }
    if (mod == 1) b1(e, (uint32_t)m.disp);
    else if (mod == 2) b4(e, (uint32_t)m.disp);
}
// [prefix] [REX] opcode bytes (low byte first) modrm: a memory operand / a register operand
static void om(Em *e, int pfx, int w, int force, uint32_t opc, int nopc, int reg, Mem m) {
    if (pfx) b1(e, pfx);
    rex(e, w, reg, m.index < 0 ? 0 : m.index, m.base, force);
    for (int i = 0; i < nopc; i++) b1(e, opc >> (8 * i));
    modrm_m(e, reg, m);
}
static void orr(Em *e, int pfx, int w, int force, uint32_t opc, int nopc, int reg, int rm) {
    if (pfx) b1(e, pfx);
    rex(e, w, reg, 0, rm, force);
    for (int i = 0; i < nopc; i++) b1(e, opc >> (8 * i));
    b1(e, 0xC0 | (reg & 7) << 3 | (rm & 7));
}
static int fits8(uint32_t i) { return (int32_t)i >= -128 && (int32_t)i <= 127; }

static void mov_rm(Em *e, int r, Mem m) { om(e, 0, 0, 0, 0x8B, 1, r, m); }
static void mov_mr(Em *e, Mem m, int r) { om(e, 0, 0, 0, 0x89, 1, r, m); }
static void mov_mi(Em *e, Mem m, uint32_t i) { om(e, 0, 0, 0, 0xC7, 1, 0, m), b4(e, i); }
static void mov_ri(Em *e, int r, uint32_t i) {
    if (!i) {
        orr(e, 0, 0, 0, 0x31, 1, r, r);  // xor r, r (flags: only where none are live)
        return;
    }
    rex(e, 0, 0, 0, r, 0), b1(e, 0xB8 + (r & 7)), b4(e, i);
}
static void mov_ri_keep(Em *e, int r, uint32_t i) { rex(e, 0, 0, 0, r, 0), b1(e, 0xB8 + (r & 7)), b4(e, i); }
static void mov_ri64(Em *e, int r, uint64_t i) { rex(e, 1, 0, 0, r, 0), b1(e, 0xB8 + (r & 7)), b8(e, i); }
static void mov_rr(Em *e, int d, int s) { orr(e, 0, 0, 0, 0x89, 1, s, d); }
static void mov_rr64(Em *e, int d, int s) { orr(e, 0, 1, 0, 0x89, 1, s, d); }
static void mov_rm64(Em *e, int r, Mem m) { om(e, 0, 1, 0, 0x8B, 1, r, m); }
static void alu_rr(Em *e, int op, int d, int s) { orr(e, 0, 0, 0, 0x01 + 8 * op, 1, s, d); }
static void alu_rr64(Em *e, int op, int d, int s) { orr(e, 0, 1, 0, 0x01 + 8 * op, 1, s, d); }
static void alu_rm(Em *e, int op, int d, Mem m) { om(e, 0, 0, 0, 0x03 + 8 * op, 1, d, m); }
static void alu_ri(Em *e, int op, int r, uint32_t i) {
    if (fits8(i)) orr(e, 0, 0, 0, 0x83, 1, op, r), b1(e, i);
    else orr(e, 0, 0, 0, 0x81, 1, op, r), b4(e, i);
}
static void alu_mi(Em *e, int op, Mem m, uint32_t i) {
    if (fits8(i)) om(e, 0, 0, 0, 0x83, 1, op, m), b1(e, i);
    else om(e, 0, 0, 0, 0x81, 1, op, m), b4(e, i);
}
static void alu_mi64(Em *e, int op, Mem m, uint32_t i) {
    if (fits8(i)) om(e, 0, 1, 0, 0x83, 1, op, m), b1(e, i);
    else om(e, 0, 1, 0, 0x81, 1, op, m), b4(e, i);
}
static void sh_ri(Em *e, int k, int r, int n) {
    if (n == 1) orr(e, 0, 0, 0, 0xD1, 1, k, r);
    else orr(e, 0, 0, 0, 0xC1, 1, k, r), b1(e, n);
}
static void sh_ri64(Em *e, int k, int r, int n) { orr(e, 0, 1, 0, 0xC1, 1, k, r), b1(e, n); }
static void sh_rcl(Em *e, int k, int r) { orr(e, 0, 0, 0, 0xD3, 1, k, r); }
static void test_rr(Em *e, int a, int b) { orr(e, 0, 0, 0, 0x85, 1, b, a); }
static void test_rr64(Em *e, int a, int b) { orr(e, 0, 1, 0, 0x85, 1, b, a); }
static void setcc_m(Em *e, int cc, Mem m) { om(e, 0, 0, 0, 0x900F + (cc << 8), 2, 0, m); }
static void setcc_r(Em *e, int cc, int r) { orr(e, 0, 0, 1, 0x900F + (cc << 8), 2, 0, r); }
static void movb_mr(Em *e, Mem m, int r) { om(e, 0, 0, 1, 0x88, 1, r, m); }
static void movw_mr(Em *e, Mem m, int r) { om(e, 0x66, 0, 0, 0x89, 1, r, m); }
static void movw_mi(Em *e, Mem m, uint32_t i) { om(e, 0x66, 0, 0, 0xC7, 1, 0, m), b1(e, i), b1(e, i >> 8); }
static void movzx8_rm(Em *e, int r, Mem m) { om(e, 0, 0, 0, 0xB60F, 2, r, m); }
static void movzx16_rm(Em *e, int r, Mem m) { om(e, 0, 0, 0, 0xB70F, 2, r, m); }
static void movsx8_rm(Em *e, int r, Mem m) { om(e, 0, 0, 0, 0xBE0F, 2, r, m); }
static void movsx16_rm(Em *e, int r, Mem m) { om(e, 0, 0, 0, 0xBF0F, 2, r, m); }
static void movzx8_rr(Em *e, int d, int s) { orr(e, 0, 0, 1, 0xB60F, 2, d, s); }
static void movzx16_rr(Em *e, int d, int s) { orr(e, 0, 0, 0, 0xB70F, 2, d, s); }
static void movsx8_rr(Em *e, int d, int s) { orr(e, 0, 0, 1, 0xBE0F, 2, d, s); }
static void movsx16_rr(Em *e, int d, int s) { orr(e, 0, 0, 0, 0xBF0F, 2, d, s); }
static void movsxd_rm(Em *e, int r, Mem m) { om(e, 0, 1, 0, 0x63, 1, r, m); }
static void imul_rm(Em *e, int r, Mem m) { om(e, 0, 0, 0, 0xAF0F, 2, r, m); }
static void imul_rr64(Em *e, int d, int s) { orr(e, 0, 1, 0, 0xAF0F, 2, d, s); }
static void bsr_rm(Em *e, int r, Mem m) { om(e, 0, 0, 0, 0xBD0F, 2, r, m); }
static void bswap_r(Em *e, int r) { rex(e, 0, 0, 0, r, 0), b1(e, 0x0F), b1(e, 0xC8 + (r & 7)); }
static void not_r(Em *e, int r) { orr(e, 0, 0, 0, 0xF7, 1, 2, r); }
static void cmpb_mi(Em *e, Mem m, uint32_t i) { om(e, 0, 0, 0, 0x80, 1, X_CMP, m), b1(e, i); }
static void alub_rm(Em *e, int op, int r, Mem m) { om(e, 0, 0, 1, 0x02 + 8 * op, 1, r, m); }  // op r8, m8
static void cmov_rr(Em *e, int cc, int d, int s) { orr(e, 0, 0, 0, 0x400F + (cc << 8), 2, d, s); }
static void lea_rm(Em *e, int r, Mem m) { om(e, 0, 0, 0, 0x8D, 1, r, m); }  // (32-bit: wraps as the guest does)
static void lea_rm64(Em *e, int r, Mem m) { om(e, 0, 1, 0, 0x8D, 1, r, m); }
static void call_r(Em *e, int r) { orr(e, 0, 0, 0, 0xFF, 1, 2, r); }
static void push_r(Em *e, int r) { rex(e, 0, 0, 0, r, 0), b1(e, 0x50 + (r & 7)); }
static void pop_r(Em *e, int r) { rex(e, 0, 0, 0, r, 0), b1(e, 0x58 + (r & 7)); }
static uint8_t *jcc32(Em *e, int cc) {
    b1(e, 0x0F), b1(e, 0x80 + cc);
    uint8_t *s = e->p;
    b4(e, 0);
    return s;
}
static uint8_t *jmp32(Em *e) {
    b1(e, 0xE9);
    uint8_t *s = e->p;
    b4(e, 0);
    return s;
}
static uint8_t *jcc8(Em *e, int cc) {
    b1(e, 0x70 + cc);
    uint8_t *s = e->p;
    b1(e, 0);
    return s;
}
static void patch32(uint8_t *site, const uint8_t *to) {
    int32_t r = (int32_t)(to - (site + 4));
    memcpy(site, &r, 4);
}
static void here32(Em *e, uint8_t *site) {
    if (site && !e->bad) patch32(site, e->p);
}
static void here8(Em *e, uint8_t *site) {
    if (site && !e->bad) *site = (uint8_t)(e->p - (site + 1));
}

// ================================================================ translation

static void ld_greg(J *j, int r, uint32_t n, uint32_t pc) {
    if (n == 15) mov_ri_keep(&j->e, r, pc + 8);
    else mov_rm(&j->e, r, GR(n));
}

// to the common exit: eax the next pc, edx the site
static void exit_jmp(J *j) { patch32(jmp32(&j->e), j->t->exit); }

enum { LK_NONE, LK_YES, LK_TMP };
// leave with eax (an indirect branch, or an interpreter op's result)
static void exit_ind(J *j, int link, uint32_t retpc) {
    Em *e = &j->e;
    if (link == LK_YES) mov_mi(e, FL(JO_LINK), 1);
    if (link == LK_TMP) mov_rm(e, RDX, FL(JO_TLINK)), mov_mr(e, FL(JO_LINK), RDX);
    if (link != LK_NONE) mov_mi(e, FL(JO_RETPC), retpc);
    mov_ri(e, RDX, 0);
    exit_jmp(j);
}

// leave for target (a direct branch): chainable. pc: the branch's own address
static void exit_direct(J *j, uint32_t target, int link, uint32_t pc) {
    Em *e = &j->e;
    uint8_t *to_stub[3];
    int n = 0;
    if (!link) {  // the run's return address: back to the loop
        alu_mi(e, X_CMP, FL(JO_RET), target);
        to_stub[n++] = jcc32(e, CC_E);
    }
    if (target <= j->pc0) {  // backward: a loop gives the loop a look when the code generation moved
        alu_mi(e, X_CMP, MB(R12, 0), j->t->gen);  // (the arena starts over for a new one: its blocks' gen is fixed)
        to_stub[n++] = jcc32(e, CC_NE);
    }
    uint8_t *site = jmp32(e);
    for (int i = 0; i < n; i++) here32(e, to_stub[i]);
    here32(e, site);
    mov_ri_keep(e, RAX, target);
    if (link) mov_mi(e, FL(JO_LINK), 1), mov_mi(e, FL(JO_RETPC), pc + 4);
    mov_ri_keep(e, RDX, (uint32_t)(site - j->t->base));
    exit_jmp(j);
}

// a jump taken when cond fails (NULL: always passes). The flags are read a byte at a time: the blocks write them with
// SETcc (a byte), and a wider load of a byte just stored stalls (no store forwarding: ~15 cycles each)
static uint8_t *cond_false(J *j, uint32_t cond) {
    Em *e = &j->e;
    static const int flag[8] = {F_Z, F_Z, F_C, F_C, F_N, F_N, F_V, F_V};
    if (cond < 8) {
        cmpb_mi(e, FL(flag[cond]), 0);
        return jcc32(e, cond & 1 ? CC_NE : CC_E);
    }
    switch (cond) {
    case 8:  // HI: C && !Z
    case 9:  // LS
        movzx8_rm(e, RAX, FL(F_Z));
        alu_ri(e, X_XOR, RAX, 1);
        alub_rm(e, X_AND, RAX, FL(F_C));
        return jcc32(e, cond == 8 ? CC_E : CC_NE);
    case 10:  // GE: N == V
    case 11:
        movzx8_rm(e, RAX, FL(F_N));
        alub_rm(e, X_CMP, RAX, FL(F_V));
        return jcc32(e, cond == 10 ? CC_NE : CC_E);
    case 12:  // GT: !Z && N == V
    case 13:
        movzx8_rm(e, RAX, FL(F_N));
        alub_rm(e, X_XOR, RAX, FL(F_V));
        alub_rm(e, X_OR, RAX, FL(F_Z));
        return jcc32(e, cond == 12 ? CC_NE : CC_E);
    default:
        return NULL;
    }
}

// x86 CF = the guest's C (ADC, RRX) or its inverse (SBC, RSC), from the byte
static void carry_in(J *j, int inverse) {
    cmpb_mi(&j->e, FL(F_C), 1);  // CF: C < 1, i.e. !C
    if (!inverse) b1(&j->e, 0xF5);  // cmc
}

// The bad-access check on the guest address in eax (interp.c gchk; g_ip_pc set): the thunk saves what it clobbers
static void chk(J *j, uint32_t pc, int size, int wr) {
    Em *e = &j->e;
    if (j->test) return;
    int lg = size >= 8 ? 3 : size >= 4 ? 2 : size >= 2 ? 1 : 0;
    alu_ri(e, X_CMP, RAX, 0x10000);
    uint8_t *lo = jcc8(e, CC_B);
    lea_rm(e, R10, MBI(RAX, RAX, (int32_t)0x88000000u));  // (a & 0x7FFFFFFF) - 0x3C000000, doubled
    alu_ri(e, X_CMP, R10, 0x10000000u);
    uint8_t *ok = jcc8(e, CC_AE);
    here8(e, lo);
    b1(e, 0xE8);
    uint8_t *s = e->p;
    b4(e, 0);
    if (!e->bad) patch32(s, j->t->thunk[lg][wr]);
    here8(e, ok);
    (void)pc;
}
static void set_ip(J *j, uint32_t pc) {
    if (!j->test) mov_mi(&j->e, MB(R14, 0), pc);
}

// The shift of an immediate-shifted register operand (rm, typ, amount) into ECX. want: the carry out, which is left in
// R8B (*carry 2), or is the old C (*carry -1)
static void j_shift_imm(J *j, uint32_t w, uint32_t pc, int want, int *carry) {
    Em *e = &j->e;
    uint32_t rm = BITS(w, 0, 4), typ = BITS(w, 5, 2), amt = BITS(w, 7, 5);
    ld_greg(j, RCX, rm, pc);
    *carry = -1;
    switch (typ) {
    case 0:
        if (!amt) return;
        sh_ri(e, X_SHL, RCX, (int)amt);
        break;
    case 1:
        if (!amt) {  // LSR #32
            if (want) mov_rr(e, R8, RCX), sh_ri(e, X_SHR, R8, 31), *carry = 2;
            mov_ri(e, RCX, 0);
            return;
        }
        sh_ri(e, X_SHR, RCX, (int)amt);
        break;
    case 2:
        if (!amt) {  // ASR #32
            if (want) mov_rr(e, R8, RCX), sh_ri(e, X_SHR, R8, 31), *carry = 2;
            sh_ri(e, X_SAR, RCX, 31);
            return;
        }
        sh_ri(e, X_SAR, RCX, (int)amt);
        break;
    default:
        if (!amt) {  // RRX
            carry_in(j, 0);
            orr(e, 0, 0, 0, 0xD1, 1, X_RCR, RCX);
        } else {
            sh_ri(e, X_ROR, RCX, (int)amt);  // (CF: the result's top bit, the carry out)
        }
        break;
    }
    if (want) setcc_r(e, CC_B, R8), *carry = 2;
}

// Data processing (register / immediate operand 2). Returns 0 when the interpreter has to run it. *pcw: eax holds a
// value written to pc.
static int t_dp(J *j, uint32_t w, uint32_t pc, int *pcw) {
    Em *e = &j->e;
    uint32_t I = BIT(w, 25), opc = BITS(w, 21, 4), S = BIT(w, 20), rn = BITS(w, 16, 4), rd = BITS(w, 12, 4);
    int logical = opc <= 1 || opc == 8 || opc == 9 || opc >= 12, test = opc >= 8 && opc <= 11, carry = -1;
    if (S && rd == 15 && !test) return 0;
    if (!S && test) return 0;
    if (!I && BIT(w, 4)) {  // register-specified shift
        uint32_t rs = BITS(w, 8, 4), rm = BITS(w, 0, 4), typ = BITS(w, 5, 2);
        if ((S && logical) || rs == 15 || rm == 15) return 0;
        mov_rm(e, RCX, GR(rs));
        alu_ri(e, X_AND, RCX, 0xFF);
        mov_rm(e, RDX, GR(rm));
        switch (typ) {
        case 0:
        case 1:
            sh_rcl(e, typ ? X_SHR : X_SHL, RDX);
            mov_ri(e, R8, 0);
            alu_ri(e, X_CMP, RCX, 32);
            cmov_rr(e, CC_AE, RDX, R8);
            break;
        case 2:
            mov_ri_keep(e, R8, 31);
            alu_ri(e, X_CMP, RCX, 31);
            cmov_rr(e, CC_A, RCX, R8);
            sh_rcl(e, X_SAR, RDX);
            break;
        default:
            sh_rcl(e, X_ROR, RDX);
            break;
        }
        mov_rr(e, RCX, RDX);
    } else if (I) {
        int c;
        mov_ri_keep(e, RCX, imm_operand(w, &c));
        carry = c;
    } else {
        j_shift_imm(j, w, pc, S && logical, &carry);
    }
    if (opc != 13 && opc != 15) ld_greg(j, RAX, rn, pc);
    switch (opc) {
    case 0: case 8: alu_rr(e, X_AND, RAX, RCX); break;
    case 1: case 9: alu_rr(e, X_XOR, RAX, RCX); break;
    case 2: case 10: alu_rr(e, X_SUB, RAX, RCX); break;
    case 3: alu_rr(e, X_SUB, RCX, RAX), mov_rr(e, RAX, RCX); break;
    case 4: case 11: alu_rr(e, X_ADD, RAX, RCX); break;
    case 5: carry_in(j, 0), alu_rr(e, X_ADC, RAX, RCX); break;
    case 6: carry_in(j, 1), alu_rr(e, X_SBB, RAX, RCX); break;
    case 7: carry_in(j, 1), alu_rr(e, X_SBB, RCX, RAX), mov_rr(e, RAX, RCX); break;
    case 12: alu_rr(e, X_OR, RAX, RCX); break;
    case 13: mov_rr(e, RAX, RCX); break;
    case 14: not_r(e, RCX), alu_rr(e, X_AND, RAX, RCX); break;
    default: not_r(e, RCX), mov_rr(e, RAX, RCX); break;
    }
    if (S) {
        if (!logical) {
            int sub = opc == 2 || opc == 3 || opc == 6 || opc == 7 || opc == 10;
            setcc_m(e, CC_S, FL(F_N));
            setcc_m(e, CC_E, FL(F_Z));
            setcc_m(e, sub ? CC_AE : CC_B, FL(F_C));
            setcc_m(e, CC_O, FL(F_V));
        } else {
            if (opc == 13 || opc == 15) test_rr(e, RAX, RAX);
            setcc_m(e, CC_S, FL(F_N));
            setcc_m(e, CC_E, FL(F_Z));
            if (carry == 0 || carry == 1) mov_mi(e, FL(F_C), (uint32_t)carry);
            else if (carry == 2) movb_mr(e, FL(F_C), R8);
        }
    }
    if (test) return 1;
    if (rd == 15) *pcw = 1;
    else mov_mr(e, GR(rd), RAX);
    return 1;
}

// ea in eax, the writeback value in edx: base rn plus / minus the offset in ecx (reg) or off (imm)
static void ea_wb(J *j, uint32_t w, uint32_t pc, uint32_t rn, int reg, uint32_t off, int wb) {
    Em *e = &j->e;
    uint32_t P = BIT(w, 24), U = BIT(w, 23);
    ld_greg(j, RAX, rn, pc);
    if (reg) {
        if (P) {
            alu_rr(e, U ? X_ADD : X_SUB, RAX, RCX);
            if (wb) mov_rr(e, RDX, RAX);
        } else if (wb) {
            mov_rr(e, RDX, RAX);
            alu_rr(e, U ? X_ADD : X_SUB, RDX, RCX);
        }
    } else {
        int32_t d = U ? (int32_t)off : -(int32_t)off;
        if (P) {
            if (d) lea_rm(e, RAX, MB(RAX, d));
            if (wb) mov_rr(e, RDX, RAX);
        } else if (wb) {
            lea_rm(e, RDX, MB(RAX, d));
        }
    }
}

// LDR / STR / LDRB / STRB
static int t_ldst(J *j, uint32_t w, uint32_t pc, int *pcw) {
    Em *e = &j->e;
    uint32_t I = BIT(w, 25), P = BIT(w, 24), B = BIT(w, 22), Wb = BIT(w, 21), L = BIT(w, 20);
    uint32_t rn = BITS(w, 16, 4), rt = BITS(w, 12, 4);
    int wb = (!P || Wb) && rn != 15, carry;
    if (I) j_shift_imm(j, w & ~0x10u, pc, 0, &carry);
    ea_wb(j, w, pc, rn, (int)I, BITS(w, 0, 12), wb);
    if (rn != 13) set_ip(j, pc), chk(j, pc, B ? 1 : 4, !L);
    if (L) {
        if (B) movzx8_rm(e, RCX, GMEM(0));
        else mov_rm(e, RCX, GMEM(0));
        if (wb) mov_mr(e, GR(rn), RDX);
        if (rt == 15) mov_rr(e, RAX, RCX), *pcw = 1;
        else mov_mr(e, GR(rt), RCX);
    } else {
        ld_greg(j, RCX, rt, pc);
        if (B) movb_mr(e, GMEM(0), RCX);
        else mov_mr(e, GMEM(0), RCX);
        if (wb) mov_mr(e, GR(rn), RDX);
    }
    return 1;
}

// LDRH / STRH / LDRSB / LDRSH / LDRD / STRD
static int t_xldst(J *j, uint32_t w, uint32_t pc) {
    Em *e = &j->e;
    uint32_t I = BIT(w, 22), P = BIT(w, 24), Wb = BIT(w, 21), L = BIT(w, 20), op2 = BITS(w, 5, 2);
    uint32_t rn = BITS(w, 16, 4), rt = BITS(w, 12, 4), rm = BITS(w, 0, 4);
    int wb = (!P || Wb) && rn != 15, dual = op2 >= 2 && !L;
    if (rt == 15 || (dual && (rt & 1 || rt == 14))) return 0;
    if (!I) ld_greg(j, RCX, rm, pc);
    ea_wb(j, w, pc, rn, !I, (BITS(w, 8, 4) << 4) | BITS(w, 0, 4), wb);
    int size = dual ? 8 : op2 == 2 ? 1 : 2;
    if (rn != 13) set_ip(j, pc), chk(j, pc, size, !L && op2 != 2);
    if (op2 == 2 && !L) {  // LDRD (the second word's address wraps at 4 GB as the guest's)
        mov_rm(e, RCX, GMEM(0));
        lea_rm(e, RAX, MB(RAX, 4));
        mov_rm(e, R9, GMEM(0));
        if (wb) mov_mr(e, GR(rn), RDX);
        mov_mr(e, GR(rt), RCX);
        mov_mr(e, GR(rt + 1), R9);
        return 1;
    }
    if (op2 == 3 && !L) {  // STRD
        ld_greg(j, RCX, rt, pc);
        ld_greg(j, R9, rt + 1, pc);
        mov_mr(e, GMEM(0), RCX);
        lea_rm(e, RAX, MB(RAX, 4));
        mov_mr(e, GMEM(0), R9);
        if (wb) mov_mr(e, GR(rn), RDX);
        return 1;
    }
    if (op2 == 1 && !L) {  // STRH
        ld_greg(j, RCX, rt, pc);
        movw_mr(e, GMEM(0), RCX);
        if (wb) mov_mr(e, GR(rn), RDX);
        return 1;
    }
    if (op2 == 1) movzx16_rm(e, RCX, GMEM(0));
    else if (op2 == 2) movsx8_rm(e, RCX, GMEM(0));
    else movsx16_rm(e, RCX, GMEM(0));
    if (wb) mov_mr(e, GR(rn), RDX);
    mov_mr(e, GR(rt), RCX);
    return 1;
}

// LDM / STM (not the user-register forms)
static int t_ldm(J *j, uint32_t w, uint32_t pc, int *pcw) {
    Em *e = &j->e;
    uint32_t P = BIT(w, 24), U = BIT(w, 23), Wb = BIT(w, 21), L = BIT(w, 20), rn = BITS(w, 16, 4);
    uint32_t list = w & 0xFFFF, n = (uint32_t)__builtin_popcount(list);
    if (BIT(w, 22) || !n || rn == 15) return 0;
    int32_t eo = U ? (P ? 4 : 0) : (P ? -(int32_t)(4 * n) : -(int32_t)(4 * n) + 4);
    int32_t wo = U ? (int32_t)(4 * n) : -(int32_t)(4 * n);
    mov_rm(e, RAX, GR(rn));
    if (Wb) lea_rm(e, RDX, MB(RAX, wo));
    if (eo) lea_rm(e, RAX, MB(RAX, eo));
    if (rn != 13) set_ip(j, pc), chk(j, pc, 4, !L);
    uint32_t i = 0;  // (each word's address in eax, wrapping at 4 GB as the guest's)
    if (L) {
        if (Wb) mov_mr(e, GR(rn), RDX);
        for (uint32_t r = 0; r < 16; r++) {
            if (!BIT(list, r)) continue;
            if (i++) lea_rm(e, RAX, MB(RAX, 4));
            if (r == 15) {
                mov_rm(e, R11, GMEM(0));
            } else {
                mov_rm(e, RCX, GMEM(0));
                mov_mr(e, GR(r), RCX);
            }
        }
        if (BIT(list, 15)) mov_rr(e, RAX, R11), *pcw = 1;
    } else {
        for (uint32_t r = 0; r < 16; r++) {
            if (!BIT(list, r)) continue;
            if (i++) lea_rm(e, RAX, MB(RAX, 4));
            ld_greg(j, RCX, r, pc);
            mov_mr(e, GMEM(0), RCX);
        }
        if (Wb) mov_mr(e, GR(rn), RDX);
    }
    return 1;
}

// MUL / MLA / MLS / UMULL / UMLAL / SMULL / SMLAL
static int t_mul(J *j, uint32_t w) {
    Em *e = &j->e;
    uint32_t op = BITS(w, 21, 3), S = BIT(w, 20), rd = BITS(w, 16, 4), ra = BITS(w, 12, 4), rm = BITS(w, 8, 4);
    uint32_t rn = BITS(w, 0, 4);
    if (op == 2 || rd == 15 || rm == 15 || rn == 15 || ((op & 1 || op >= 3) && ra == 15)) return 0;
    if (op <= 1 || op == 3) {
        if (op == 3 && S) return 0;
        mov_rm(e, RAX, GR(rn));
        imul_rm(e, RAX, GR(rm));
        if (op == 1) alu_rm(e, X_ADD, RAX, GR(ra));
        if (op == 3) mov_rm(e, RCX, GR(ra)), alu_rr(e, X_SUB, RCX, RAX), mov_rr(e, RAX, RCX);
        if (S) test_rr(e, RAX, RAX), setcc_m(e, CC_S, FL(F_N)), setcc_m(e, CC_E, FL(F_Z));
        mov_mr(e, GR(rd), RAX);
        return 1;
    }
    if (op >= 6) movsxd_rm(e, RAX, GR(rn)), movsxd_rm(e, RCX, GR(rm));
    else mov_rm(e, RAX, GR(rn)), mov_rm(e, RCX, GR(rm));
    imul_rr64(e, RAX, RCX);
    if (op & 1) {
        mov_rm(e, RDX, GR(ra));
        mov_rm(e, R8, GR(rd));
        sh_ri64(e, X_SHL, R8, 32);
        alu_rr64(e, X_OR, RDX, R8);
        alu_rr64(e, X_ADD, RAX, RDX);
    }
    if (S) test_rr64(e, RAX, RAX), setcc_m(e, CC_S, FL(F_N)), setcc_m(e, CC_E, FL(F_Z));
    mov_mr(e, GR(ra), RAX);
    sh_ri64(e, X_SHR, RAX, 32);
    mov_mr(e, GR(rd), RAX);
    return 1;
}

// media: SXTB / SXTH / UXTB / UXTH (+ add), REV, UBFX / SBFX, BFI / BFC
static int t_media(J *j, uint32_t w, uint32_t pc) {
    Em *e = &j->e;
    uint32_t op1 = BITS(w, 20, 5), op2 = BITS(w, 5, 3), rd = BITS(w, 12, 4), rm = BITS(w, 0, 4), rn = BITS(w, 16, 4);
    (void)pc;
    if (rd == 15) return 0;
    if ((op1 & 0x18) == 0x08) {
        uint32_t p1 = BITS(w, 20, 3);
        if (op2 == 3 && (p1 == 2 || p1 == 3 || p1 == 6 || p1 == 7)) {
            if (rm == 15) return 0;
            uint32_t rot = BITS(w, 10, 2) * 8;
            mov_rm(e, RAX, GR(rm));
            if (rot) sh_ri(e, X_ROR, RAX, (int)rot);
            if (p1 == 2) movsx8_rr(e, RAX, RAX);
            else if (p1 == 3) movsx16_rr(e, RAX, RAX);
            else if (p1 == 6) movzx8_rr(e, RAX, RAX);
            else movzx16_rr(e, RAX, RAX);
            if (rn != 15) alu_rm(e, X_ADD, RAX, GR(rn));
            mov_mr(e, GR(rd), RAX);
            return 1;
        }
        if (p1 == 3 && op2 == 1) {  // REV
            if (rm == 15) return 0;
            mov_rm(e, RAX, GR(rm));
            bswap_r(e, RAX);
            mov_mr(e, GR(rd), RAX);
            return 1;
        }
        return 0;
    }
    uint32_t lsb = BITS(w, 7, 5), msb = BITS(w, 16, 5);
    if (((op1 & 0x1E) == 0x1A || (op1 & 0x1E) == 0x1E) && (op2 & 3) == 2) {  // SBFX / UBFX
        uint32_t width = msb + 1;
        if (rm == 15 || lsb + width > 32) return 0;
        mov_rm(e, RAX, GR(rm));
        if ((op1 & 0x1E) == 0x1E) {
            if (lsb) sh_ri(e, X_SHR, RAX, (int)lsb);
            if (width < 32) alu_ri(e, X_AND, RAX, (uint32_t)((1ull << width) - 1));
        } else {
            if (32 - lsb - width) sh_ri(e, X_SHL, RAX, (int)(32 - lsb - width));
            if (32 - width) sh_ri(e, X_SAR, RAX, (int)(32 - width));
        }
        mov_mr(e, GR(rd), RAX);
        return 1;
    }
    if ((op1 & 0x1E) == 0x1C && (op2 & 3) == 0) {  // BFC / BFI
        if (msb < lsb) return 0;
        uint32_t mask = (uint32_t)(((1ull << (msb - lsb + 1)) - 1) << lsb);
        mov_rm(e, RAX, GR(rd));
        alu_ri(e, X_AND, RAX, ~mask);
        if (rm != 15) {
            mov_rm(e, RCX, GR(rm));
            if (lsb) sh_ri(e, X_SHL, RCX, (int)lsb);
            alu_ri(e, X_AND, RCX, mask);
            alu_rr(e, X_OR, RAX, RCX);
        }
        mov_mr(e, GR(rd), RAX);
        return 1;
    }
    return 0;
}

// BX / BLX to eax: a Thumb target stops as the interpreter does
static void thumb_check(J *j, uint32_t pc) {
    Em *e = &j->e;
    b1(e, 0xA8), b1(e, 1);  // test al, 1
    uint8_t *ok = jcc32(e, CC_E);
    mov_rr64(e, ARG0, RBX);
    mov_ri_keep(e, ARG1, pc);
    mov_ri64(e, RAX, (uint64_t)(uintptr_t)rt_thumb);
    call_r(e, RAX);
    here32(e, ok);
}

// one instruction through the interpreter's op (a copy kept with the blocks); eax: the next pc
static void call_op(J *j, const Op *o, uint32_t pc) {
    Em *e = &j->e;
    Op *c = NULL;
    if (j->nops < j->maxops) c = j->ops - ++j->nops, *c = *o;  // (down from t->dcur)
    else e->bad = 1;
    if (!j->test) mov_mi(e, MB(R14, 0), pc), mov_mi(e, MB(R13, 0), pc);
    mov_mi(e, FL(JO_TLINK), 0);
    mov_rr64(e, ARG0, RBX);
    mov_ri64(e, ARG1, (uint64_t)(uintptr_t)c);
    mov_ri_keep(e, ARG2, pc);
    lea_rm64(e, ARG3, FL(JO_TLINK));
    mov_ri64(e, RAX, (uint64_t)(uintptr_t)o->fn);
    call_r(e, RAX);
}

// One instruction. last: the block's last (it may branch).
static void emit_insn(J *j, const Op *o, uint32_t pc, int last) {
    Em *e = &j->e;
    uint32_t w = o->w, cond = w >> 28, cls = BITS(w, 25, 3);
    int pcw = 0, done = 0;
    uint8_t *skip = cond < 14 ? cond_false(j, cond) : NULL;  // (interp_exec, op_exec's, tests it again: harmless)
    if (cond != 15) {
        switch (cls) {
        case 0:
        case 1: {
            uint32_t op = BITS(w, 20, 5);
            if (!BIT(w, 25)) {
                uint32_t b4 = BIT(w, 4), b7 = BIT(w, 7);
                if ((op & 0x19) == 0x10 && b7 && !b4) {
                    break;  // halfword multiplies
                } else if ((op & 0x19) == 0x10 && !b7) {  // misc: BX / BLX / CLZ here
                    uint32_t op2 = BITS(w, 4, 3), mop = BITS(w, 21, 2), rm = BITS(w, 0, 4), rd = BITS(w, 12, 4);
                    if (mop == 1 && (op2 == 1 || op2 == 3) && rm != 15) {
                        mov_rm(e, RAX, GR(rm));
                        thumb_check(j, pc);
                        if (op2 == 3) mov_mi(e, GR(14), pc + 4);
                        exit_ind(j, op2 == 3 ? LK_YES : LK_NONE, pc + 4);
                        done = 2;
                    } else if (mop == 3 && op2 == 1 && rm != 15 && rd != 15) {
                        bsr_rm(e, RCX, GR(rm));  // (not LZCNT: the JIT keeps to baseline x86-64, Android's x86_64 too)
                        mov_ri_keep(e, RDX, 0xFFFFFFFFu);
                        cmov_rr(e, CC_E, RCX, RDX);  // (0: bit -1, so 32)
                        mov_ri_keep(e, RAX, 31);
                        alu_rr(e, X_SUB, RAX, RCX);
                        mov_mr(e, GR(rd), RAX);
                        done = 1;
                    }
                } else if (b4 && b7) {
                    if (BITS(w, 5, 2) == 0) done = BIT(w, 24) ? 0 : t_mul(j, w);
                    else done = t_xldst(j, w, pc);
                } else {
                    done = t_dp(j, w, pc, &pcw);
                }
            } else if (op == 0x10 || op == 0x14) {  // MOVW / MOVT
                uint32_t rd = BITS(w, 12, 4), v = (BITS(w, 16, 4) << 12) | BITS(w, 0, 12);
                if (rd != 15) {
                    if (op == 0x10) mov_mi(e, GR(rd), v);
                    else movw_mi(e, MB(RBX, 4 * (int)rd + 2), v);
                    done = 1;
                }
            } else if ((op & 0x1B) != 0x12) {
                done = t_dp(j, w, pc, &pcw);
            }
            break;
        }
        case 2:
        case 3:
            if (cls == 3 && BIT(w, 4)) done = t_media(j, w, pc);
            else done = t_ldst(j, w, pc, &pcw);
            break;
        case 4:
            done = t_ldm(j, w, pc, &pcw);
            break;
        case 5: {
            uint32_t t = pc + 8 + (uint32_t)((int32_t)(w << 8) >> 6);
            if (BIT(w, 24)) mov_mi(e, GR(14), pc + 4);
            exit_direct(j, t, (int)BIT(w, 24), pc);
            done = 2;
            break;
        }
        default:
            break;
        }
    }
    if (pcw) exit_ind(j, LK_NONE, pc + 4);
    if (!done) {
        call_op(j, o, pc);
        if (last) {
            exit_ind(j, LK_TMP, pc + 4);
        } else {
            alu_ri(e, X_CMP, RAX, pc + 4);
            uint8_t *on = jcc32(e, CC_E);
            exit_ind(j, LK_TMP, pc + 4);
            here32(e, on);
        }
    }
    here32(e, skip);
}

// ================================================================ arena
static int jit_header(JitTls *t) {
    uint8_t *b = t->base;
    // RUNTIME_FUNCTION at +0 covering enter .. the arena's end, its UNWIND_INFO at +16 (the enter stub's prologue:
    // 8 pushes, sub rsp 40; every address after it has that frame)
    static const uint8_t uw[] = {1, 16, 9, 0,
                                 16, 0x42, 12, 0xF0, 10, 0xE0, 8, 0xD0, 6, 0xC0, 4, 0x70, 3, 0x60, 2, 0x50, 1, 0x30, 0, 0};
    memcpy(b + 16, uw, sizeof uw);
    uint32_t enter_off = 64;
    uint32_t rf[3] = {enter_off, JIT_ARENA, 16};
    memcpy(b, rf, sizeof rf);
    Em e = {b + enter_off, b + 4096, 0};
    t->enter = (JitEnter)(void *)e.p;
    static const int saved[8] = {RBX, RBP, RSI, RDI, R12, R13, R14, R15};
    for (int i = 0; i < 8; i++) push_r(&e, saved[i]);
    orr(&e, 0, 1, 0, 0x83, 1, X_SUB, RSP), b1(&e, 40);
    mov_rr64(&e, RBX, ARG0);
    mov_rr64(&e, RAX, ARG1);
    mov_rm64(&e, R14, MB(ARG2, 0));
    mov_rm64(&e, R13, MB(ARG2, 8));
    mov_rm64(&e, R12, MB(ARG2, 16));
    mov_ri64(&e, R15, GUEST_BASE);
    orr(&e, 0, 0, 0, 0xFF, 1, 4, RAX);  // jmp rax
    t->exit = e.p;
    mov_mr(&e, FL(JO_SITE), RDX);
    orr(&e, 0, 1, 0, 0x83, 1, X_ADD, RSP), b1(&e, 40);
    for (int i = 7; i >= 0; i--) pop_r(&e, saved[i]);
    b1(&e, 0xC3);
    // thunks: bad_access(eax, size, write) with every register the blocks may hold kept
    static const int vol[9] = {RAX, RCX, RDX, RSI, RDI, R8, R9, R10, R11};
    for (int lg = 0; lg < 4; lg++)
        for (int wr = 0; wr < 2; wr++) {
            t->thunk[lg][wr] = e.p;
            for (int i = 0; i < 9; i++) push_r(&e, vol[i]);
            orr(&e, 0, 1, 0, 0x83, 1, X_SUB, RSP), b1(&e, 32);
            mov_rr(&e, ARG0, RAX);
            mov_ri_keep(&e, ARG1, 1u << lg);
            mov_ri_keep(&e, ARG2, (uint32_t)wr);
            mov_ri64(&e, RAX, (uint64_t)(uintptr_t)bad_access);
            call_r(&e, RAX);
            orr(&e, 0, 1, 0, 0x83, 1, X_ADD, RSP), b1(&e, 32);
            for (int i = 8; i >= 0; i--) pop_r(&e, vol[i]);
            b1(&e, 0xC3);
        }
    if (e.bad) return 0;
    t->code0 = (uint8_t *)(((uintptr_t)e.p + 63) & ~(uintptr_t)63);
    t->rf = b;
    if (!jit_mem_register(b, t->rf)) t->rf = NULL;
    return 1;
}


// ================================================================ blocks
// The whole block: its words checked, its instructions, the fall-through exit, and the changed-code exit
static void jit_emit_block(J *j, const Op *ops, const uint32_t *mem, uint32_t n) {
    Em *e = &j->e;
    JitTls *t = j->t;
    uint32_t pc = j->pc0;
    uint8_t *entry = e->p, *chg[JIT_MAX];
    if (!j->test) {  // the words it was made from (through a register: code may be above 2 GB, a plugin's)
        mov_ri_keep(e, RAX, pc);
        for (uint32_t i = 0; i < n; i++) {
            alu_mi(e, X_CMP, GMEM((int32_t)(4 * i)), mem[i]);
            chg[i] = jcc32(e, CC_NE);
        }
    }
    if (stats_on > 0) alu_mi64(e, X_ADD, FL(JO_INSNS), n);
    if (!j->test) mov_mi(e, MB(R13, 0), pc);
    for (uint32_t i = 0; i < n; i++) emit_insn(j, &ops[i], pc + 4 * i, i + 1 == n);
    exit_direct(j, pc + 4 * n, 0, pc + 4 * (n - 1));
    if (!j->test) {  // changed: rebuild (edx: this entry, to send its callers on to the new block)
        for (uint32_t i = 0; i < n; i++) here32(e, chg[i]);
        mov_ri_keep(e, RAX, pc);
        mov_mi(e, FL(JO_EXIT), 1);
        mov_ri_keep(e, RDX, (uint32_t)(entry - t->base));
        exit_jmp(j);
    }
}

// an exit's jump (site: its rel32's offset in the arena) on to the block at to
static void jit_patch_site(JitTls *t, uint32_t site, const uint8_t *to) { patch32(t->base + site, to); }

// old's entry jumps on to new (callers chained to old)
static void jit_redirect(uint8_t *old, const uint8_t *new) {
    old[0] = 0xE9;
    patch32(old + 1, new);
}

static void jit_code_written(void *p, size_t n) { (void)p, (void)n; }  // (x86: coherent)
