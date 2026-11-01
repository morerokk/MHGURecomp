#!/usr/bin/env python3
# Reads what MHGU_SAMPLE=<file> wrote (runtime/sampler.c, Windows).
# samples.py <file> timeline [bin s]           per bin: frames presented a second, Gcycles a second per busy thread
# samples.py <file> top <t0> <t1> [n] [match]  per thread in [t0, t1): the top functions where the time went (leaf)
#                                              and under which (inclusive), by CPU cycles; only stacks with <match>
# samples.py <file> wait <t0> <t1> [n] [match] the same for sleeping threads: where they wait (by samples)
# t0 / t1: seconds from the first record, or Python's time.perf_counter() (both QueryPerformanceCounter: a log
# timestamped with it lines up). Game functions show as main:<guest address> / sdk:<guest address>, the runtime's by
# name (nm on mhgurecomp.exe), others as module+offset.
import bisect, os, struct, subprocess, sys
from collections import defaultdict

path = sys.argv[1]
mods, fns = [], []
qpf = 1e7
for l in open(path + '.map'):
    p = l.split(None, 3)
    if p[0] == 'qpf': qpf = float(p[1])
    elif p[0] == 'module': mods.append((int(p[1], 16), int(p[2], 16), p[3].strip()))
    elif p[0] == 'fn': fns.append((int(p[1], 16), p[2] + ':' + p[3].strip()))
fns.sort()
fn_addr = [a for a, _ in fns]
exe = next(m for m in mods if m[2].lower().endswith('mhgurecomp.exe'))
syms = []
nm = subprocess.run([os.environ.get('NM', 'nm'), '-C', exe[2]], capture_output=True, text=True).stdout
for l in nm.splitlines():
    p = l.split(None, 2)
    if len(p) == 3 and p[1] in 'tT': syms.append((int(p[0], 16), p[2]))
syms.sort()
sym_addr = [a for a, _ in syms]
img_base = 0x140000000

cache = {}
def name(a):
    if a in cache: return cache[a]
    r = None
    for base, size, mp in mods:
        if base <= a < base + size:
            mn = os.path.basename(mp).lower()
            if mn == 'mhgugame.dll':
                i = bisect.bisect_right(fn_addr, a) - 1
                r = fns[i][1] if i >= 0 else 'dll?'
            elif mn == 'mhgurecomp.exe':
                i = bisect.bisect_right(sym_addr, a - base + img_base) - 1
                r = syms[i][1] if i >= 0 else 'exe?'
            else:
                r = '%s+%x' % (mn, a - base)
            break
    cache[a] = r or '?%x' % a
    return cache[a]

data = open(path, 'rb').read()
S, F = [], []
i = 0
while i < len(data):
    k = data[i:i + 1]; i += 1
    if k == b'F':
        if i + 16 > len(data): break
        t, n = struct.unpack_from('<QQ', data, i); i += 16
        F.append((t / qpf, n))
    elif k == b'S':
        if i + 32 > len(data): break
        t, tid, cyc, n = struct.unpack_from('<QQQQ', data, i); i += 32
        if i + 8 * n > len(data): break
        fr = struct.unpack_from('<%dQ' % n, data, i); i += 8 * n
        S.append((t / qpf, tid, cyc, fr))
    else:
        break
T0 = min(F[0][0] if F else 1e18, S[0][0] if S else 1e18)
print('T0 %.4f (perf_counter), %d samples, %.1f s' % (T0, len(S), S[-1][0] - T0))

if sys.argv[2] == 'timeline':
    b = float(sys.argv[3]) if len(sys.argv) > 3 else 0.5
    bins = defaultdict(lambda: defaultdict(int))
    for t, tid, cyc, fr in S: bins[int((t - T0) / b)][tid] += cyc
    fps = {}
    for (t1, n1), (t2, n2) in zip(F, F[1:]):
        fps.setdefault(int((t1 - T0) / b), []).append((n2 - n1) / (t2 - t1))
    top_tids = defaultdict(int)
    for t, tid, cyc, fr in S: top_tids[tid] += cyc
    tids = [t for t, _ in sorted(top_tids.items(), key=lambda x: -x[1])[:8]]
    print('   t    fps  ' + ' '.join('%6d' % t for t in tids) + '   (Gcycles/s per thread)')
    for k in sorted(set(bins) | set(fps)):
        f = fps.get(k)
        print('%6.1f %5.0f  ' % (k * b, sum(f) / len(f) if f else 0) +
              ' '.join('%6.2f' % (bins[k][t] / b / 1e9) for t in tids))
else:
    a, z = float(sys.argv[3]), float(sys.argv[4])
    if a > 1e4: a, z = a - T0, z - T0  # (perf_counter seconds, as in ts.py's log)
    n = int(sys.argv[5]) if len(sys.argv) > 5 else 15
    filt = sys.argv[6] if len(sys.argv) > 6 else None
    wait = sys.argv[2] == 'wait'  # (samples of sleeping threads, counted)
    per = defaultdict(lambda: [0, defaultdict(int), defaultdict(int)])
    for t, tid, cyc, fr in S:
        if not (a <= t - T0 < z): continue
        if wait != (cyc < 20000): continue
        if wait: cyc = 1e7
        names = [name(x) for x in fr]
        if filt and not any(filt in x for x in names): continue
        e = per[tid]
        e[0] += cyc
        e[1][names[0] if names else '?'] += cyc
        for x in set(names): e[2][x] += cyc
    for tid, (tot, leaf, inc) in sorted(per.items(), key=lambda x: -x[1][0])[:6]:
        if tot < 0.02 * 1e9: continue
        print('\nthread %d: %.2f Gcycles' % (tid, tot / 1e9))
        print('  leaf:')
        for k, v in sorted(leaf.items(), key=lambda x: -x[1])[:n]: print('   %5.1f%% %s' % (100 * v / tot, k[:110]))
        print('  inclusive:')
        for k, v in sorted(inc.items(), key=lambda x: -x[1])[:n]: print('   %5.1f%% %s' % (100 * v / tot, k[:110]))
