"""Pack a build folder into its release archive, for GitHub (make dist).

usage: python tools/dist.py <build dir> <out dir> <windows|linux> <version> [--arch x86_64|aarch64] [--bonus <file>]

Windows: <out>/Windows-MHGURecomp.<version>.zip; Linux: <out>/Linux[ARM64]-MHGURecomp.<version>.tar.gz (the programs
keep their executable permission). Inside, one folder of the same name holds what a player copies: the game, its code
library, the updater, the DLLs (Windows), pipelines.list, licenses/, bonus/system (--bonus: the donor save of the free downloads) and
the empty data/, mods/ and cheats/ folders, with a note in data/ of what goes there. Nothing else of the build folder:
not its game files, mods, settings, caches or log. The updater (updater/updater.c) installs the same archive, leaving
data/, mods/, cheats/ and bonus/ alone. Prints the archive's size and SHA-256: GitHub shows that digest for the upload.
"""
import hashlib
import io
import os
import sys
import tarfile
import time
import zipfile

DATA_NOTE = ('Put your own dump of Monster Hunter Generations Ultimate 1.4.0 here: the exefs and romfs folders\n'
             '(data/exefs/main, data/romfs/...).\n')


def files(build, platform):
    """(path in the archive, file on disk, executable) of everything shipped from the build folder."""
    if platform == 'windows':
        programs = ['mhgurecomp.exe', 'mhgugame.dll', 'mhgurecomp-updater.exe']
        extra = sorted(n for n in os.listdir(build) if n.lower().endswith('.dll') and n not in programs)
    else:
        programs = ['mhgurecomp', 'mhgugame.so', 'mhgurecomp-updater']
        extra = []
    out = []
    for n in programs + extra:
        p = os.path.join(build, n)
        if not os.path.isfile(p):
            sys.exit(f'dist: {p} is missing (make first)')
        out.append((n, p, n in programs))
    pl = os.path.join(build, 'pipelines.list')  # (the recorded pipelines a first run builds: make's pipelist)
    if not os.path.isfile(pl):
        sys.exit(f'dist: {pl} is missing (make first)')
    out.append(('pipelines.list', pl, False))
    lic = os.path.join(build, 'licenses')
    if not os.path.isdir(lic):
        sys.exit(f'dist: {lic} is missing (make first)')
    for d, _, names in sorted(os.walk(lic)):
        for n in sorted(names):
            p = os.path.join(d, n)
            out.append(('licenses/' + os.path.relpath(p, lic).replace(os.sep, '/'), p, False))
    return out


def check_version(build, platform, version):
    """The updater must be this version's: it compares its own with GitHub's latest release."""
    p = os.path.join(build, 'mhgurecomp-updater' + ('.exe' if platform == 'windows' else ''))
    with open(p, 'rb') as f:
        if b'MHGURecomp-updater/' + version.encode() + b'\0' not in f.read():
            sys.exit(f'dist: {p} was not built as version {version} (make dist VERSION={version} rebuilds it)')


def check_arch(build, arch):
    machine = 183 if arch == 'aarch64' else 62
    for name in ('mhgurecomp', 'mhgugame.so', 'mhgurecomp-updater'):
        with open(os.path.join(build, name), 'rb') as f:
            h = f.read(64)
        if not (len(h) == 64 and h[:7] == b'\x7fELF\x02\x01\x01'
                and int.from_bytes(h[16:18], 'little') in (2, 3)
                and int.from_bytes(h[18:20], 'little') == machine
                and h[20:24] == b'\x01\0\0\0' and h[52:54] == b'\x40\0'):
            sys.exit(f'dist: {name} is not a Linux {arch} ELF64 binary')


def main():
    args = sys.argv[1:]
    bonus = None
    arch = 'x86_64'  # old callers package Linux x86-64
    if '--arch' in args:
        i = args.index('--arch')
        arch = args[i + 1]
        del args[i:i + 2]
    arch = {'arm64': 'aarch64', 'amd64': 'x86_64'}.get(arch, arch)
    if '--bonus' in args:
        i = args.index('--bonus')
        bonus = args[i + 1]
        del args[i:i + 2]
    if len(args) != 4 or args[2] not in ('windows', 'linux'):
        sys.exit(__doc__)
    build, out_dir, platform, version = args
    if arch not in ('x86_64', 'aarch64') or (platform == 'windows' and arch != 'x86_64'):
        sys.exit(f'dist: unsupported platform/architecture: {platform}/{arch}')
    check_version(build, platform, version)
    if platform == 'linux':
        check_arch(build, arch)
    # (Linux-: up to 0.11.4 ArchLinux-, when it needed Arch's SDL3 and shaderc; every updater takes both)
    top = ('Windows' if platform == 'windows' else
           'LinuxARM64' if arch == 'aarch64' else 'Linux') + '-MHGURecomp'
    name = f'{top}.{version}' + ('.zip' if platform == 'windows' else '.tar.gz')
    entries = files(build, platform)
    if bonus:
        if not os.path.isfile(bonus):
            sys.exit(f'dist: {bonus} is missing')
        entries.append(('bonus/system', bonus, False))
    else:
        print('dist: no bonus/system (BONUS=<file>): bonus/ left empty')
    dirs = ['', 'data', 'mods', 'cheats', 'bonus']
    for rel, _, _ in entries:
        parts = rel.split('/')[:-1]
        for k in range(1, len(parts) + 1):
            d = '/'.join(parts[:k])
            if d not in dirs:
                dirs.append(d)
    note = 'data/putexefsromfsfoldershere.txt'

    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, name)
    part = path + '.part'
    now = time.time()
    if platform == 'windows':
        with zipfile.ZipFile(part, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for d in dirs:
                z.writestr(zipfile.ZipInfo((top + '/' + d).rstrip('/') + '/', time.localtime(now)[:6]), b'')
            z.writestr(zipfile.ZipInfo(f'{top}/{note}', time.localtime(now)[:6]), DATA_NOTE, zipfile.ZIP_DEFLATED)
            for rel, p, _ in entries:
                z.write(p, f'{top}/{rel}')
    else:
        def info(rel, size, mode, mtime, kind=tarfile.REGTYPE):
            t = tarfile.TarInfo((top + '/' + rel).rstrip('/'))
            t.size, t.mode, t.mtime, t.type = size, mode, int(mtime), kind
            t.uid = t.gid = 0
            t.uname = t.gname = ''
            return t
        with tarfile.open(part, 'w:gz', compresslevel=9, format=tarfile.GNU_FORMAT) as t:
            for d in dirs:
                t.addfile(info(d, 0, 0o755, now, tarfile.DIRTYPE))
            t.addfile(info(note, len(DATA_NOTE), 0o644, now), io.BytesIO(DATA_NOTE.encode()))
            for rel, p, program in entries:
                with open(p, 'rb') as f:
                    t.addfile(info(rel, os.path.getsize(p), 0o755 if program else 0o644, os.path.getmtime(p)), f)
    os.replace(part, path)

    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    print(f'dist: {path}')
    print(f'      {os.path.getsize(path)} bytes, sha256:{h.hexdigest()}')
    print(f'      upload it to the GitHub release {version} (tag {version}, not a pre-release)')


if __name__ == '__main__':
    main()
