#!/usr/bin/env python3
"""Make a self-contained macOS app of a build.

    python3 tools/make_app.py [--libs DIR] <build dir> <game dir> <app path>
    python3 tools/make_app.py --release --gen-flags F --gamelib-id ID [--version V] [--min-macos X] [--libs DIR]
                              [--bonus DIR] <build dir> <app path>

The app holds the runtime and the game code library of <build dir>, the game files (<game dir>/exefs and /romfs as APFS
clones: no extra space on the same disk), the libraries they load from outside macOS (those make macos-deps built in
--libs, found also through the build's rpath; rewritten to load from the app), Homebrew's MoltenVK with its Vulkan
driver manifest, and the game's own icon when the extraction left one (<game dir>/icon.jpg). It is signed ad hoc for
this Mac.

Everything the game writes goes to ~/Library/Application Support/MHGURecomp (runtime/platform/darwin.c): settings,
caches, mods/, cheats/, log.txt and the saves.

The app contains game code and data made from your own dump: it is for you, never for sharing.

--release makes the app a release is (make release): no game code, game files or game icon in it. Players put their
own exefs/ and romfs/ in ~/Library/Application Support/MHGURecomp/data (runtime/main.c), and the app builds the game
code from them on their Mac at its first start (runtime/platform/darwin_gamecode.m), with what it carries in
Contents/Resources/gamecode: tools/build_gamecode.py, the code generator, the runtime headers the code includes and
build.json (--gen-flags: the Makefile's GENFLAGS; --gamelib-id: the id the runtime was built with). Also: the version
and --min-macos in Info.plist, the donor save of --bonus in Contents/Resources/bonus (runtime/services/bonus.c), no
debug map, and a check that every binary in it loads on --min-macos and names no folder of this Mac, and that it holds
no game code.
"""
import argparse
import json
import os
import plistlib
import shutil
import subprocess
import sys

SYSTEM_PREFIXES = ("/usr/lib/", "/System/")  # (what macOS has: everything else goes into the app)
LIB_DIRS = []  # where @rpath references are looked up first (--libs)
EXE = "mhgurecomp"
GAMELIB = "mhgugame.dylib"
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# Homebrew's MoltenVK (the Makefile's BREW: `brew --prefix`)
BREW = os.environ.get("BREW") or subprocess.run(["brew", "--prefix"], capture_output=True, text=True).stdout.strip() \
    or "/opt/homebrew"
MOLTENVK = os.path.join(BREW, "opt", "molten-vk")
# what builds the game code on a player's Mac (tools/build_gamecode.py): {in Resources/gamecode: in the repository}
GAMECODE_FILES = {
    "build_gamecode.py": "tools/build_gamecode.py",
    "gen.py": "tools/gen.py", "lifter.py": "tools/lifter.py", "neon.py": "tools/neon.py", "nso.py": "tools/nso.py",
    "hle_symbols.txt": "runtime/hle/hle_symbols.txt",
    "gamelib_entry.c": "tools/gamelib_entry.c",
    **{"include/" + h: "runtime/" + h for h in ("cpu.h", "vfp.h", "neon_rt.h", "gamelib.h")},
}
# Contents/Resources/Licenses: a folder per bundled library (from <--libs>/../share/licenses: tools/macos_deps.sh, and
# Homebrew's MoltenVK), and MHGU Recomp's own
LICENSED = {
    "SDL3": "libSDL3",
    "FFmpeg": "libavformat, libavcodec and libavutil (where their source is and how they were built: FFmpeg/SOURCE.txt)",
    "Vulkan-Loader": "libvulkan",
    "shaderc": "libshaderc_shared, with glslang and SPIRV-Tools (and SPIRV-Headers' headers)",
    "MoltenVK": "libMoltenVK",
    "MHGURecomp": "MHGU Recomp itself (MIT), and the code it builds on and bundles (THIRD_PARTY_NOTICES)",
}


def run(*cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, text=True, **kw).stdout


def deps(path):
    """The libraries a Mach-O file loads, as written in it (otool -L, without its own id)."""
    lines = run("otool", "-L", path).splitlines()[1:]
    out = [l.strip().split(" (")[0] for l in lines if l.strip()]
    own_id = run("otool", "-D", path).splitlines()[1:]
    return [d for d in out if not own_id or d != own_id[0].strip()]


