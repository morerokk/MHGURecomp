// The JIT's AArch64 code (runtime/interp_jit.h has the design; the translations are interp_jit_x64.h's, one for one).
// Blocks: x19 the Cpu, x20 GUEST_BASE, x21 &g_ip_pc, x22 &g_ip_last, x23 &g_code_gen; w0-w9, w16, w17 scratch (x18 is
// left alone: the platform's); the exit takes w0 (next pc) and w1 (site). Guest memory is [x20, wN, uxtw]. AArch32's
// condition codes and carry are AArch64's, so ADDS / SUBS / ADCS / SBCS leave the guest's NZCV in the host's flags,
// which CSET stores (a byte each, read back a byte at a time as on x86-64). Code written or patched is followed by an
// instruction cache flush (jit_code_written).

enum { XC = 19, XG = 20, XIP = 21, XIPL = 22, XGEN = 23, XZR = 31 };
enum { A_EQ, A_NE, A_HS, A_LO, A_MI, A_PL, A_VS, A_VC, A_HI, A_LS, A_GE, A_LT, A_GT, A_LE, A_AL };
enum { SH_LSL, SH_LSR, SH_ASR, SH_ROR };
enum { F_N = 68, F_Z = 72, F_C = 76, F_V = 80 };

// ================================================================ AArch64 encoding
static void ins(Em *e, uint32_t w) { b4(e, w); }
static void movz(Em *e, int sf, int rd, uint32_t imm, int hw) {
    ins(e, (sf ? 0xD2800000u : 0x52800000u) | (uint32_t)hw << 21 | (imm & 0xFFFF) << 5 | (uint32_t)rd);
}
static void movk(Em *e, int sf, int rd, uint32_t imm, int hw) {
    ins(e, (sf ? 0xF2800000u : 0x72800000u) | (uint32_t)hw << 21 | (imm & 0xFFFF) << 5 | (uint32_t)rd);
}
static void mov_w(Em *e, int rd, uint32_t v) {  // a 32-bit constant
    if (!(v >> 16)) movz(e, 0, rd, v, 0);
    else if (!(v & 0xFFFF)) movz(e, 0, rd, v >> 16, 1);
    else if ((v >> 16) == 0xFFFF) ins(e, 0x12800000u | (~v & 0xFFFF) << 5 | (uint32_t)rd);  // movn
    else movz(e, 0, rd, v, 0), movk(e, 0, rd, v >> 16, 1);
}
static void mov_x(Em *e, int rd, uint64_t v) {  // a 64-bit constant (pointers)
    int first = 1;
    for (int hw = 0; hw < 4; hw++) {
        uint32_t part = (uint32_t)(v >> (16 * hw)) & 0xFFFF;
        if (!part) continue;
        if (first) movz(e, 1, rd, part, hw), first = 0;
        else movk(e, 1, rd, part, hw);
    }
    if (first) movz(e, 1, rd, 0, 0);
}
// add / sub (shifted register; rd 31 = zr with S)
static void addsub_r(Em *e, int sub, int S, int sf, int rd, int rn, int rm, int sh, int amt) {
    ins(e, 0x0B000000u | (uint32_t)sf << 31 | (uint32_t)sub << 30 | (uint32_t)S << 29 | (uint32_t)sh << 22 |
               (uint32_t)rm << 16 | (uint32_t)amt << 10 | (uint32_t)rn << 5 | (uint32_t)rd);
}
static void addsub_i(Em *e, int sub, int S, int sf, int rd, int rn, uint32_t imm12, int sh12) {
    ins(e, 0x11000000u | (uint32_t)sf << 31 | (uint32_t)sub << 30 | (uint32_t)S << 29 | (uint32_t)sh12 << 22 |
               (imm12 & 0xFFF) << 10 | (uint32_t)rn << 5 | (uint32_t)rd);
}
enum { L_AND, L_ORR, L_EOR, L_ANDS };
static void logic_r(Em *e, int opc, int N, int sf, int rd, int rn, int rm, int sh, int amt) {
    ins(e, 0x0A000000u | (uint32_t)sf << 31 | (uint32_t)opc << 29 | (uint32_t)sh << 22 | (uint32_t)N << 21 |
               (uint32_t)rm << 16 | (uint32_t)amt << 10 | (uint32_t)rn << 5 | (uint32_t)rd);
}
static void mov_rr(Em *e, int rd, int rm) { logic_r(e, L_ORR, 0, 0, rd, XZR, rm, 0, 0); }
static void mov_rr64(Em *e, int rd, int rm) { logic_r(e, L_ORR, 0, 1, rd, XZR, rm, 0, 0); }
static void tst_rr(Em *e, int sf, int r) { logic_r(e, L_ANDS, 0, sf, XZR, r, r, 0, 0); }
static void cmp_rr(Em *e, int rn, int rm) { addsub_r(e, 1, 1, 0, XZR, rn, rm, 0, 0); }
static void cmp_ri(Em *e, int rn, uint32_t imm12, int sh12) { addsub_i(e, 1, 1, 0, XZR, rn, imm12, sh12); }
static void adcsbc(Em *e, int sbc, int S, int rd, int rn, int rm) {
    ins(e, 0x1A000000u | (uint32_t)sbc << 30 | (uint32_t)S << 29 | (uint32_t)rm << 16 | (uint32_t)rn << 5 | (uint32_t)rd);
}
static void shiftv(Em *e, int kind, int rd, int rn, int rm) {
    ins(e, 0x1AC02000u | (uint32_t)rm << 16 | (uint32_t)kind << 10 | (uint32_t)rn << 5 | (uint32_t)rd);
}
enum { BF_S, BF_B, BF_U };  // SBFM, BFM, UBFM
static void bfm(Em *e, int opc, int sf, int rd, int rn, int immr, int imms) {
    ins(e, 0x13000000u | (uint32_t)sf << 31 | (uint32_t)opc << 29 | (uint32_t)sf << 22 | (uint32_t)immr << 16 |
               (uint32_t)imms << 10 | (uint32_t)rn << 5 | (uint32_t)rd);
}
static void lsl_i(Em *e, int rd, int rn, int s) { bfm(e, BF_U, 0, rd, rn, (32 - s) & 31, 31 - s); }
static void lsr_i(Em *e, int rd, int rn, int s) { bfm(e, BF_U, 0, rd, rn, s, 31); }
static void asr_i(Em *e, int rd, int rn, int s) { bfm(e, BF_S, 0, rd, rn, s, 31); }
static void ror_i(Em *e, int rd, int rn, int s) {  // extr rd, rn, rn, #s
    ins(e, 0x13800000u | (uint32_t)rn << 16 | (uint32_t)s << 10 | (uint32_t)rn << 5 | (uint32_t)rd);
}
static void ubfx(Em *e, int rd, int rn, int lsb, int width) { bfm(e, BF_U, 0, rd, rn, lsb, lsb + width - 1); }
static void csel(Em *e, int rd, int rn, int rm, int cond) {
    ins(e, 0x1A800000u | (uint32_t)rm << 16 | (uint32_t)cond << 12 | (uint32_t)rn << 5 | (uint32_t)rd);
}
static void cset(Em *e, int rd, int cond) {  // csinc rd, wzr, wzr, !cond
    ins(e, 0x1A800400u | 31u << 16 | (uint32_t)(cond ^ 1) << 12 | 31u << 5 | (uint32_t)rd);
}
static void madd(Em *e, int sub, int rd, int rn, int rm, int ra) {
    ins(e, 0x1B000000u | (uint32_t)sub << 15 | (uint32_t)rm << 16 | (uint32_t)ra << 10 | (uint32_t)rn << 5 | (uint32_t)rd);
}
static void maddl(Em *e, int uns, int rd, int rn, int rm, int ra) {  // (s|u)maddl xd, wn, wm, xa
    ins(e, (uns ? 0x9BA00000u : 0x9B200000u) | (uint32_t)rm << 16 | (uint32_t)ra << 10 | (uint32_t)rn << 5 | (uint32_t)rd);
}
// loads / stores: unsigned scaled immediate (base + off), register (base + wm, uxtw), unscaled (ldur)
#define M_LDRW 0xB9400000u
#define M_STRW 0xB9000000u
#define M_LDRX 0xF9400000u
#define M_STRX 0xF9000000u
#define M_LDRB 0x39400000u
#define M_STRB 0x39000000u
#define M_LDRH 0x79400000u
#define M_STRH 0x79000000u
#define M_LDRSB 0x39C00000u
#define M_LDRSH 0x79C00000u
static void ldst(Em *e, uint32_t op, int rt, int rn, uint32_t off) {
    uint32_t scale = op >> 30;  // (the size field)
    if ((off & ((1u << scale) - 1)) || (off >> scale) > 0xFFF) {
        e->bad = 1;
        return;
    }
    ins(e, op | (off >> scale) << 10 | (uint32_t)rn << 5 | (uint32_t)rt);
}
static void ldst_rx(Em *e, uint32_t op, int rt, int rn, int rm) {  // [xn, wm, uxtw]: the immediate opcode's register form
    ins(e, (op & ~0x01000000u) | 0x00204800u | (uint32_t)rm << 16 | (uint32_t)rn << 5 | (uint32_t)rt);
}
static void ldur(Em *e, int sf, int rt, int rn, int32_t off) {
    ins(e, (sf ? 0xF8400000u : 0xB8400000u) | ((uint32_t)off & 0x1FF) << 12 | (uint32_t)rn << 5 | (uint32_t)rt);
}
static void add_uxtw(Em *e, int xd, int xn, int wm) {
    ins(e, 0x8B204000u | (uint32_t)wm << 16 | (uint32_t)xn << 5 | (uint32_t)xd);
}
static void stp_x(Em *e, int pre, int rt, int rt2, int rn, int off) {  // stp xt, xt2, [xn, #off](!)
    ins(e, (pre ? 0xA9800000u : 0xA9000000u) | (((uint32_t)(off / 8)) & 0x7F) << 15 | (uint32_t)rt2 << 10 |
               (uint32_t)rn << 5 | (uint32_t)rt);
}
static void ldp_x(Em *e, int post, int rt, int rt2, int rn, int off) {  // ldp xt, xt2, [xn](, #off) / [xn, #off]
    ins(e, (post ? 0xA8C00000u : 0xA9400000u) | (((uint32_t)(off / 8)) & 0x7F) << 15 | (uint32_t)rt2 << 10 |
               (uint32_t)rn << 5 | (uint32_t)rt);
}
static void blr(Em *e, int rn) { ins(e, 0xD63F0000u | (uint32_t)rn << 5); }
static void br(Em *e, int rn) { ins(e, 0xD61F0000u | (uint32_t)rn << 5); }
// branches with a field patched later: the site is the instruction
static uint8_t *b_(Em *e) { uint8_t *s = e->p; ins(e, 0x14000000u); return s; }
static uint8_t *bcond(Em *e, int cond) { uint8_t *s = e->p; ins(e, 0x54000000u | (uint32_t)cond); return s; }
static uint8_t *cbz(Em *e, int nz, int rt) { uint8_t *s = e->p; ins(e, (nz ? 0x35000000u : 0x34000000u) | (uint32_t)rt); return s; }
static uint8_t *tbz(Em *e, int nz, int rt, int bit) {
    uint8_t *s = e->p;
    ins(e, (nz ? 0x37000000u : 0x36000000u) | (uint32_t)bit << 19 | (uint32_t)rt);
    return s;
}
static void patch_br(uint8_t *site, const uint8_t *to) {  // (by its kind: B / BL, B.cond / CBZ / CBNZ, TBZ / TBNZ)
    uint32_t w;
    memcpy(&w, site, 4);
    int32_t d = (int32_t)((to - site) / 4);
    if ((w & 0x7C000000u) == 0x14000000u) w = (w & 0xFC000000u) | ((uint32_t)d & 0x3FFFFFF);
    else if ((w & 0x7E000000u) == 0x36000000u) w = (w & ~(0x3FFFu << 5)) | ((uint32_t)d & 0x3FFF) << 5;
    else w = (w & ~(0x7FFFFu << 5)) | ((uint32_t)d & 0x7FFFF) << 5;
    memcpy(site, &w, 4);
}
static void here(Em *e, uint8_t *site) {
    if (site && !e->bad) patch_br(site, e->p);
}
static void b_to(Em *e, const uint8_t *to) {
    uint8_t *s = b_(e);
    if (!e->bad) patch_br(s, to);
}
static void bl_to(Em *e, const uint8_t *to) {
    uint8_t *s = e->p;
    ins(e, 0x94000000u);
    if (!e->bad) patch_br(s, to);
}

