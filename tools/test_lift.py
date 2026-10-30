"""Differential test of the lifter: instructions sampled from a module's reachable code are run on Unicorn (the
reference) and as lifted C (tools/test_harness.c) from the same random register / memory state, and every
register, flag, VFP register and the scratch memory are compared.

Usage: test_lift.py <nso> <base> [--tests N] [--vectors K] [--seed S] [--only REGEX] [--interp]
--interp runs the same instructions through the interpreter (runtime/interp.c) instead.
Needs clang on PATH (MSYS2 UCRT64). Exit status 0 when everything matches.
"""
import argparse
import os
import platform
import random
import re
import struct
import subprocess
import sys

import capstone
import unicorn as uc
from unicorn import arm_const as A

from gen import Module

DATA = 0x40000000
DATA_SIZE = 0x2000
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def pattern():
    out = bytearray(DATA_SIZE)
    x = 12345
    for i in range(DATA_SIZE):
        x = (x * 1103515245 + 12345) & 0xFFFFFFFF
        out[i] = (x >> 16) & 0xFF
    return bytes(out)


PATTERN = pattern()
NICE_F = [0.0, -0.0, 1.0, -1.0, 0.5, 2.0, 3.25, -7.75, 100.0, 1e-3, 65536.5, 1e9, -1e9, 2147483648.0,
          4294967296.0, float('inf'), float('-inf'), 1e-40]


def rand_float(rng):
    r = rng.random()
    if r < 0.3:
        return rng.choice(NICE_F)
    if r < 0.8:
        return rng.uniform(-1000, 1000)
    return rng.uniform(-1e6, 1e6)


def f32bits(f):
    return struct.unpack('<I', struct.pack('<f', f))[0]


def f64bits(f):
    return struct.unpack('<Q', struct.pack('<d', f))[0]


def rand_state(rng, mem_insn):
    regs = []
    for i in range(15):
        if mem_insn and rng.random() < 0.6:
            regs.append((DATA + 0x1000 + rng.randrange(-0x400, 0x400)) & ~3)
        elif mem_insn:
            regs.append(rng.randrange(0, 0x100) & ~3)
        else:
            r = rng.random()
            if r < 0.25:
                regs.append(rng.randrange(0, 64))
            elif r < 0.35:
                regs.append(rng.choice([0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xFFFF, 0x10000]))
            else:
                regs.append(rng.getrandbits(32))
    flags = rng.getrandbits(4)  # NZCV
    q = rng.getrandbits(1)
    ge = rng.getrandbits(4)
    fpscr = rng.getrandbits(4) << 28
    d = []
    for i in range(32):
        r = rng.random()
        if r < 0.5:
            d.append(f32bits(rand_float(rng)) | (f32bits(rand_float(rng)) << 32))
        elif r < 0.85:
            d.append(f64bits(rand_float(rng)))
        else:
            d.append(rng.getrandbits(64))
    return regs, flags, q, ge, fpscr, d


MEM_MN = re.compile(r'^(ldr|str|ldm|stm|push|pop|vldr|vstr|vldm|vstm|vpush|vpop|vld|vst|swp|lda|stl|ldrex|strex|pld)')


