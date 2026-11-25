"""The interpreter's JIT (runtime/interp_jit.h) against the interpreter itself, with tools/test_jit.c: instruction
words sampled from a module's code run as blocks of their own on random states (`words`), and whole runs from its
function entries on random registers and memory (`prog`, Linux: it forks). tools/test_lift.py --jit checks the
translations against Unicorn instead (one instruction at a time, x86-64 only).

Usage: test_jit.py <nso> <base> [--words N] [--prog N] [--seed S] [--a64] [--wsl DISTRO]
--a64: the AArch64 back end, cross-built (clang --target=aarch64-linux-gnu, the distro's aarch64-linux-gnu-gcc
sysroot) and run under qemu-aarch64. --wsl DISTRO: build and run inside that WSL distro (from Windows; prog needs
Linux, so it is skipped on Windows without it). Needs clang. Exit status 0 when nothing differs.
"""
import argparse
import os
import platform
import random
import re
import subprocess
import sys

from gen import Module

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def wslpath(p):
    p = os.path.abspath(p).replace('\\', '/')
    return '/mnt/' + p[0].lower() + p[2:] if re.match(r'^[A-Za-z]:', p) else p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('nso')
    ap.add_argument('base', type=lambda s: int(s, 0))
    ap.add_argument('--words', type=int, default=3000)
    ap.add_argument('--prog', type=int, default=400)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--functions', type=int, default=4000)
    ap.add_argument('--a64', action='store_true')
    ap.add_argument('--wsl')
    args = ap.parse_args()
    rng = random.Random(args.seed)
    m = Module('t', args.nso, args.base)
    starts = m.boundaries()
    rng.shuffle(starts)
    order = sorted(starts)
    nxt = {a: (order[i + 1] if i + 1 < len(order) else m.hi) for i, a in enumerate(order)}
    words, seen = [], set()
    for a in starts[:args.functions]:
        code, _, _, _ = m.scan(a, nxt[a])
        for x in sorted(code):
            w = m.word(x)
            ins = m.lift(x)
            if ins.kind != 'op' or w in seen or re.search(r'rt_(svc|ticks|udf|bkpt|thumb)|tpidr|ldrex|strex', ins.code):
                continue
            seen.add(w)
            words.append((x, w))
    rng.shuffle(words)
    words = words[:args.words]
    work = os.path.join(os.environ.get('TEMP', '/tmp'), 'mhgu_test_jit')
    os.makedirs(work, exist_ok=True)
    open(os.path.join(work, 'words.txt'), 'w').write(''.join('%x %08x\n' % xw for xw in words))
    open(os.path.join(work, 'funcs.txt'), 'w').write(''.join('%x\n' % a for a in starts))
    open(os.path.join(work, 'image.bin'), 'wb').write(bytes(m.nso.image))
    print(f'{len(words)} instruction words, {min(args.prog, 2 * len(starts))} whole runs', file=sys.stderr)

    rt = os.path.join(ROOT, 'runtime')
    srcs = [os.path.join(HERE, 'test_jit.c')] + [os.path.join(rt, f) for f in ('interp.c', 'jit_mem.c', 'neon.c', 'simd_int.c')]
    linux = bool(args.wsl) or os.name != 'nt'
    conv = wslpath if args.wsl else (lambda p: p)
    exe = (work + '/test_jit' if not args.wsl else '/tmp/mhgu_test_jit') + ('.exe' if os.name == 'nt' and not args.wsl else '')
    if args.a64:
        if not linux:
            sys.exit('--a64 needs Linux (or --wsl)')
        cc = ['clang', '--target=aarch64-linux-gnu', '--sysroot=/usr/aarch64-linux-gnu', '--gcc-toolchain=/usr',
              '-B"$G"', '-L"$G"', '-static']
        run = ['qemu-aarch64', exe]
    elif platform.machine() in ('aarch64', 'arm64'):  # an AArch64 host: its own back end, run natively
        cc = ['clang', '-march=armv8-a', '-fsigned-char']
        run = [exe]
    else:
        cc = ['clang'] if sys.platform == 'darwin' and os.uname().machine == 'arm64' else ['clang', '-march=x86-64-v3']  # (native on Apple Silicon)
        run = [exe]
    build = cc + ['-O1', '-w', '-I' + conv(rt), '-o', exe] + [conv(s) for s in srcs] + ([] if os.name == 'nt' and not args.wsl else ['-lpthread', '-lm'])
    steps = ' '.join(build) + ' && ' + ' '.join(run) + ' words ' + conv(os.path.join(work, 'words.txt'))
    if linux:
        steps += ' ; r1=$? ; ' + ' '.join(run) + ' prog ' + ' '.join(conv(os.path.join(work, f)) for f in ('image.bin', 'funcs.txt')) + \
                 f' {args.prog} ; r2=$? ; exit $((r1 | r2))'
    if args.a64:
        steps = 'G=/usr/lib/gcc/aarch64-linux-gnu/$(ls /usr/lib/gcc/aarch64-linux-gnu/ | head -1) && ' + steps
    if args.wsl:
        env = dict(os.environ, MSYS_NO_PATHCONV='1')
        r = subprocess.run(['wsl', '-d', args.wsl, '--', 'bash', '-c', '(' + steps + '); echo "test_jit: exit $?"'], env=env,
                           stdout=subprocess.PIPE, text=True)
        sys.stdout.write(r.stdout)
        code = re.findall(r'test_jit: exit (\d+)', r.stdout)
        sys.exit(int(code[-1]) if code else 2)
    elif os.name == 'nt':
        r = subprocess.run(build)
        if not r.returncode:
            r = subprocess.run(run + ['words', os.path.join(work, 'words.txt')])
        print('(prog runs need Linux: --wsl DISTRO)', file=sys.stderr)
    else:
        r = subprocess.run(['bash', '-c', steps])
    sys.exit(r.returncode)


if __name__ == '__main__':
    main()