// ================================================================ translation
#define GOFF(n) (4u * (uint32_t)(n))
static void ld_greg(J *j, int r, uint32_t n, uint32_t pc) {
    if (n == 15) mov_w(&j->e, r, pc + 8);
    else ldst(&j->e, M_LDRW, r, XC, GOFF(n));
}
static void st_greg(J *j, int r, uint32_t n) { ldst(&j->e, M_STRW, r, XC, GOFF(n)); }
static void st_field(J *j, uint32_t off, uint32_t v) {  // (w16)
    if (v) mov_w(&j->e, 16, v), ldst(&j->e, M_STRW, 16, XC, off);
    else ldst(&j->e, M_STRW, XZR, XC, off);
}

static void exit_jmp(J *j) { b_to(&j->e, j->t->exit); }

enum { LK_NONE, LK_YES, LK_TMP };
static void exit_ind(J *j, int link, uint32_t retpc) {  // w0: the next pc
    Em *e = &j->e;
    if (link == LK_YES) st_field(j, JO_LINK, 1);
    if (link == LK_TMP) ldst(e, M_LDRW, 16, XC, JO_TLINK), ldst(e, M_STRW, 16, XC, JO_LINK);
    if (link != LK_NONE) st_field(j, JO_RETPC, retpc);
    movz(e, 0, 1, 0, 0);
    exit_jmp(j);
}