def resolve(dep, loader):
    """A dependency's file: absolute, or relative to the library that loads it (@loader_path / @rpath)."""
    if dep.startswith("@loader_path/"):
        return os.path.realpath(os.path.join(os.path.dirname(loader), dep[len("@loader_path/"):]))
    if dep.startswith("@rpath/"):
        for base in (*LIB_DIRS, *[r for r in rpaths(loader) if r.startswith("/")], os.path.dirname(loader),
                     os.path.join(os.path.dirname(loader), "..", "lib")):
            p = os.path.join(base, dep[len("@rpath/"):])
            if os.path.exists(p):
                return os.path.realpath(p)
        return None
    return os.path.realpath(dep) if os.path.exists(dep) else dep


def bundle_libraries(roots, frameworks):
    """Copy every library the roots need (recursively) that macOS doesn't have into frameworks; point every reference at
    the copy.
    roots: {file in the app: the file it was copied from} (whose folder @loader_path / @rpath references go by)."""
    origin = dict(roots)
    copied = {}  # real path -> name in Frameworks
    todo = list(roots)
    while todo:
        f = todo.pop()
        for d in deps(f):
            real = resolve(d, origin[f])
            if real and real.startswith(SYSTEM_PREFIXES):
                continue
            if not real or not os.path.isfile(real):
                sys.exit(f"make_app: {os.path.basename(f)} loads {d}, which isn't here")
            name = os.path.basename(real)
            if real not in copied:
                copied[real] = name
                dst = os.path.join(frameworks, name)
                shutil.copy2(real, dst)
                os.chmod(dst, 0o755)
                run("install_name_tool", "-id", "@rpath/" + name, dst)
                origin[dst] = real
                todo.append(dst)
            run("install_name_tool", "-change", d, "@rpath/" + copied[real], f)
    return copied


def add_rpath(path, rpath):
    if rpath not in run("otool", "-l", path):
        run("install_name_tool", "-add_rpath", rpath, path)


def rpaths(path):
    lines = run("otool", "-l", path).splitlines()
    return [lines[i + 2].split()[1] for i, l in enumerate(lines) if l.strip() == "cmd LC_RPATH"]


def drop_build_paths(path, strip):
    """No absolute rpath (the Makefile's, to $(DEPS)/lib: the app loads its own copies); strip: no debug map either, so
    a release keeps no path of the build machine."""
    for r in rpaths(path):
        if not r.startswith("@"):
            run("install_name_tool", "-delete_rpath", r, path)
    if strip:
        run("strip", "-S", path)  # (debug map only: the symbols stay, for the game code library and crash backtraces)


def version(v):
    return tuple(int(x) for x in v.split("."))


def min_macos(path):
    """The oldest macOS a Mach-O file loads on (LC_BUILD_VERSION minos, or LC_VERSION_MIN_MACOSX version)."""
    out = run("otool", "-l", path).splitlines()
    for i, l in enumerate(out):
        if l.strip() in ("cmd LC_BUILD_VERSION", "cmd LC_VERSION_MIN_MACOSX"):
            for m in out[i + 1:i + 5]:
                k = m.split()
                if k and k[0] in ("minos", "version"):
                    return k[1]
    return None


def check_release(app, want):
    """Every binary in the app loads on macOS `want`, loads nothing that isn't in the app or in macOS, and names no
    folder of the home of whoever built it."""
    bad = []
    home = os.path.expanduser("~").encode()
    for root, _, files in os.walk(os.path.join(app, "Contents")):
        for name in files:
            f = os.path.join(root, name)
            if name.startswith("mhgugame") or (root.endswith(("/main", "/sdk")) and name.endswith(".c")):
                bad.append(f"{name}: game code")
            if os.path.islink(f) or not run("file", "-b", f).startswith("Mach-O"):
                continue
            v = min_macos(f)
            if not v or version(v) > version(want):
                bad.append(f"{name}: needs macOS {v}")
            for d in deps(f):
                if not d.startswith(("@rpath/", "@loader_path/", "@executable_path/") + SYSTEM_PREFIXES):
                    bad.append(f"{name}: loads {d}")
            for r in rpaths(f):
                if not r.startswith("@"):
                    bad.append(f"{name}: rpath {r}")
            with open(f, "rb") as fh:
                if home in fh.read():
                    bad.append(f"{name}: names {home.decode()} (a folder of this Mac)")
    if bad:
        sys.exit("make_app: not a release:\n  " + "\n  ".join(bad))