class Runner:
    def __init__(self, m):
        self.m = m
        mu = uc.Uc(uc.UC_ARCH_ARM, uc.UC_MODE_ARM)
        try:
            mu.ctl_set_cpu_model(uc.arm_const.UC_CPU_ARM_MAX)
        except Exception:
            pass
        size = (len(m.nso.image) + 0xFFFF) & ~0xFFFF
        mu.mem_map(m.base, size)
        mu.mem_write(m.base, bytes(m.nso.image))
        mu.mem_map(DATA, DATA_SIZE)
        mu.hook_add(uc.UC_HOOK_MEM_UNMAPPED, self.on_unmapped)
        self.writes_outside = False
        mu.hook_add(uc.UC_HOOK_MEM_WRITE, self.on_write)
        # VFP on: CPACR cp10/cp11 full access, FPEXC.EN
        mu.reg_write(A.UC_ARM_REG_C1_C0_2, mu.reg_read(A.UC_ARM_REG_C1_C0_2) | (0xF << 20))
        mu.reg_write(A.UC_ARM_REG_FPEXC, 0x40000000)
        self.mu = mu
        self.lazy = set()

    def on_unmapped(self, mu, access, addr, size, value, data):
        page = addr & ~0xFFF
        for p in (page, (addr + size - 1) & ~0xFFF):
            if p not in self.lazy:
                mu.mem_map(p, 0x1000)
                self.lazy.add(p)
        return True

    def on_write(self, mu, access, addr, size, value, data):
        if not (DATA <= addr and addr + size <= DATA + DATA_SIZE):
            self.writes_outside = True

    def run(self, addr, state):
        mu = self.mu
        regs, flags, q, ge, fpscr, d = state
        for p in self.lazy:
            mu.mem_unmap(p, 0x1000)
        self.lazy.clear()
        self.writes_outside = False
        mu.mem_write(DATA, PATTERN)
        mu.reg_write(A.UC_ARM_REG_CPSR, 0x10)  # user mode first: SP / LR are banked per mode
        for i in range(13):
            mu.reg_write(A.UC_ARM_REG_R0 + i, regs[i])
        mu.reg_write(A.UC_ARM_REG_SP, regs[13])
        mu.reg_write(A.UC_ARM_REG_LR, regs[14])
        mu.reg_write(A.UC_ARM_REG_CPSR, (flags << 28) | (q << 27) | (ge << 16) | 0x10)
        mu.reg_write(A.UC_ARM_REG_FPSCR, fpscr)
        for i in range(32):
            mu.reg_write(A.UC_ARM_REG_D0 + i, d[i])
        mu.emu_start(addr, addr + 4, count=1)
        out_regs = [mu.reg_read(A.UC_ARM_REG_R0 + i) for i in range(13)]
        out_regs += [mu.reg_read(A.UC_ARM_REG_SP), mu.reg_read(A.UC_ARM_REG_LR)]
        cpsr = mu.reg_read(A.UC_ARM_REG_CPSR)
        fps = mu.reg_read(A.UC_ARM_REG_FPSCR)
        dd = [mu.reg_read(A.UC_ARM_REG_D0 + i) for i in range(32)]
        mem = bytes(mu.mem_read(DATA, DATA_SIZE))
        return out_regs, cpsr, fps, dd, mem


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('nso')
    ap.add_argument('base', type=lambda s: int(s, 0))
    ap.add_argument('--tests', type=int, default=3000)
    ap.add_argument('--vectors', type=int, default=4)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--functions', type=int, default=4000)
    ap.add_argument('--only')
    ap.add_argument('--keep', action='store_true')
    ap.add_argument('--interp', action='store_true', help='test runtime/interp.c instead of the lifted C')
    args = ap.parse_args()
    rng = random.Random(args.seed)

    m = Module('t', args.nso, args.base)
    starts = m.boundaries()
    rng.shuffle(starts)
    cs = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM | capstone.CS_MODE_V8)
    order = sorted(starts)
    nxt = {a: (order[i + 1] if i + 1 < len(order) else m.hi) for i, a in enumerate(order)}
    picked, seen_words = [], set()
    for a in starts[:args.functions]:
        code, labels, ext, tables = m.scan(a, nxt[a])
        for x in sorted(code):
            w = m.word(x)
            ins = m.lift(x)
            if ins.kind != 'op' or w in seen_words:
                continue
            if re.search(r'rt_(svc|ticks|udf|bkpt|thumb)|tpidr|19200000u|ldrex|strex', ins.code):
                continue
            dis = next(cs.disasm(struct.pack('<I', w), x), None)
            if dis is None:
                continue
            if args.only and not re.search(args.only, dis.mnemonic):
                continue
            seen_words.add(w)
            picked.append((x, w, ins, dis))
    rng.shuffle(picked)
    picked = picked[:args.tests]
    print(f'{len(picked)} instructions, {args.vectors} vectors each', file=sys.stderr)

    run = Runner(m)
    work = os.path.join(os.environ.get('TEMP', '/tmp'), 'mhgu_test_lift')
    os.makedirs(work, exist_ok=True)
    src = ['#include "cpu.h"\n#include "neon_rt.h"\n#include "interp.h"\n']
    vecs, expect, meta = [], [], []
    for ti, (x, w, ins, dis) in enumerate(picked):
        body = ins.code if not ins.cond else f'if ({ins.cond}) {{ {ins.code} }}'
        if args.interp:
            body = f'int l; interp_exec(c, 0x{x:x}u, 0x{w:x}u, &l);'
        src.append(f'static void t{ti}(Cpu *restrict c) {{ {body} }}\n')
        mem_insn = bool(MEM_MN.match(dis.mnemonic))
        for k in range(args.vectors):
            st = rand_state(rng, mem_insn)
            try:
                res = run.run(x, st)
            except uc.UcError as e:
                continue
            if run.writes_outside:
                continue
            vecs.append((ti, st))
            expect.append(res)
            meta.append((x, w, dis))
    src.append('typedef void (*T)(Cpu *);\nT tests[] = {' + ', '.join(f't{i}' for i in range(len(picked))) + '};\n')
    src.append(f'int ntests = {len(picked)};\n')
    open(os.path.join(work, 'tests.c'), 'w').write(''.join(src))
    with open(os.path.join(work, 'vectors.bin'), 'wb') as f:
        f.write(struct.pack('<II', len(vecs), m.base))
        for ti, (regs, flags, q, ge, fpscr, d) in vecs:
            f.write(struct.pack('<I15I4I', ti, *regs, flags, q, ge, fpscr))
            f.write(struct.pack('<32Q', *d))
    with open(os.path.join(work, 'image.bin'), 'wb') as f:
        f.write(bytes(m.nso.image))
    with open(os.path.join(work, 'pattern.bin'), 'wb') as f:
        f.write(PATTERN)
    exe = os.path.join(work, 'harness.exe')
    # the game code's target (Makefile GENARCH): Apple Silicon or x86-64-v3
    arch = '-mcpu=apple-m1' if platform.machine() == 'arm64' and sys.platform == 'darwin' else '-march=x86-64-v3'
    cmd = ['clang', '-O1', '-fno-strict-aliasing', '-ffp-contract=off', arch, '-w',
           f'-I{os.path.join(ROOT, "runtime")}', '-o', exe, os.path.join(HERE, 'test_harness.c'),
           os.path.join(work, 'tests.c'), os.path.join(ROOT, 'runtime', 'simd_int.c'),
           os.path.join(ROOT, 'runtime', 'neon.c')] + ([os.path.join(ROOT, 'runtime', 'interp.c')] if args.interp else [])
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        print(r.stderr[:6000], file=sys.stderr)
        sys.exit(2)
    out = subprocess.run([exe, work], capture_output=True)
    if out.returncode:
        print('harness failed', out.returncode, out.stderr.decode()[:2000], file=sys.stderr)
        sys.exit(2)
    data = out.stdout
    rec = 15 * 4 + 4 * 4 + 32 * 8 + 4
    fails = {}
    nan_diffs = 0
    for i, (res, (x, w, dis)) in enumerate(zip(expect, meta)):
        o = data[i * rec:(i + 1) * rec]
        regs = struct.unpack_from('<15I', o, 0)
        nzcv, q, ge, fpscr = struct.unpack_from('<4I', o, 60)
        dd = struct.unpack_from('<32Q', o, 76)
        memh = struct.unpack_from('<I', o, 76 + 256)[0]
        eregs, cpsr, efps, edd, emem = res
        problems = []
        for r_ in range(15):
            if regs[r_] != eregs[r_]:
                problems.append(f'r{r_} {regs[r_]:08x} != {eregs[r_]:08x}')
        if nzcv != cpsr >> 28:
            problems.append(f'nzcv {nzcv:x} != {cpsr >> 28:x}')
        if q != (cpsr >> 27) & 1:
            problems.append(f'q {q} != {(cpsr >> 27) & 1}')
        if ge != (cpsr >> 16) & 15:
            problems.append(f'ge {ge:x} != {(cpsr >> 16) & 15:x}')
        if (fpscr >> 28) != (efps >> 28):
            problems.append(f'fpscr nzcv {fpscr >> 28:x} != {efps >> 28:x}')
        for r_ in range(32):
            if dd[r_] != edd[r_]:
                a, b = dd[r_], edd[r_]
                # NaN payload / sign differences (x86 vs ARM default NaN) are reported separately
                def isnan32(v): return (v & 0x7F800000) == 0x7F800000 and (v & 0x7FFFFF)
                def isnan64(v): return (v & 0x7FF0000000000000) == 0x7FF0000000000000 and (v & 0xFFFFFFFFFFFFF)
                lo_ok = (a & 0xFFFFFFFF) == (b & 0xFFFFFFFF) or (isnan32(a & 0xFFFFFFFF) and isnan32(b & 0xFFFFFFFF))
                hi_ok = (a >> 32) == (b >> 32) or (isnan32(a >> 32) and isnan32(b >> 32))
                if (lo_ok and hi_ok) or (isnan64(a) and isnan64(b)):
                    nan_diffs += 1
                    continue
                problems.append(f'd{r_} {a:016x} != {b:016x}')
        h = 2166136261
        for byte in emem:
            h = ((h ^ byte) * 16777619) & 0xFFFFFFFF
        if h != memh:
            problems.append('memory')
        if problems:
            key = (x, w)
            if key not in fails:
                fails[key] = (dis, problems, vecs[i][1])
    for (x, w), (dis, problems, st) in list(fails.items())[:80]:
        print(f'FAIL {x:08x} {w:08x}  {dis.mnemonic} {dis.op_str}: {"; ".join(problems[:4])}')
    print(f'{len(picked)} instructions, {len(vecs)} vectors: {len(fails)} failing instructions, '
          f'{nan_diffs} NaN-payload differences', file=sys.stderr)
    sys.exit(1 if fails else 0)


if __name__ == '__main__':
    main()