static void exit_direct(J *j, uint32_t target, int link, uint32_t pc) {
    Em *e = &j->e;
    uint8_t *to_stub[3];
    int n = 0;
    if (!link) {
        ldst(e, M_LDRW, 16, XC, JO_RET);
        mov_w(e, 17, target);
        cmp_rr(e, 16, 17);
        to_stub[n++] = bcond(e, A_EQ);
    }
    if (target <= j->pc0) {
        ldst(e, M_LDRW, 16, XGEN, 0);
        mov_w(e, 17, j->t->gen);
        cmp_rr(e, 16, 17);
        to_stub[n++] = bcond(e, A_NE);
    }
    uint8_t *site = b_(e);
    for (int i = 0; i < n; i++) here(e, to_stub[i]);
    here(e, site);
    mov_w(e, 0, target);
    if (link) st_field(j, JO_LINK, 1), st_field(j, JO_RETPC, pc + 4);
    mov_w(e, 1, (uint32_t)(site - j->t->base));
    exit_jmp(j);
}

// a branch taken when cond fails (NULL: always passes); flags read a byte each
static uint8_t *cond_false(J *j, uint32_t cond) {
    Em *e = &j->e;
    static const uint32_t flag[8] = {F_Z, F_Z, F_C, F_C, F_N, F_N, F_V, F_V};
    if (cond < 8) {
        ldst(e, M_LDRB, 16, XC, flag[cond]);
        return cbz(e, (int)(cond & 1), 16);  // (EQ / CS / MI / VS fail on 0, their opposites on 1)
    }
    if (cond >= 14) return NULL;
    // the guest's NZCV into the host's flags, and the condition itself
    ldst(e, M_LDRB, 16, XC, F_N);
    ldst(e, M_LDRB, 17, XC, F_Z);
    logic_r(e, L_ORR, 0, 0, 16, 17, 16, SH_LSL, 1);
    ldst(e, M_LDRB, 17, XC, F_C);
    logic_r(e, L_ORR, 0, 0, 16, 17, 16, SH_LSL, 1);
    ldst(e, M_LDRB, 17, XC, F_V);
    logic_r(e, L_ORR, 0, 0, 16, 17, 16, SH_LSL, 1);
    lsl_i(e, 16, 16, 28);
    ins(e, 0xD51B4200u | 16u);  // msr nzcv, x16
    return bcond(e, (int)(cond ^ 1));
}