def add_licenses(resources):
    lic = os.path.join(resources, "Licenses")
    for d in LIB_DIRS:
        src = os.path.join(d, "..", "share", "licenses")
        if os.path.isdir(src):
            shutil.copytree(src, lic, dirs_exist_ok=True, ignore=shutil.ignore_patterns(".complete"))
    os.makedirs(os.path.join(lic, "MoltenVK"), exist_ok=True)
    shutil.copy2(os.path.join(MOLTENVK, "LICENSE"), os.path.join(lic, "MoltenVK", "LICENSE"))
    os.makedirs(os.path.join(lic, "MHGURecomp"), exist_ok=True)
    for f in ("LICENSE", "THIRD_PARTY_NOTICES"):
        shutil.copy2(os.path.join(REPO, f), os.path.join(lic, "MHGURecomp", f))
    with open(os.path.join(lic, "README.txt"), "w") as f:
        f.write("MHGU Recomp bundles these libraries, each under its own license: the files in its folder.\n\n")
        for name, what in LICENSED.items():
            f.write(f"{name}/: {what}\n")
    # the libraries without license files
    return [n for n in LICENSED if not os.path.isdir(os.path.join(lic, n)) or not os.listdir(os.path.join(lic, n))]


def make_icon(jpg, resources):
    """The game's icon (a JPEG from its control data) as AppIcon.icns."""
    iconset = os.path.join(resources, "AppIcon.iconset")
    os.makedirs(iconset, exist_ok=True)
    for size in (16, 32, 64, 128, 256, 512):
        for scale in (1, 2):
            px = size * scale
            name = f"icon_{size}x{size}{'@2x' if scale == 2 else ''}.png"
            run("sips", "-s", "format", "png", "-z", str(px), str(px), jpg, "--out", os.path.join(iconset, name))
    run("iconutil", "-c", "icns", iconset, "-o", os.path.join(resources, "AppIcon.icns"))
    shutil.rmtree(iconset)


