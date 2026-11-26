"""Copy the DLLs a Windows build needs next to it, so the executable starts without MSYS2 on PATH.

usage: python tools/bundle_dlls.py <dll dir> <out dir> <exe or dll>... [--own <dir>]... [--load <dll>]...
           [--licenses <dir> [--with <package>...]]

Follows the import tables (and delay-load imports) of the given files recursively. A DLL found in <dll dir> (MSYS2's
ucrt64/bin) is copied to <out dir> when missing or older; anything not found there is the system's (kernel32,
ucrtbase...) and is left alone. vulkan-1.dll is never copied: the GPU driver's own loader (System32) must be used.

--own: a folder of DLLs built here (SDL3.dll), found there before <dll dir>; copied the same way, their licenses not
collected (the Makefile writes those). --load: a DLL loaded at run time with LoadLibrary rather than imported
(libusb-1.0.dll, which SDL opens when a Switch 2 controller is plugged in), bundled as if imported.

--licenses: the license files of the MSYS2 package behind each DLL copied, and of the packages named after --with
(libraries linked in rather than shipped as DLLs, without the package prefix: libstdc++, glslang...), go to
<dir>/<package>/, and <dir>/README.txt lists them all: version, license, where its exact source is, and a written
offer for the source of the (L)GPL ones shipped as DLLs. Read from pacman's local database (var/lib/pacman/local).
"""
import os
import shutil
import struct
import sys

NEVER = {'vulkan-1.dll'}
PROJECT = 'https://github.com/morerokk/MHGURecomp'