static void store_nzcv(J *j) {  // after ADDS / SUBS / ADCS / SBCS
    Em *e = &j->e;
    cset(e, 16, A_MI), ldst(e, M_STRB, 16, XC, F_N);
    cset(e, 16, A_EQ), ldst(e, M_STRB, 16, XC, F_Z);
    cset(e, 16, A_HS), ldst(e, M_STRB, 16, XC, F_C);
    cset(e, 16, A_VS), ldst(e, M_STRB, 16, XC, F_V);
}
static void store_nz(J *j) {
    Em *e = &j->e;
    cset(e, 16, A_MI), ldst(e, M_STRB, 16, XC, F_N);
    cset(e, 16, A_EQ), ldst(e, M_STRB, 16, XC, F_Z);
}
static void carry_in(J *j) {  // host C = guest C (the same sense on both)
    ldst(&j->e, M_LDRB, 16, XC, F_C);
    cmp_ri(&j->e, 16, 1, 0);
}

// the bad-access check on the guest address in w0 (g_ip_pc set): the thunk keeps every register
static void chk(J *j, uint32_t pc, int size, int wr) {
    Em *e = &j->e;
    (void)pc;
    if (j->test) return;
    int lg = size >= 8 ? 3 : size >= 4 ? 2 : size >= 2 ? 1 : 0;
    movz(e, 0, 17, 0x8800, 1);
    addsub_r(e, 0, 0, 0, 16, 17, 0, SH_LSL, 1);  // 2a - 0x78000000: (a & 0x7FFFFFFF) - 0x3C000000, doubled
    lsr_i(e, 16, 16, 28);
    uint8_t *flt = cbz(e, 0, 16);
    cmp_ri(e, 0, 0x10, 1);  // 0x10000
    uint8_t *ok = bcond(e, A_HS);
    here(e, flt);
    bl_to(e, j->t->thunk[lg][wr]);
    here(e, ok);
}
static void set_ip(J *j, uint32_t pc) {
    if (j->test) return;
    mov_w(&j->e, 16, pc);
    ldst(&j->e, M_STRW, 16, XIP, 0);
}

// an immediate-shifted register operand into w1; want: its carry out into w3 (*carry 2)
static void j_shift_imm(J *j, uint32_t w, uint32_t pc, int want, int *carry) {
    Em *e = &j->e;
    uint32_t rm = BITS(w, 0, 4), typ = BITS(w, 5, 2), amt = BITS(w, 7, 5);
    ld_greg(j, 1, rm, pc);
    *carry = want ? 2 : -1;
    switch (typ) {
    case 0:
        if (!amt) { *carry = -1; return; }
        if (want) ubfx(e, 3, 1, 32 - (int)amt, 1);
        lsl_i(e, 1, 1, (int)amt);
        return;
    case 1:
        if (!amt) {  // LSR #32
            if (want) lsr_i(e, 3, 1, 31);
            movz(e, 0, 1, 0, 0);
            return;
        }
        if (want) ubfx(e, 3, 1, (int)amt - 1, 1);
        lsr_i(e, 1, 1, (int)amt);
        return;
    case 2:
        if (!amt) {  // ASR #32
            if (want) lsr_i(e, 3, 1, 31);
            asr_i(e, 1, 1, 31);
            return;
        }
        if (want) ubfx(e, 3, 1, (int)amt - 1, 1);
        asr_i(e, 1, 1, (int)amt);
        return;
    default:
        if (!amt) {  // RRX
            ldst(e, M_LDRB, 16, XC, F_C);
            if (want) ubfx(e, 3, 1, 0, 1);
            lsr_i(e, 1, 1, 1);
            logic_r(e, L_ORR, 0, 0, 1, 1, 16, SH_LSL, 31);
            return;
        }
        if (want) ubfx(e, 3, 1, (int)amt - 1, 1);
        ror_i(e, 1, 1, (int)amt);
        return;
    }
}

// data processing: as interp_jit_x64.h's t_dp (a in w0, operand 2 in w1, the result in w0)
static int t_dp(J *j, uint32_t w, uint32_t pc, int *pcw) {
    Em *e = &j->e;
    uint32_t I = BIT(w, 25), opc = BITS(w, 21, 4), S = BIT(w, 20), rn = BITS(w, 16, 4), rd = BITS(w, 12, 4);
    int logical = opc <= 1 || opc == 8 || opc == 9 || opc >= 12, test = opc >= 8 && opc <= 11, carry = -1;
    if (S && rd == 15 && !test) return 0;
    if (!S && test) return 0;
    if (!I && BIT(w, 4)) {  // register-specified shift
        uint32_t rs = BITS(w, 8, 4), rm = BITS(w, 0, 4), typ = BITS(w, 5, 2);
        if ((S && logical) || rs == 15 || rm == 15) return 0;
        ldst(e, M_LDRW, 16, XC, GOFF(rs));
        ubfx(e, 16, 16, 0, 8);
        ldst(e, M_LDRW, 2, XC, GOFF(rm));
        switch (typ) {
        case 0:
        case 1:
            shiftv(e, (int)typ, 2, 2, 16);
            cmp_ri(e, 16, 32, 0);
            csel(e, 2, 2, XZR, A_LO);
            break;
        case 2:
            movz(e, 0, 17, 31, 0);
            cmp_ri(e, 16, 31, 0);
            csel(e, 16, 16, 17, A_LS);
            shiftv(e, SH_ASR, 2, 2, 16);
            break;
        default:
            shiftv(e, SH_ROR, 2, 2, 16);
            break;
        }
        mov_rr(e, 1, 2);
    } else if (I) {
        int c;
        mov_w(e, 1, imm_operand(w, &c));
        carry = c;
    } else {
        j_shift_imm(j, w, pc, S && logical, &carry);
    }
    if (opc != 13 && opc != 15) ld_greg(j, 0, rn, pc);
    int s = (int)S;
    switch (opc) {
    case 0: case 8: logic_r(e, s ? L_ANDS : L_AND, 0, 0, 0, 0, 1, 0, 0); break;
    case 1: case 9: logic_r(e, L_EOR, 0, 0, 0, 0, 1, 0, 0); break;
    case 2: case 10: addsub_r(e, 1, s, 0, 0, 0, 1, 0, 0); break;
    case 3: addsub_r(e, 1, s, 0, 0, 1, 0, 0, 0); break;
    case 4: case 11: addsub_r(e, 0, s, 0, 0, 0, 1, 0, 0); break;
    case 5: carry_in(j), adcsbc(e, 0, s, 0, 0, 1); break;
    case 6: carry_in(j), adcsbc(e, 1, s, 0, 0, 1); break;
    case 7: carry_in(j), adcsbc(e, 1, s, 0, 1, 0); break;
    case 12: logic_r(e, L_ORR, 0, 0, 0, 0, 1, 0, 0); break;
    case 13: mov_rr(e, 0, 1); break;
    case 14: logic_r(e, s ? L_ANDS : L_AND, 1, 0, 0, 0, 1, 0, 0); break;
    default: logic_r(e, L_ORR, 1, 0, 0, XZR, 1, 0, 0); break;
    }
    if (S) {
        if (!logical) {
            store_nzcv(j);
        } else {
            if (opc != 0 && opc != 8 && opc != 14) tst_rr(e, 0, 0);
            store_nz(j);
            if (carry == 0 || carry == 1) {
                if (carry) movz(e, 0, 16, 1, 0), ldst(e, M_STRB, 16, XC, F_C);
                else ldst(e, M_STRB, XZR, XC, F_C);
            } else if (carry == 2) {
                ldst(e, M_STRB, 3, XC, F_C);
            }
        }
    }
    if (test) return 1;
    if (rd == 15) *pcw = 1;
    else st_greg(j, 0, rd);
    return 1;
}