def main():
    ap = argparse.ArgumentParser(usage=__doc__.split("\n\n")[1])
    ap.add_argument("--release", action="store_true")
    ap.add_argument("--version", default="1.4.0")
    ap.add_argument("--min-macos", default="13.0")
    ap.add_argument("--libs", action="append", default=[])
    ap.add_argument("--bonus")
    ap.add_argument("--gen-flags")
    ap.add_argument("--gamelib-id")
    ap.add_argument("paths", nargs="+")
    a = ap.parse_args()
    if len(a.paths) != (2 if a.release else 3) or (a.release and not (a.gen_flags and a.gamelib_id)):
        sys.exit(__doc__)
    build, app = os.path.abspath(a.paths[0]), os.path.abspath(a.paths[-1])
    game = None if a.release else os.path.abspath(a.paths[1])
    LIB_DIRS[:] = [os.path.abspath(d) for d in a.libs]
    for f in (os.path.join(build, EXE),) + (() if a.release else (os.path.join(build, GAMELIB),)):
        if not os.path.isfile(f):
            sys.exit(f"make_app: {f} is missing: build first (make GAME=...)")
    if game and (not os.path.isfile(os.path.join(game, "exefs", "main")) or not os.path.isdir(os.path.join(game, "romfs"))):
        sys.exit(f"make_app: no exefs/ and romfs/ in {game}")
    bonus = a.bonus and os.path.join(os.path.abspath(a.bonus), "system")
    if bonus and not os.path.isfile(bonus):
        sys.exit(f"make_app: no donor save (system) in {a.bonus}")
    if os.path.exists(app):
        shutil.rmtree(app)
    contents = os.path.join(app, "Contents")
    macos, frameworks, resources = (os.path.join(contents, d) for d in ("MacOS", "Frameworks", "Resources"))
    for d in (macos, frameworks, resources):
        os.makedirs(d)

    # code: the runtime, and the game code library (a release: what builds it on the player's Mac instead)
    exe = os.path.join(macos, EXE)
    roots = {exe: os.path.join(build, EXE)}
    if a.release:
        gamecode = os.path.join(resources, "gamecode")
        for dst, src in GAMECODE_FILES.items():
            os.makedirs(os.path.dirname(os.path.join(gamecode, dst)), exist_ok=True)
            shutil.copy2(os.path.join(REPO, src), os.path.join(gamecode, dst))
        with open(os.path.join(gamecode, "build.json"), "w") as f:
            json.dump({"genflags": a.gen_flags.strip(), "id": a.gamelib_id, "min_macos": a.min_macos}, f, indent=2)
    else:
        roots[os.path.join(macos, GAMELIB)] = os.path.join(build, GAMELIB)
    for dst, src in roots.items():
        shutil.copy2(src, dst)
        drop_build_paths(dst, strip=a.release)
    moltenvk_src = os.path.realpath(os.path.join(MOLTENVK, "lib", "libMoltenVK.dylib"))
    moltenvk = os.path.join(frameworks, "libMoltenVK.dylib")
    shutil.copy2(moltenvk_src, moltenvk)
    os.chmod(moltenvk, 0o755)
    run("install_name_tool", "-id", "@rpath/libMoltenVK.dylib", moltenvk)
    libs = bundle_libraries({**roots, moltenvk: moltenvk_src}, frameworks)
    add_rpath(exe, "@executable_path/../Frameworks")
    for name in os.listdir(frameworks):
        add_rpath(os.path.join(frameworks, name), "@loader_path")
    print(f"make_app: {len(libs) + 1} libraries in Frameworks")

    # Vulkan driver manifest (the runtime points the loader at it: runtime/platform/darwin.c plat_init)
    icd_dir = os.path.join(resources, "vulkan", "icd.d")
    os.makedirs(icd_dir)
    with open(os.path.join(MOLTENVK, "etc", "vulkan", "icd.d", "MoltenVK_icd.json")) as f:
        icd = json.load(f)
    icd["ICD"]["library_path"] = "../../../Frameworks/libMoltenVK.dylib"
    with open(os.path.join(icd_dir, "MoltenVK_icd.json"), "w") as f:
        json.dump(icd, f, indent=2)

    # game files: APFS clones (cp -c), copies on other file systems. A release has none: the player's own are in
    # ~/Library/Application Support/MHGURecomp/data (runtime/main.c)
    if game:
        data = os.path.join(resources, "data")
        os.makedirs(data)
        for d in ("exefs", "romfs"):
            subprocess.run(["cp", "-cR", os.path.join(game, d), os.path.join(data, d)], check=True)
        os.symlink("../Resources/data", os.path.join(macos, "data"))
    # the pipelines recorded in the common areas, built in the background on a first run (make's pipelist; the
    # runtime reads it from Resources in an app: runtime/nvn/vulkan/exec.c plist_load)
    plist = os.path.join(REPO, "extras", "pipelines.list")
    if os.path.isfile(plist):
        shutil.copy2(plist, os.path.join(resources, "pipelines.list"))
    elif a.release:
        sys.exit("make_app: no extras/pipelines.list")
    if bonus:
        os.makedirs(os.path.join(resources, "bonus"))
        shutil.copy2(bonus, os.path.join(resources, "bonus", "system"))

    missing = add_licenses(resources)
    if missing and a.release:
        sys.exit(f"make_app: no license files for {', '.join(missing)}: make macos-deps keeps them")

    # icon (the game's own: not in a release) and Info.plist
    icon = game and os.path.join(game, "icon.jpg")
    if icon and os.path.isfile(icon):
        make_icon(icon, resources)
    plist = {
        "CFBundleIdentifier": "local.mhgurecomp.game",
        "CFBundleName": "MHGU Recomp",
        "CFBundleDisplayName": "MHGU Recomp",
        "CFBundleExecutable": EXE,
        "CFBundlePackageType": "APPL",
        "CFBundleShortVersionString": a.version,
        "CFBundleVersion": run("git", "-C", os.path.dirname(os.path.abspath(__file__)), "rev-parse", "--short",
                               "HEAD").strip() or "1",
        "LSMinimumSystemVersion": a.min_macos,
        "LSApplicationCategoryType": "public.app-category.games",  # (Game Mode in full screen)
        "NSHighResolutionCapable": True,
        "GCSupportsControllerUserInteraction": True,
        # (Local Play finds other players' hunts by broadcasting on the local network: runtime/net/ldn.c)
        "NSLocalNetworkUsageDescription": "MHGU Recomp finds and joins other players' hunts on your local network "
                                          "(Local Play multiplayer).",
    }
    if os.path.isfile(os.path.join(resources, "AppIcon.icns")):
        plist["CFBundleIconFile"] = "AppIcon"
    with open(os.path.join(contents, "Info.plist"), "wb") as f:
        plistlib.dump(plist, f)

    # ad-hoc signature: the libraries first (install_name_tool invalidated theirs), then the app
    for name in sorted(os.listdir(frameworks)):
        run("codesign", "--force", "-s", "-", os.path.join(frameworks, name))
    for f in roots:
        if f != exe:  # (the game code library; the executable is signed with the app)
            run("codesign", "--force", "-s", "-", f)
    run("codesign", "--force", "-s", "-", app)
    if a.release:
        check_release(app, a.min_macos)
    print(f"make_app: {app}")


if __name__ == "__main__":
    main()
