"""Generate C for a whole AArch32 NSO module.

Usage: gen.py <name> <nso> <base> <outdir> [--hle hle_symbols.txt] [--stats]

Functions come from the module's .ARM.exidx unwind table (every compiled function has an entry), its symbols,
pointers to code found in relocated data (vtables, callbacks) and direct call targets. Each function is walked
by recursive descent from its entry, so the literal pools and jump tables that sit between ARM functions are
never lifted as code. A branch that leaves its function becomes a new function start (a tail call), repeated to
a fixpoint, so any split of the code into functions stays correct.
"""
import argparse
import os
import re
import struct
import sys
import time
from collections import Counter

from lifter import Insn, Lifter, Unsupported, bits, bit, hx, sext
from nso import Nso, R_ARM_RELATIVE, R_ARM_ABS32, STT_FUNC


def fname(a):
    return f'f_{a:x}'


def sanitize(sym):
    return 'hle_' + re.sub(r'[^A-Za-z0-9_]', '_', sym)


MOV_LR_PC = 0xE1A0E00F

# Game instructions replaced at generation time: (module, address) -> (expected word, C for that instruction).
# main 0xdbcd34 / 0xdbcd38: the docked render size, `mov r0, #1920` / `movw r7, #1080` (the constants resolution mods
# patch), taken from the settings instead (runtime/config.c).
PATCHES = {
    ('main', 0xdbcd34): (0xE3A00D1E, '{ extern uint32_t g_render_size[2]; c->r[0] = g_render_size[0]; }'),
    ('main', 0xdbcd38): (0xE3007438, '{ extern uint32_t g_render_size[2]; c->r[7] = g_render_size[1]; }'),
    # 2D UI canvas size: mov r7, #1280 (0xdbcd48) / mov r7, #720 (0xdbcd54)
    ('main', 0xdbcd48): (0xE3A07C05, '{ extern uint32_t g_ui_size[2]; c->r[7] = g_ui_size[0]; }'),
    ('main', 0xdbcd54): (0xE3A07E2D, '{ extern uint32_t g_ui_size[2]; c->r[7] = g_ui_size[1]; }'),
    # 3D camera projection (sCamera / uCamera -> the perspective matrix at 0x9c617c(near, far, fovy, aspect)): a
    # camera made for the screen (aspect 16:9) gets the picture's shape (config.c camera_aspect, 16:10 or wider; a model
    # preview's camera at 16:10 its box's 16:9 shape) and its field of view from config.c camera_fovy (the FOV factor;
    # Vert+ below 16:9); other cameras keep theirs
    ('main', 0x21c36c): (0xEDD11A0E, '{ extern float camera_aspect(float); c->v.f[3] = camera_aspect(LDF(c->r[1] + 0x38u)); }'),
    ('main', 0x21c370): (0xEE211A02, '{ extern float camera_fovy(float, float); c->v.f[2] = camera_fovy(c->v.f[2] * c->v.f[4], c->v.f[3]); }'),
    ('main', 0x21c3b8): (0xEDD01A0E, '{ extern float camera_aspect(float); c->v.f[3] = camera_aspect(LDF(c->r[0] + 0x38u)); }'),
    ('main', 0x21c3c0): (0xEE221A01, '{ extern float camera_fovy(float, float); c->v.f[2] = camera_fovy(c->v.f[4] * c->v.f[2], c->v.f[3]); }'),
    ('main', 0xaf44a8): (0xEDD11A0E, '{ extern float camera_aspect(float); c->v.f[3] = camera_aspect(LDF(c->r[1] + 0x38u)); }'),
    ('main', 0xaf44ac): (0xEE211A02, '{ extern float camera_fovy(float, float); c->v.f[2] = camera_fovy(c->v.f[2] * c->v.f[4], c->v.f[3]); }'),
    # A monster's stall timers count per update (`vadd.f32 s0, s16, s0` / `vadd.f32 s0, s2, s0`; r4 = the monster):
    # runtime/hle/emstall.c scales what they add by its step
    ('main', 0x2c4058): (0xEE380A00, '{ extern float emstall_scale(uint32_t); c->v.f[0] += c->v.f[16] * emstall_scale(c->r[4]); }'),
    ('main', 0x2c4150): (0xEE310A00, '{ extern float emstall_scale(uint32_t); c->v.f[0] = c->v.f[2] + c->v.f[0] * emstall_scale(c->r[4]); }'),
    # The AI script interpreter's loop, `str r0, [r4]`, the new script position (debug, MHGU_EMTRACE_CMD: runtime/hle/emtrace.c)
    ('main', 0x2855b4): (0xE5840000, '{ extern void emtrace_cmd(Cpu *); ST32(c->r[4], c->r[0]); emtrace_cmd(c); }'),
    # Per-monster network receive (push {r4, r5, fp, lr}): runtime/net/emsync.c may drop a small monster's message
    ('main', 0x2cb120): (0xE92D4830, '{ extern int emsync_filter(Cpu *); if (emsync_filter(c)) { c->pc = c->r[14]; return; } '
                                     'uint32_t ea = c->r[13] - 16u; ST32(ea, c->r[4]); ST32(ea + 4u, c->r[5]); '
                                     'ST32(ea + 8u, c->r[11]); ST32(ea + 12u, c->r[14]); c->r[13] = ea; }'),
    # uScheduler's update (push {r4, lr}): runtime/hle/evskip.c speeds up an event camera pan when A is pressed
    ('main', 0xb66164): (0xE92D4010, '{ extern void evskip_update(Cpu *); evskip_update(c); '
                                     'uint32_t ea = c->r[13] - 8u; ST32(ea, c->r[4]); ST32(ea + 4u, c->r[14]); c->r[13] = ea; }'),
    # Buff icon cycle on the HUD's status plates (runtime/hle/buffcycle.c): the countdown's `sub r7, r7, #1` per update
    # and its reload `mov r0, #20` count in the frame step instead of updates
    ('main', 0x834d20): (0xE2477001, '{ extern uint32_t buffcycle_tick(void); c->r[7] -= buffcycle_tick(); }'),
    ('main', 0x834ef4): (0xE3A00014, '{ extern uint32_t buffcycle_reload(void); c->r[0] = buffcycle_reload(); }'),
    # Two-button input window (runtime/hle/combowin.c): the countdown a press waits for a second button takes 1.0 off
    # per update (`vmov.f32 s2, #-1.0`); it takes the frame step instead
    ('main', 0x47ca84): (0xEEBF1A00, '{ extern float combowin_tick(void); c->v.f[2] = -combowin_tick(); }'),
    # Town NPC fade (uLobbyNpc, 0x8c2d9c): `vldr s8, [r2]` loads the squared distance within which the NPC fades in
    # (table at 0x8c2ef0: 2500^2, or 1800^2 with flag 0x40 of +0x186a); config.c npc_fade_dist2 scales it ([Graphics]
    # DrawDistance)
    ('main', 0x8c2e34): (0xED924A00, '{ extern float npc_fade_dist2(float); c->v.f[8] = npc_fade_dist2(LDF(c->r[2])); }'),
    # ... and its speech bubble (0x8c11f8: the same check against its own copy of the table, 0x8c1328, then the bubble
    # above the head, 0x938530)
    ('main', 0x8c127c): (0xED924A00, '{ extern float npc_fade_dist2(float); c->v.f[8] = npc_fade_dist2(LDF(c->r[2])); }'),
    # Quest photo request (sScreenShot, 0xa6f97c: `mov r1, #1` before the store to +0x2c that asks for the capture): the
    # renderer doesn't skip draws for building shaders while the one-off capture is drawn (exec.c, PHOTO_FRAMES)
    ('main', 0xa6f9ac): (0xE3A01001, '{ extern void backend_photo_requested(void); backend_photo_requested(); c->r[1] = 1; }'),
}