// ea in w0, the writeback value in w2 (offset: w1 for reg, off otherwise)
static void ea_wb(J *j, uint32_t w, uint32_t pc, uint32_t rn, int reg, uint32_t off, int wb) {
    Em *e = &j->e;
    uint32_t P = BIT(w, 24), U = BIT(w, 23);
    ld_greg(j, 0, rn, pc);
    if (reg) {
        if (P) {
            addsub_r(e, !U, 0, 0, 0, 0, 1, 0, 0);
            if (wb) mov_rr(e, 2, 0);
        } else if (wb) {
            addsub_r(e, !U, 0, 0, 2, 0, 1, 0, 0);
        }
    } else if (P) {
        if (off) addsub_i(e, !U, 0, 0, 0, 0, off, 0);
        if (wb) mov_rr(e, 2, 0);
    } else if (wb) {
        addsub_i(e, !U, 0, 0, 2, 0, off, 0);
    }
}

static int t_ldst(J *j, uint32_t w, uint32_t pc, int *pcw) {
    Em *e = &j->e;
    uint32_t I = BIT(w, 25), P = BIT(w, 24), B = BIT(w, 22), Wb = BIT(w, 21), L = BIT(w, 20);
    uint32_t rn = BITS(w, 16, 4), rt = BITS(w, 12, 4);
    int wb = (!P || Wb) && rn != 15, carry;
    if (I) j_shift_imm(j, w & ~0x10u, pc, 0, &carry);
    ea_wb(j, w, pc, rn, (int)I, BITS(w, 0, 12), wb);
    if (rn != 13) set_ip(j, pc), chk(j, pc, B ? 1 : 4, !L);
    if (L) {
        ldst_rx(e, B ? M_LDRB : M_LDRW, 4, XG, 0);
        if (wb) st_greg(j, 2, rn);
        if (rt == 15) mov_rr(e, 0, 4), *pcw = 1;
        else st_greg(j, 4, rt);
    } else {
        ld_greg(j, 4, rt, pc);
        ldst_rx(e, B ? M_STRB : M_STRW, 4, XG, 0);
        if (wb) st_greg(j, 2, rn);
    }
    return 1;
}

static int t_xldst(J *j, uint32_t w, uint32_t pc) {
    Em *e = &j->e;
    uint32_t I = BIT(w, 22), P = BIT(w, 24), Wb = BIT(w, 21), L = BIT(w, 20), op2 = BITS(w, 5, 2);
    uint32_t rn = BITS(w, 16, 4), rt = BITS(w, 12, 4), rm = BITS(w, 0, 4);
    int wb = (!P || Wb) && rn != 15, dual = op2 >= 2 && !L;
    if (rt == 15 || (dual && (rt & 1 || rt == 14))) return 0;
    if (!I) ld_greg(j, 1, rm, pc);
    ea_wb(j, w, pc, rn, !I, (BITS(w, 8, 4) << 4) | BITS(w, 0, 4), wb);
    int size = dual ? 8 : op2 == 2 ? 1 : 2;
    if (rn != 13) set_ip(j, pc), chk(j, pc, size, !L && op2 != 2);
    if (dual) {  // (the second word's address wraps at 4 GB as the guest's)
        if (op2 == 2) {  // LDRD
            ldst_rx(e, M_LDRW, 4, XG, 0);
            addsub_i(e, 0, 0, 0, 0, 0, 4, 0);
            ldst_rx(e, M_LDRW, 5, XG, 0);
            if (wb) st_greg(j, 2, rn);
            st_greg(j, 4, rt);
            st_greg(j, 5, rt + 1);
        } else {  // STRD
            ld_greg(j, 4, rt, pc);
            ld_greg(j, 5, rt + 1, pc);
            ldst_rx(e, M_STRW, 4, XG, 0);
            addsub_i(e, 0, 0, 0, 0, 0, 4, 0);
            ldst_rx(e, M_STRW, 5, XG, 0);
            if (wb) st_greg(j, 2, rn);
        }
        return 1;
    }
    if (op2 == 1 && !L) {  // STRH
        ld_greg(j, 4, rt, pc);
        ldst_rx(e, M_STRH, 4, XG, 0);
        if (wb) st_greg(j, 2, rn);
        return 1;
    }
    ldst_rx(e, op2 == 1 ? M_LDRH : op2 == 2 ? M_LDRSB : M_LDRSH, 4, XG, 0);
    if (wb) st_greg(j, 2, rn);
    st_greg(j, 4, rt);
    return 1;
}