def imports(path):
    """Names of the DLLs a PE file imports (normal and delay-load)."""
    with open(path, 'rb') as f:
        d = f.read()
    pe = struct.unpack_from('<I', d, 0x3C)[0]
    if d[pe:pe + 4] != b'PE\0\0':
        return []
    nsec, opt_size = struct.unpack_from('<H', d, pe + 6)[0], struct.unpack_from('<H', d, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from('<H', d, opt)[0]
    dirs = opt + (112 if magic == 0x20B else 96)  # PE32+ / PE32 data directories
    sections = []
    for i in range(nsec):
        s = opt + opt_size + 40 * i
        vsize, va, rsize, raw = struct.unpack_from('<IIII', d, s + 8)
        sections.append((va, max(vsize, rsize), raw))

    def off(rva):
        for va, size, raw in sections:
            if va <= rva < va + size:
                return rva - va + raw
        return None

    def cstr(rva):
        o = off(rva)
        return d[o:d.index(b'\0', o)].decode('ascii', 'replace') if o is not None else None

    names = []
    for index, entry, name_at in ((1, 20, 12), (13, 32, 4)):  # import table, delay-load import table
        rva = struct.unpack_from('<I', d, dirs + 8 * index)[0]
        o = off(rva) if rva else None
        while o is not None and o + entry <= len(d):
            name_rva = struct.unpack_from('<I', d, o + name_at)[0]
            if not name_rva:
                break
            n = cstr(name_rva)
            if n:
                names.append(n)
            o += entry
    return names


def pacman_db(root):
    """pacman's local database: {package name: {'BASE', 'VERSION', 'LICENSE', 'URL': ..., 'FILES': [...]}}."""
    db = {}
    local = os.path.join(root, 'var', 'lib', 'pacman', 'local')
    for entry in os.listdir(local) if os.path.isdir(local) else []:
        info = {}
        for part in ('desc', 'files'):
            p = os.path.join(local, entry, part)
            if not os.path.exists(p):
                continue
            key = None
            for line in open(p, encoding='utf-8', errors='replace').read().splitlines():
                if line.startswith('%') and line.endswith('%'):
                    key = line.strip('%')
                    info.setdefault(key, [])
                elif line and key:
                    info[key].append(line)
        if info.get('NAME'):
            db[info['NAME'][0]] = info
    return db


def write_licenses(src, out, dlls, extra):
    root = os.path.dirname(os.path.dirname(os.path.abspath(src)))  # (C:/msys64/ucrt64/bin -> C:/msys64)
    prefix_dir = os.path.basename(os.path.dirname(os.path.abspath(src)))  # ucrt64
    db = pacman_db(root)
    if not db:
        print('bundle_dlls: no pacman database: licenses of the bundled DLLs not collected')
        return
    owner = {}
    for name, info in db.items():
        for f in info.get('FILES', []):
            owner[f.lower()] = name
    prefix = os.environ.get('MINGW_PACKAGE_PREFIX', 'mingw-w64-ucrt-x86_64')
    rows = []  # (what, package, how)
    for dll in sorted(dlls):
        pkg = owner.get(f'{prefix_dir}/bin/{dll}'.lower())
        if pkg:
            rows.append((dll, pkg, 'DLL'))
        else:
            print(f'bundle_dlls: no package owns {dll}: its license not collected')
    for short in extra:
        if f'{prefix}-{short}' in db:
            rows.append((short, f'{prefix}-{short}', 'linked in'))
        else:
            print(f'bundle_dlls: package {prefix}-{short} is not installed: its license not collected')
    os.makedirs(out, exist_ok=True)
    for n in os.listdir(out):  # (the package folders are all this script's: made anew)
        if os.path.isdir(os.path.join(out, n)):
            shutil.rmtree(os.path.join(out, n))
    lines = ['Licenses of the third-party software in this game folder, besides LICENSE.txt (MHGU Recomp itself),',
             'THIRD_PARTY_NOTICES.txt (code compiled into mhgurecomp.exe), FFmpeg.txt and SDL3.txt.',
             '',
             'From MSYS2 (https://www.msys2.org/): each package\'s license files are in the folder of its name here, and its',
             'exact source (the upstream release and MSYS2\'s build recipe and patches) at the address given.', '']
    gpl = []
    done = set()
    for what, pkg, how in rows:
        info = db[pkg]
        short = pkg[len(prefix) + 1:] if pkg.startswith(prefix + '-') else pkg
        version, base = info.get('VERSION', ['?'])[0], info.get('BASE', [pkg])[0]
        lic = ' '.join(info.get('LICENSE', ['?']))
        source = f'https://repo.msys2.org/mingw/sources/{base}-{version}.src.tar.zst'
        lines += [f'{what} ({how}): {pkg} {version}', f'    license: {lic}', f'    source:  {source}',
                  f'    project: {info.get("URL", ["?"])[0]}', '']
        if how == 'DLL' and 'GPL' in lic.split('documentation:')[0]:
            gpl.append(f'{what} ({pkg} {version})')
        if pkg in done:
            continue
        done.add(pkg)
        for f in info.get('FILES', []):
            if '/share/licenses/' in f and not f.endswith('/'):
                rel = f.split('/share/licenses/', 1)[1]
                t = os.path.join(out, short, *rel.split('/')[1:])
                os.makedirs(os.path.dirname(t), exist_ok=True)
                shutil.copy2(os.path.join(root, *f.split('/')), t)
    if gpl:
        lines += ['Written offer: for at least three years after this copy of the program was released, the complete',
                  'corresponding source code of these (L)GPL-licensed parts, in the versions included here, is available from',
                  f'the MHGU Recomp project on request ({PROJECT}/issues), besides MSYS2\'s addresses above:']
        lines += [f'    {g}' for g in gpl] + ['']
    with open(os.path.join(out, 'README.txt'), 'w', newline='\r\n') as f:
        f.write('\n'.join(lines))
    print(f'bundle_dlls: licenses of {len(done)} packages in {out}')


def main():
    args = sys.argv[1:]
    lic_dir, extra, own, load = None, [], [], []
    if '--with' in args:
        extra = args[args.index('--with') + 1:]
        args = args[:args.index('--with')]
    rest = []
    while args:
        a = args.pop(0)
        if a in ('--licenses', '--own', '--load'):
            v = args.pop(0)
            if a == '--licenses':
                lic_dir = v
            else:
                (own if a == '--own' else load).append(v)
        else:
            rest.append(a)
    src, out, roots = rest[0], rest[1], rest[2:]
    have = {n.lower(): os.path.join(src, n) for n in os.listdir(src)}
    msys = set(have)
    for d in own:
        for n in os.listdir(d):
            if n.lower().endswith('.dll'):
                have[n.lower()] = os.path.join(d, n)
                msys.discard(n.lower())
    seen, todo, copied = set(), list(roots), 0
    pending = list(load)
    while todo or pending:
        for name in pending or imports(todo.pop()):
            k = name.lower()
            if k in seen or k in NEVER:
                continue
            if k not in have:
                if name in pending:
                    sys.exit(f'bundle_dlls: {name} (--load) is in neither {src} nor {own}')
                continue
            seen.add(k)
            s = have[k]
            t = os.path.join(out, os.path.basename(s))
            # (an own DLL also when its size differs: build/ may hold a newer copy of MSYS2's from an SDL3=system build)
            if not os.path.exists(t) or os.path.getmtime(t) < os.path.getmtime(s) or \
                    (k not in msys and os.path.getsize(t) != os.path.getsize(s)):
                shutil.copy2(s, t)
                copied += 1
            todo.append(s)
        pending = []
    print(f'bundle_dlls: {len(seen)} DLLs needed, {copied} copied to {out}')
    for n in os.listdir(out):  # (MSYS2 DLLs an earlier build needed and this one doesn't)
        if n.lower().endswith('.dll') and n.lower() in have and n.lower() not in seen:
            os.remove(os.path.join(out, n))
            print(f'bundle_dlls: {n} not needed any more: removed')
    if lic_dir:
        write_licenses(src, lic_dir, [os.path.basename(have[k]) for k in seen if k in msys], extra)


if __name__ == '__main__':
    main()