# Monster jump physics (runtime/hle/emjump.c): the stores of `pos += vel; vel += acc; acc.y += -0.2`, done once per
# update without the frame step, advance by the step instead. (address, kind, axis, jerk): the store `vstr sN,
# [r0, #off]` of that status block field. 0x1122b4c / 0x1122b9c: Arzuros / Lagombi / Volvidon hops (vertical only);
# 0x1122e48 / 0x1122ee4: Volvidon's body press, rising / falling; 0x10f6eb4: Zinogre (no jerk).
EMJUMP = [
    (0x1122b4c, 'pos', 1, -0.2), (0x1122b60, 'vel', 1, -0.2), (0x1122b74, 'acc', 1, -0.2),
    (0x1122b9c, 'pos', 1, -0.2), (0x1122bb0, 'vel', 1, -0.2), (0x1122bc8, 'acc', 1, -0.2),
    (0x1122e48, 'pos', 0, 0), (0x1122e5c, 'pos', 1, -0.2), (0x1122e70, 'pos', 2, 0),
    (0x1122e84, 'vel', 0, 0), (0x1122e98, 'vel', 1, -0.2), (0x1122eac, 'vel', 2, 0), (0x1122ec0, 'acc', 1, -0.2),
    (0x1122ee4, 'pos', 0, 0), (0x1122ef8, 'pos', 1, -0.2), (0x1122f0c, 'pos', 2, 0),
    (0x1122f20, 'vel', 0, 0), (0x1122f34, 'vel', 1, -0.2), (0x1122f48, 'vel', 2, 0), (0x1122f5c, 'acc', 1, -0.2),
    (0x10f6eb4, 'pos', 0, 0), (0x10f6ec4, 'pos', 1, 0), (0x10f6ed4, 'pos', 2, 0),
    (0x10f6ee8, 'vel', 0, 0), (0x10f6ef8, 'vel', 1, 0), (0x10f6f08, 'vel', 2, 0),
]
for _a, _kind, _axis, _jerk in EMJUMP:
    _off = {'pos': 0x40, 'vel': 0x10, 'acc': 0x20}[_kind] + 4 * _axis  # (vstr's offset / 4 in the low byte)
    _reg = 1 if _a == 0x1122bc8 else 0  # vstr s2 there, s0 elsewhere
    _word = 0xED800A00 | (_reg << 12) | (_off // 4)
    PATCHES[('main', _a)] = (_word, f'{{ extern void emjump_{_kind}(Cpu *, int, float); emjump_{_kind}(c, {_axis}, {float(_jerk)}f); }}')


class Module:
    def __init__(self, name, path, base):
        self.name = name
        self.nso = Nso(path)
        self.base = base
        n = self.nso
        self.lo = base + n.text[0]
        self.hi = base + n.text[0] + n.text[1]
        self.words = struct.unpack(f'<{n.text[1] // 4}I', bytes(n.image[n.text[0]:n.text[0] + (n.text[1] & ~3)]))
        self.lifter = Lifter(n.image, base, self.lo, self.hi)
        self.cache = {}
        self.stats = Counter()
        self.unsupported = Counter()
        self.undetected = []  # add pc, rX, rY jumps that weren't understood as tables (written to undetected_tables.txt)
        self.data = set()  # text addresses known to hold data (literal pools, jump tables)

    def in_text(self, a):
        return self.lo <= a < self.hi and a % 4 == 0

    def word(self, a):
        return self.words[(a - self.lo) // 4]

    def lift(self, a):
        ins = self.cache.get(a)
        if ins is None:
            L = self.lifter
            L.prev_mov_lr_pc = a - 4 >= self.lo and self.word(a - 4) == MOV_LR_PC
            L.lits = []
            w = self.word(a)
            try:
                ins = L.lift(w, a)
            except Unsupported as e:
                self.stats['unsupported'] += 1
                key = re.sub(r' [0-9a-f]{8}$', '', str(e))
                self.unsupported[key] += 1
                ins = Insn('trap', f'rt_udf(c, {hx(a)}); /* {e} */')
                ins.cond = None
            patch = PATCHES.get((self.name, a))
            if patch:
                if w != patch[0] or ins.kind != 'op' or ins.cond:
                    sys.exit(f'{self.name} {a:x}: patch expects {patch[0]:08x}, found {w:08x} (another game version?)')
                ins.code = patch[1]
            lits = self.lit_refs(w, a)
            ins.reads_pc_table = lits
            self.cache[a] = ins
        return ins

    def lit_refs(self, w, a):
        """Text addresses an instruction reads as data (pc-relative loads)."""
        out = []
        if bits(w, 28, 4) == 15:
            return out
        if bits(w, 25, 3) == 2 and bits(w, 16, 4) == 15 and bit(w, 20) and bit(w, 24) and not bit(w, 21):
            imm = bits(w, 0, 12)
            out.append((a + 8 + (imm if bit(w, 23) else -imm)) & ~3)
        elif bits(w, 25, 3) == 6 and bits(w, 9, 3) == 5 and bits(w, 16, 4) == 15 and bit(w, 24) and not bit(w, 21):
            off = bits(w, 0, 8) * 4
            t = ((a + 8) & ~3) + (off if bit(w, 23) else -off)
            out.append(t)
            if bit(w, 8):
                out.append(t + 4)
        elif (bits(w, 25, 3) == 0 and bit(w, 22) and bit(w, 7) and bit(w, 4) and bits(w, 16, 4) == 15
              and bit(w, 24) and not bit(w, 21) and bits(w, 5, 2)):
            imm = (bits(w, 8, 4) << 4) | bits(w, 0, 4)
            t = (a + 8 + (imm if bit(w, 23) else -imm)) & ~3
            out.append(t)
            if bits(w, 5, 2) == 2 and not bit(w, 20):  # LDRD literal
                out.append(t + 4)
        return [x for x in out if self.in_text(x)]

    # ------------------------------------------------------------ function starts
    def exidx_functions(self):
        n = self.nso
        out = []
        for off in range(n.eh_start, n.eh_end, 8):
            v = n.u32(off) & 0x7FFFFFFF
            if v & 0x40000000:
                v -= 0x80000000
            out.append(self.base + off + v)
        return out

    def boundaries(self):
        n, b = self.nso, self.base
        starts = set(a for a in self.exidx_functions() if self.in_text(a))
        for name, info, shndx, value, size in n.syms:
            if shndx and (info & 0xF) == STT_FUNC:
                starts.add(b + (value & ~1))
        for off, typ, sym, add in n.relocs:
            if typ == R_ARM_RELATIVE and self.in_text(b + add):
                starts.add(b + add)
        starts.add(self.lo)  # module entry (crt0)
        return sorted(a for a in starts if self.in_text(a))

    # ------------------------------------------------------------ jump tables
    def jump_table(self, a, entry, end):
        """`add rB, pc, #imm; ldr rT, [rI, rB]; add pc, rT, rB` (clang PIC tables of offsets from the table
        base), or `add pc, pc, rI, lsl #2` into a table of branches. Returns (table_start, [targets]) or None."""
        w = self.word(a)
        if (w & 0x0FE0FFF0) == 0x008FF100 and bits(w, 28, 4) != 15:  # add pc, pc, rI, lsl #2
            n = self.table_bound(a, bits(w, 0, 4), entry)
            if not n:
                return None
            return a + 8, [a + 8 + 4 * i for i in range(n)], 0
        if (w & 0x0FE0FFF0) != 0x0080F000 or a - 8 < entry:  # add pc, rX, rY (no shift)
            return None
        rx, ry = bits(w, 16, 4), bits(w, 0, 4)
        # back from the add pc: `ldr rT, [rX, rB]` then `add rB, pc, #imm`, with unrelated instructions allowed in
        # between as long as they don't write rB / rT (the scheduler moves loads of other values there)
        ld_at = adr_at = None
        for k in range(1, 33):  # (the scheduler puts up to ~20 constant loads between the adr and the jump)
            p = a - 4 * k
            if p < entry:
                return None
            x = self.word(p)
            if ld_at is None:
                if (x & 0x0FF00FF0) == 0x07900000 and bits(x, 12, 4) in (rx, ry):
                    ld_at = p
                continue
            if (x & 0x0FFF0000) == 0x028F0000:
                adr_at = p
                break
        if ld_at is None or adr_at is None:
            return None
        ld, adr = self.word(ld_at), self.word(adr_at)
        rb = bits(adr, 12, 4)
        rt = bits(ld, 12, 4)
        if rb not in (rx, ry) or rt not in (rx, ry) or rb == rt:
            return None
        if rb not in (bits(ld, 16, 4), bits(ld, 0, 4)):
            return None
        def writes(x, r):
            if bits(x, 26, 2) == 1:  # load / store: only loads write Rt (and writeback writes Rn)
                return (bit(x, 20) and bits(x, 12, 4) == r) or ((bit(x, 21) or not bit(x, 24)) and bits(x, 16, 4) == r)
            if bits(x, 26, 2) == 0:  # data processing (compares write nothing)
                return bits(x, 12, 4) == r and not (bits(x, 23, 2) == 2 and bit(x, 20))
            if bits(x, 26, 2) == 3 and bits(x, 28, 4) != 15:  # coprocessor / VFP (the scheduler puts vmov here)
                if bits(x, 24, 4) == 0xE:  # vmov rT, sN / vmrs rT (to a core register: bit 20) or a VFP data op
                    return bool(bit(x, 4) and bit(x, 20)) and bits(x, 12, 4) == r
                if bits(x, 21, 7) == 0b1100010:  # vmov rT, rT2, dN (bit 20: to core registers)
                    return bool(bit(x, 20)) and r in (bits(x, 12, 4), bits(x, 16, 4))
                if bits(x, 25, 3) == 6:  # vldr / vstr / vldm / vstm: only a writeback writes Rn
                    return bool(bit(x, 21)) and bits(x, 16, 4) == r
            return True  # anything else: be conservative
        for p in range(adr_at + 4, a, 4):  # nothing in between may write rB, nor rT after the load
            if p == ld_at:
                continue
            x = self.word(p)
            if writes(x, rb) or (p > ld_at and writes(x, rt)):
                return None
        rot = bits(adr, 8, 4) * 2
        imm = bits(adr, 0, 8)
        imm = ((imm >> rot) | (imm << (32 - rot))) & 0xFFFFFFFF if rot else imm
        tbase = adr_at + 8 + imm
        n = self.table_bound(adr_at, None, entry)
        bounded = self.table_targets(tbase, n, entry, end) if n else None
        # heuristic: valid entries up to the first forward target (the table ends where code after it starts;
        # backward targets say nothing about its length)
        guessed, lowest, i = [], end, 0
        while tbase + 4 * i < lowest and tbase + 4 * i < end:
            t = (tbase + self.word(tbase + 4 * i)) & 0xFFFFFFFF
            if not (entry <= t < end) or t % 4 or (tbase <= t < tbase + 4 * (i + 1)):
                break
            guessed.append(t)
            if t > tbase:
                lowest = min(lowest, t)
            i += 1
        # the longer of the two: an entry too many is only an extra label, one too few a crash
        targets = bounded if bounded and len(bounded) >= len(guessed) else guessed
        n = len(targets)
        if not n:
            return None
        for i in range(n):
            self.data.add(tbase + 4 * i)
        return tbase, targets, n

    def table_targets(self, tbase, n, entry, end):
        """Targets of the first n entries; a table ends early at an entry that is no target of this function (the
        bound can over-count: `cmp rI, #15 / beq default` leaves 15 entries, not 16)."""
        out = []
        for i in range(n):
            if tbase + 4 * i >= self.hi:
                break
            t = (tbase + self.word(tbase + 4 * i)) & 0xFFFFFFFF
            if not (entry <= t < end) or t % 4 or tbase <= t < tbase + 4 * n:
                break
            out.append(t)
        return out or None

    def table_bound(self, a, reg, entry):
        """Number of table entries from the code before a: a bounds check (`cmp rX, #N` + `bhi`/`bls`: N + 1) or
        a mask of the index (`and rX, rY, #2^k - 1`: 2^k); the smaller when both are there."""
        bound = None
        for k in range(1, 16):
            p = a - 4 * k
            if p < entry:
                break
            w = self.word(p)
            rot = bits(w, 8, 4) * 2
            imm = bits(w, 0, 8)
            imm = ((imm >> rot) | (imm << (32 - rot))) & 0xFFFFFFFF if rot else imm
            if (w & 0x0FF0F000) == 0x03500000:  # cmp rX, #imm
                if reg is not None and bits(w, 16, 4) != reg:
                    continue
                if imm < 4096:
                    bound = imm + 1 if bound is None else min(bound, imm + 1)
                break
            if (w & 0x0FE00000) == 0x02000000 and imm and (imm & (imm + 1)) == 0 and imm < 4096:  # and rX, rY, #2^k-1
                bound = imm + 1 if bound is None else min(bound, imm + 1)
            if bits(w, 25, 3) == 5 and bits(w, 28, 4) == 14:  # an unconditional branch: not this block any more
                break
        return bound

    def computed_local(self, a, entry):
        """True if the register an indirect jump at a goes to was computed by address arithmetic (a computed goto
        inside the function, e.g. compiler-rt's unrolled division) rather than loaded (a tail call)."""
        w = self.word(a)
        if (w & 0x0FFFFFF0) == 0x012FFF10:  # bx rm
            reg = bits(w, 0, 4)
        elif (w & 0x0FEF0FF0) == 0x01A0F000:  # mov pc, rm
            reg = bits(w, 0, 4)
        elif bits(w, 26, 2) == 0 and bits(w, 12, 4) == 15 and bits(w, 21, 4) in (2, 4):  # add/sub pc, ...
            return bits(w, 16, 4) == 15 or self.computed_reg(a, entry, bits(w, 16, 4))
        else:
            return False
        if reg == 14:
            return False
        return self.computed_reg(a, entry, reg)

    def computed_reg(self, a, entry, reg):
        for k in range(1, 12):
            p = a - 4 * k
            if p < entry:
                return False
            x = self.word(p)
            if bits(x, 28, 4) == 15:
                continue
            if bits(x, 26, 2) == 0 and not (bit(x, 25) == 0 and bit(x, 4) and bit(x, 7)) and bits(x, 12, 4) == reg:
                op = bits(x, 21, 4)
                if op in (2, 4) and (bits(x, 16, 4) == 15 or (not bit(x, 25) and bits(x, 0, 4) == 15)):
                    return True  # add/sub reg, pc, ...
                if op in (2, 4):
                    return self.computed_reg(p, entry, bits(x, 16, 4))  # follow the base: sub ip, ip, r3 lsl #3
                return False
            if bits(x, 26, 2) == 1 and bit(x, 20) and bits(x, 12, 4) == reg:
                return False  # loaded: a pointer
            if bits(x, 25, 3) == 5:
                return False  # crossed a branch
        return False

    # ------------------------------------------------------------ scanning
    def scan(self, entry, end):
        """Recursive descent over [entry, end). Returns (code addresses, labels, external targets, tables)."""
        code, labels, ext, tables = set(), {entry}, set(), {}
        work = [entry]
        while work:
            a = work.pop()
            while entry <= a < end and a not in code and a not in self.data:
                code.add(a)
                ins = self.lift(a)
                for d in ins.reads_pc_table or ():
                    self.data.add(d)
                k = ins.kind
                cond = ins.cond is not None
                if k == 'b':
                    t = ins.target
                    if entry <= t < end:
                        labels.add(t)
                        work.append(t)
                    elif self.in_text(t):
                        ext.add(t)
                    if not cond:
                        break
                elif k == 'bl':
                    if self.in_text(ins.target):
                        ext.add(ins.target)
                elif k == 'br':
                    jt = self.jump_table(a, entry, end)
                    if jt:
                        tables[a] = jt
                        for t in jt[1]:
                            labels.add(t)
                            work.append(t)
                        self.stats['jumptables'] += 1
                    elif self.computed_local(a, entry):
                        # computed goto: may land on any instruction of the function, so every word of it is
                        # lifted and labelled (the decoded garbage among them is never reached)
                        tables[a] = 'all'
                        self.stats['computed_goto'] += 1
                        for t in range(entry, end, 4):
                            if t not in self.data:
                                labels.add(t)
                                work.append(t)
                    else:
                        self.stats['br_dispatch'] += 1
                        if (self.word(a) & 0x0FE0F000) == 0x0080F000:  # add pc, rX, rY: a table not understood
                            self.stats['br_addpc'] += 1
                            self.undetected.append(a)
                    if not cond:
                        break
                elif k in ('ret', 'trap'):
                    if not cond:
                        break
                a += 4
            else:
                if a >= end and end < self.hi and a == end:
                    ext.add(end)  # falls through into the next function
        return code, labels, ext, tables

    # ------------------------------------------------------------ emission
    def emit_function(self, entry, end, code, labels, tables, hle, refs):
        out = [f'void {fname(entry)}(Cpu *restrict c) {{']
        if entry in hle:
            out.append(f'    {sanitize(hle[entry])}(c);')
            out.append('}')
            return out
        out.append(f'    FN_ENTER({hx(entry)});')

        def jump(t):
            if entry <= t < end:
                if t in code:
                    return f'goto L_{t:x};'
                return f'rt_udf(c, {hx(t)});'  # (fell into data: after a call that does not return)
            if t not in self.entry_set:
                return f'c->pc = {hx(t)}; __attribute__((musttail)) return rt_dispatch(c);'
            refs.add(t)
            return f'__attribute__((musttail)) return {fname(t)}(c);'

        order = sorted(code)
        self.needs_dispatch = False
        prev = None
        for a in order:
            if prev is not None and a != prev + 4:
                # a gap (data, unreachable): the previous instruction ended its block, or fell into data
                if not self.ends_block(prev):
                    out.append('    ' + jump(prev + 4))
            if a in labels:
                out.append(f'L_{a:x}:;')
            ins = self.lift(a)
            out.append('    ' + self.render(ins, a, jump, refs, tables))
            prev = a
        if prev is not None and not self.ends_block(prev):
            out.append('    ' + (jump(prev + 4) if prev + 4 < self.hi else f'rt_udf(c, {hx(prev + 4)});'))
        if self.needs_dispatch:  # computed goto: any labelled instruction of this function
            cases = ' '.join(f'case {hx(t)}: goto L_{t:x};' for t in order if t in labels)
            out.append('L_dispatch:')
            out.append(f'    switch (c->pc) {{ {cases} default: break; }}')
            out.append('    __attribute__((musttail)) return rt_dispatch(c);')
        out.append('}')
        return out

    def ends_block(self, a):
        ins = self.lift(a)
        return ins.cond is None and ins.kind in ('b', 'br', 'ret', 'trap')

    def render(self, ins, a, jump, refs, tables):
        k, cond = ins.kind, ins.cond
        if k == 'op':
            body = ins.code
        elif k == 'trap':
            body = ins.code
        elif k == 'b':
            body = jump(ins.target)
        elif k == 'bl':
            t = ins.target
            if t in self.entry_set:
                refs.add(t)
                body = f'c->r[14] = {hx(a + 4)}; {fname(t)}(c);'
            else:
                body = f'c->r[14] = {hx(a + 4)}; c->pc = {hx(t)}; rt_dispatch(c);'
        elif k == 'blr':
            body = f'{ins.code} c->r[14] = {hx(a + 4)}; rt_dispatch(c);'
        elif k == 'ret':
            body = f'{ins.code} return;' if ins.code else 'return;'
        elif k == 'br':
            jt = tables.get(a)
            if jt == 'all':
                body = f'{ins.code} goto L_dispatch;'
                self.needs_dispatch = True
            elif jt:
                tbase, targets, _ = jt
                cases = ' '.join(f'case {hx(t)}: goto L_{t:x};' for t in sorted(set(targets)))
                body = f'{ins.code} switch (c->pc) {{ {cases} default: break; }} __attribute__((musttail)) return rt_dispatch(c);'
            else:
                body = f'{ins.code} __attribute__((musttail)) return rt_dispatch(c);'
        else:
            raise Unsupported(k)
        if cond:
            return f'if ({cond}) {{ {body} }}'
        return body


def write_if_changed(path, text):
    """Leaves unchanged chunks untouched so make only recompiles what changed."""
    try:
        with open(path, newline='') as f:
            if f.read() == text:
                return
    except FileNotFoundError:
        pass
    with open(path, 'w', newline='\n') as f:
        f.write(text)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('name')
    ap.add_argument('nso')
    ap.add_argument('base', type=lambda s: int(s, 0))
    ap.add_argument('outdir')
    ap.add_argument('--hle')
    ap.add_argument('--chunk', type=int, default=1500)
    ap.add_argument('--chunk-bytes', type=int, default=1200000)
    ap.add_argument('--big-fn', type=int, default=100000)
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--stats', action='store_true')
    args = ap.parse_args()

    t0 = time.time()
    m = Module(args.name, args.nso, args.base)
    starts = m.boundaries()
    hle = {}
    if args.hle and os.path.exists(args.hle):
        want = {l.strip() for l in open(args.hle) if l.strip() and not l.startswith('#')}
        for name, info, shndx, value, size in m.nso.syms:
            if shndx and name in want:
                hle[args.base + value] = name

    start_set = set(starts)
    scanned = {}
    rounds = 0
    while True:
        rounds += 1
        order = sorted(start_set)
        new = set()
        for i, a in enumerate(order):
            end = order[i + 1] if i + 1 < len(order) else m.hi
            key = (a, end)
            if key not in scanned:
                scanned[key] = m.scan(a, end)
            new |= scanned[key][2] - start_set
        new = {a for a in new if a not in m.data}
        if not new:
            break
        start_set |= new
        print(f'{args.name}: round {rounds}: {len(start_set)} functions (+{len(new)})', file=sys.stderr, flush=True)
    entries = {}
    for i, a in enumerate(order):
        end = order[i + 1] if i + 1 < len(order) else m.hi
        code, labels, ext, tables = scanned[(a, end)]
        entries[a] = (end, code, labels, tables)
    ninsn = sum(len(e[1]) for e in entries.values())
    print(f'{args.name}: {len(starts)} boundaries, {len(entries)} functions, {ninsn} instructions, {rounds} rounds, '
          f'{dict(m.stats)}, {time.time() - t0:.0f}s', file=sys.stderr)
    if args.stats or m.unsupported:
        for k, v in m.unsupported.most_common(60 if args.stats else 15):
            print(f'  unsupported {v:7d}  {k}', file=sys.stderr)

    os.makedirs(args.outdir, exist_ok=True)
    with open(os.path.join(args.outdir, 'undetected_tables.txt'), 'w') as f:
        for a in sorted(set(m.undetected)):
            f.write(f'{a:08x}\n')
    order = sorted(entries)
    m.entry_set = set(order)
    if args.limit:
        order = order[:args.limit]
    nchunks = 0
    ci = 0
    pending = None
    while ci < len(order) or pending:
        body, refs, size, fns = [], set(), 0, []
        while (pending or ci < len(order)) and len(fns) < args.chunk and size < args.chunk_bytes:
            if pending:
                a, lines, r = pending
                pending = None
            else:
                a = order[ci]
                ci += 1
                end, code, labels, tables = entries[a]
                r = set()
                lines = m.emit_function(a, end, code, labels, tables, hle, r)
            n = sum(len(l) + 1 for l in lines)
            if n > args.big_fn and fns:
                pending = (a, lines, r)
                break
            body += lines
            size += n
            fns.append(a)
            refs |= r
            if n > args.big_fn:
                break
        path = os.path.join(args.outdir, f'{args.name}_{nchunks:03d}.c')
        text = ['#include "cpu.h"\n#include "neon_rt.h"\n']
        text += [f'void {fname(r)}(Cpu *c);\n' for r in sorted(refs)]
        text += [f'void {sanitize(hle[a])}(Cpu *c);\n' for a in sorted(set(hle) & set(fns))]
        text += ['\n'.join(body), '\n']
        write_if_changed(path, ''.join(text))
        nchunks += 1
        print(f'{args.name}: progress {ci}/{len(order)} files {nchunks}', file=sys.stderr, flush=True)
    for fn in os.listdir(args.outdir):
        mm = re.fullmatch(re.escape(args.name) + r'_(\d{3})\.c', fn)
        if mm and int(mm.group(1)) >= nchunks:
            os.remove(os.path.join(args.outdir, fn))
    import io
    with io.StringIO() as f:
        f.write('#include "cpu.h"\n')
        for a in order:
            f.write(f'void {fname(a)}(Cpu *c);\n')
        f.write(f'const uint32_t {args.name}_fn_addrs[] = {{\n')
        f.write(',\n'.join(hx(a) for a in order))
        f.write('};\n')
        f.write(f'const GuestFn {args.name}_fn_ptrs[] = {{\n')
        f.write(',\n'.join(fname(a) for a in order))
        f.write('};\n')
        f.write(f'const uint32_t {args.name}_fn_count = {len(order)};\n')
        write_if_changed(os.path.join(args.outdir, f'{args.name}_table.c'), f.getvalue())
    print(f'{args.name}: wrote {nchunks} chunks, {time.time() - t0:.0f}s total', file=sys.stderr)


if __name__ == '__main__':
    main()