static int t_ldm(J *j, uint32_t w, uint32_t pc, int *pcw) {
    Em *e = &j->e;
    uint32_t P = BIT(w, 24), U = BIT(w, 23), Wb = BIT(w, 21), L = BIT(w, 20), rn = BITS(w, 16, 4);
    uint32_t list = w & 0xFFFF, n = (uint32_t)__builtin_popcount(list);
    if (BIT(w, 22) || !n || rn == 15) return 0;
    int32_t eo = U ? (P ? 4 : 0) : (P ? -(int32_t)(4 * n) : -(int32_t)(4 * n) + 4);
    int32_t wo = U ? (int32_t)(4 * n) : -(int32_t)(4 * n);
    ldst(e, M_LDRW, 0, XC, GOFF(rn));
    if (Wb) addsub_i(e, wo < 0, 0, 0, 2, 0, (uint32_t)(wo < 0 ? -wo : wo), 0);
    if (eo) addsub_i(e, eo < 0, 0, 0, 0, 0, (uint32_t)(eo < 0 ? -eo : eo), 0);
    if (rn != 13) set_ip(j, pc), chk(j, pc, 4, !L);
    uint32_t i = 0;  // (each word's address in w0, wrapping at 4 GB as the guest's)
    if (L) {
        if (Wb) st_greg(j, 2, rn);
        for (uint32_t r = 0; r < 16; r++) {
            if (!BIT(list, r)) continue;
            if (i++) addsub_i(e, 0, 0, 0, 0, 0, 4, 0);
            if (r == 15) {
                ldst_rx(e, M_LDRW, 5, XG, 0);
            } else {
                ldst_rx(e, M_LDRW, 4, XG, 0);
                st_greg(j, 4, r);
            }
        }
        if (BIT(list, 15)) mov_rr(e, 0, 5), *pcw = 1;
    } else {
        for (uint32_t r = 0; r < 16; r++) {
            if (!BIT(list, r)) continue;
            if (i++) addsub_i(e, 0, 0, 0, 0, 0, 4, 0);
            ld_greg(j, 4, r, pc);
            ldst_rx(e, M_STRW, 4, XG, 0);
        }
        if (Wb) st_greg(j, 2, rn);
    }
    return 1;
}

static int t_mul(J *j, uint32_t w) {
    Em *e = &j->e;
    uint32_t op = BITS(w, 21, 3), S = BIT(w, 20), rd = BITS(w, 16, 4), ra = BITS(w, 12, 4), rm = BITS(w, 8, 4);
    uint32_t rn = BITS(w, 0, 4);
    if (op == 2 || rd == 15 || rm == 15 || rn == 15 || ((op & 1 || op >= 3) && ra == 15)) return 0;
    ldst(e, M_LDRW, 0, XC, GOFF(rn));
    ldst(e, M_LDRW, 1, XC, GOFF(rm));
    if (op <= 1 || op == 3) {
        if (op == 3 && S) return 0;
        if (op) ldst(e, M_LDRW, 2, XC, GOFF(ra));
        madd(e, op == 3, 0, 0, 1, op ? 2 : XZR);
        if (S) tst_rr(e, 0, 0), store_nz(j);
        st_greg(j, 0, rd);
        return 1;
    }
    if (op & 1) {
        ldst(e, M_LDRW, 2, XC, GOFF(ra));
        ldst(e, M_LDRW, 4, XC, GOFF(rd));
        logic_r(e, L_ORR, 0, 1, 2, 2, 4, SH_LSL, 32);
    }
    maddl(e, op < 6, 0, 0, 1, op & 1 ? 2 : XZR);
    if (S) tst_rr(e, 1, 0), store_nz(j);
    st_greg(j, 0, ra);
    bfm(e, BF_U, 1, 0, 0, 32, 63);  // lsr x0, x0, #32
    st_greg(j, 0, rd);
    return 1;
}

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
            ldst(e, M_LDRW, 0, XC, GOFF(rm));
            if (rot) ror_i(e, 0, 0, (int)rot);
            bfm(e, p1 >= 6 ? BF_U : BF_S, 0, 0, 0, 0, p1 & 1 ? 15 : 7);
            if (rn != 15) ldst(e, M_LDRW, 1, XC, GOFF(rn)), addsub_r(e, 0, 0, 0, 0, 0, 1, 0, 0);
            st_greg(j, 0, rd);
            return 1;
        }
        if (p1 == 3 && op2 == 1) {  // REV
            if (rm == 15) return 0;
            ldst(e, M_LDRW, 0, XC, GOFF(rm));
            ins(e, 0x5AC00800u);  // rev w0, w0
            st_greg(j, 0, rd);
            return 1;
        }
        return 0;
    }
    uint32_t lsb = BITS(w, 7, 5), msb = BITS(w, 16, 5);
    if (((op1 & 0x1E) == 0x1A || (op1 & 0x1E) == 0x1E) && (op2 & 3) == 2) {  // SBFX / UBFX
        uint32_t width = msb + 1;
        if (rm == 15 || lsb + width > 32) return 0;
        ldst(e, M_LDRW, 0, XC, GOFF(rm));
        bfm(e, (op1 & 0x1E) == 0x1E ? BF_U : BF_S, 0, 0, 0, (int)lsb, (int)(lsb + width - 1));
        st_greg(j, 0, rd);
        return 1;
    }
    if ((op1 & 0x1E) == 0x1C && (op2 & 3) == 0) {  // BFC / BFI
        if (msb < lsb) return 0;
        ldst(e, M_LDRW, 0, XC, GOFF(rd));
        if (rm != 15) ldst(e, M_LDRW, 1, XC, GOFF(rm));
        bfm(e, BF_B, 0, 0, rm == 15 ? XZR : 1, (int)((32 - lsb) & 31), (int)(msb - lsb));
        st_greg(j, 0, rd);
        return 1;
    }
    return 0;
}

static void thumb_check(J *j, uint32_t pc) {  // (target in w0)
    Em *e = &j->e;
    uint8_t *ok = tbz(e, 0, 0, 0);
    mov_rr64(e, 0, XC);
    mov_w(e, 1, pc);
    mov_x(e, 16, (uint64_t)(uintptr_t)rt_thumb);
    blr(e, 16);
    here(e, ok);
}

static void call_op(J *j, const Op *o, uint32_t pc) {
    Em *e = &j->e;
    Op *c = NULL;
    if (j->nops < j->maxops) c = j->ops - ++j->nops, *c = *o;
    else e->bad = 1;
    if (!j->test) {
        mov_w(e, 16, pc);
        ldst(e, M_STRW, 16, XIP, 0);
        ldst(e, M_STRW, 16, XIPL, 0);
    }
    ldst(e, M_STRW, XZR, XC, JO_TLINK);
    mov_rr64(e, 0, XC);
    mov_x(e, 1, (uint64_t)(uintptr_t)c);
    mov_w(e, 2, pc);
    addsub_i(e, 0, 0, 1, 3, XC, JO_TLINK, 0);
    mov_x(e, 16, (uint64_t)(uintptr_t)o->fn);
    blr(e, 16);
}

static void emit_insn(J *j, const Op *o, uint32_t pc, int last) {
    Em *e = &j->e;
    uint32_t w = o->w, cond = w >> 28, cls = BITS(w, 25, 3);
    int pcw = 0, done = 0;
    uint8_t *skip = cond < 14 ? cond_false(j, cond) : NULL;
    if (cond != 15) {
        switch (cls) {
        case 0:
        case 1: {
            uint32_t op = BITS(w, 20, 5);
            if (!BIT(w, 25)) {
                uint32_t b4_ = BIT(w, 4), b7 = BIT(w, 7);
                if ((op & 0x19) == 0x10 && b7 && !b4_) {
                    break;
                } else if ((op & 0x19) == 0x10 && !b7) {
                    uint32_t op2 = BITS(w, 4, 3), mop = BITS(w, 21, 2), rm = BITS(w, 0, 4), rd = BITS(w, 12, 4);
                    if (mop == 1 && (op2 == 1 || op2 == 3) && rm != 15) {
                        ldst(e, M_LDRW, 0, XC, GOFF(rm));
                        thumb_check(j, pc);
                        if (op2 == 3) mov_w(e, 16, pc + 4), st_greg(j, 16, 14);
                        exit_ind(j, op2 == 3 ? LK_YES : LK_NONE, pc + 4);
                        done = 2;
                    } else if (mop == 3 && op2 == 1 && rm != 15 && rd != 15) {
                        ldst(e, M_LDRW, 0, XC, GOFF(rm));
                        ins(e, 0x5AC01000u);  // clz w0, w0
                        st_greg(j, 0, rd);
                        done = 1;
                    }
                } else if (b4_ && b7) {
                    if (BITS(w, 5, 2) == 0) done = BIT(w, 24) ? 0 : t_mul(j, w);
                    else done = t_xldst(j, w, pc);
                } else {
                    done = t_dp(j, w, pc, &pcw);
                }
            } else if (op == 0x10 || op == 0x14) {  // MOVW / MOVT
                uint32_t rd = BITS(w, 12, 4), v = (BITS(w, 16, 4) << 12) | BITS(w, 0, 12);
                if (rd != 15) {
                    mov_w(e, 16, v);
                    if (op == 0x10) st_greg(j, 16, rd);
                    else ldst(e, M_STRH, 16, XC, GOFF(rd) + 2);
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
            if (BIT(w, 24)) mov_w(e, 16, pc + 4), st_greg(j, 16, 14);
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
            mov_w(e, 16, pc + 4);
            cmp_rr(e, 0, 16);
            uint8_t *on = bcond(e, A_EQ);
            exit_ind(j, LK_TMP, pc + 4);
            here(e, on);
        }
    }
    here(e, skip);
}

// ================================================================ arena
static int jit_header(JitTls *t) {
    Em e = {t->base, t->base + 4096, 0};
    t->enter = (JitEnter)(void *)e.p;
    stp_x(&e, 1, 29, 30, 31, -96);  // stp x29, x30, [sp, #-96]!
    ins(&e, 0x910003FDu);           // mov x29, sp
    stp_x(&e, 0, 19, 20, 31, 16);
    stp_x(&e, 0, 21, 22, 31, 32);
    stp_x(&e, 0, 23, 24, 31, 48);
    mov_rr64(&e, XC, 0);
    ldst(&e, M_LDRX, XIP, 2, 0);
    ldst(&e, M_LDRX, XIPL, 2, 8);
    ldst(&e, M_LDRX, XGEN, 2, 16);
    mov_x(&e, XG, GUEST_BASE);
    br(&e, 1);
    t->exit = e.p;
    ldst(&e, M_STRW, 1, XC, JO_SITE);
    ldp_x(&e, 0, 23, 24, 31, 48);
    ldp_x(&e, 0, 21, 22, 31, 32);
    ldp_x(&e, 0, 19, 20, 31, 16);
    ldp_x(&e, 1, 29, 30, 31, 96);  // ldp x29, x30, [sp], #96
    ins(&e, 0xD65F03C0u);          // ret
    // thunks: bad_access(w0, size, write) with x0-x17 and x29 / x30 kept
    for (int lg = 0; lg < 4; lg++)
        for (int wr = 0; wr < 2; wr++) {
            t->thunk[lg][wr] = e.p;
            stp_x(&e, 1, 0, 1, 31, -160);
            for (int r = 2; r < 18; r += 2) stp_x(&e, 0, r, r + 1, 31, 8 * r);
            stp_x(&e, 0, 29, 30, 31, 144);
            movz(&e, 0, 1, 1u << lg, 0);
            movz(&e, 0, 2, (uint32_t)wr, 0);
            mov_x(&e, 16, (uint64_t)(uintptr_t)bad_access);
            blr(&e, 16);
            ldp_x(&e, 0, 29, 30, 31, 144);
            for (int r = 16; r >= 2; r -= 2) ldp_x(&e, 0, r, r + 1, 31, 8 * r);
            ldp_x(&e, 1, 0, 1, 31, 160);
            ins(&e, 0xD65F03C0u);
        }
    if (e.bad) return 0;
    t->code0 = (uint8_t *)(((uintptr_t)e.p + 63) & ~(uintptr_t)63);
    t->rf = NULL;
    __builtin___clear_cache((char *)t->base, (char *)e.p);
    return 1;
}

// ================================================================ blocks
// The whole block: its words checked (against a literal pool after it), its instructions, the fall-through exit, and
// the changed-code exit
static void lit_load(J *j, int sf, int rt, uint64_t v) {  // ldr wt / xt, =v
    if (j->nlit < (int)(sizeof j->lit / sizeof j->lit[0])) {
        j->lit[j->nlit].at = j->e.p, j->lit[j->nlit].v = v, j->lit[j->nlit].wide = sf;
        j->nlit++;
    } else {
        j->e.bad = 1;
    }
    ins(&j->e, (sf ? 0x58000000u : 0x18000000u) | (uint32_t)rt);
}

static void jit_emit_block(J *j, const Op *ops, const uint32_t *mem, uint32_t n) {
    Em *e = &j->e;
    JitTls *t = j->t;
    uint32_t pc = j->pc0;
    uint8_t *entry = e->p, *chg[JIT_MAX];
    int nchg = 0;
    if (!j->test) {
        mov_w(e, 9, pc);
        add_uxtw(e, 9, XG, 9);
        for (uint32_t i = 0; i < n; i += 2) {
            int two = i + 1 < n;
            ldur(e, two, 16, 9, (int32_t)(4 * i));
            lit_load(j, two, 17, two ? (uint64_t)mem[i] | (uint64_t)mem[i + 1] << 32 : mem[i]);
            addsub_r(e, 1, 1, two, XZR, 16, 17, 0, 0);
            chg[nchg++] = bcond(e, A_NE);
        }
    }
    if (stats_on > 0) {
        ldst(e, M_LDRX, 16, XC, JO_INSNS);
        addsub_i(e, 0, 0, 1, 16, 16, n, 0);
        ldst(e, M_STRX, 16, XC, JO_INSNS);
    }
    if (!j->test) mov_w(e, 16, pc), ldst(e, M_STRW, 16, XIPL, 0);
    for (uint32_t i = 0; i < n; i++) emit_insn(j, &ops[i], pc + 4 * i, i + 1 == n);
    exit_direct(j, pc + 4 * n, 0, pc + 4 * (n - 1));
    if (!j->test) {
        for (int i = 0; i < nchg; i++) here(e, chg[i]);
        mov_w(e, 0, pc);
        st_field(j, JO_EXIT, 1);
        mov_w(e, 1, (uint32_t)(entry - t->base));
        exit_jmp(j);
    }
    // the literal pool
    while ((uintptr_t)e->p & 7) ins(e, 0xD503201Fu);  // (nop)
    for (int i = 0; i < j->nlit; i++) {
        uint8_t *at = e->p;
        if (j->lit[i].wide) b8(e, j->lit[i].v);
        else b4(e, (uint32_t)j->lit[i].v), b4(e, 0);
        if (e->bad) break;
        uint32_t w;
        memcpy(&w, j->lit[i].at, 4);
        w |= ((uint32_t)((at - j->lit[i].at) / 4) & 0x7FFFF) << 5;
        memcpy(j->lit[i].at, &w, 4);
    }
}

static void jit_code_written(void *p, size_t n) { __builtin___clear_cache((char *)p, (char *)p + n); }

static void jit_patch_site(JitTls *t, uint32_t site, const uint8_t *to) {
    jit_write_begin();
    patch_br(t->base + site, to);
    jit_code_written(t->base + site, 4);
    jit_write_end();
}

static void jit_redirect(uint8_t *old, const uint8_t *new) {
    uint32_t w = 0x14000000u;
    jit_write_begin();
    memcpy(old, &w, 4);
    patch_br(old, new);
    jit_code_written(old, 4);
    jit_write_end();
}
