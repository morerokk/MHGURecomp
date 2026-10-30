#!/usr/bin/env python3
"""Builds the game code library on a player's Mac from their own game files: a release app carries no game code
(tools/make_app.py --release). The app runs it (runtime/platform/darwin_gamecode.m) with macOS's own python3 and clang,
from Apple's Command Line Developer Tools:

    python3 -E -s build_gamecode.py <game dir> <out .dylib> <executable>

Beside it in the app: gen.py and its modules, hle_symbols.txt, gamelib_entry.c, include/ (the runtime headers the code
includes) and build.json (the flags, the minimum macOS and the id the runtime was built with: runtime/gamelib.h).
The same steps as `make gen` and `make` (Makefile): the C from the game's exefs/main and exefs/sdk, each file compiled,
linked against the executable. For the app's window it prints `progress <0..1> <what>` lines on stdout; the rest
(the generator's and the compiler's output) goes to stderr, which the app keeps as build-log.txt.
"""
import concurrent.futures
import glob
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
# (generating is about a quarter of the time, compiling the rest; linking takes seconds)
GEN_SHARE, LINK_AT = 0.25, 0.98


def say(fraction, what):
    print(f"progress {fraction:.3f} {what}", flush=True)


def log(text):
    sys.stderr.write(text)
    sys.stderr.flush()


def xcrun(*args):
    """The selected developer tools' answer (--sdk macosx: plain xcrun can name a Command Line Tools SDK that isn't
    the selected one's, or isn't there)."""
    return subprocess.run(["/usr/bin/xcrun", "--sdk", "macosx", *args], check=True, capture_output=True,
                          text=True).stdout.strip()


def generate(game, work):
    """generated/main and generated/sdk, both at once (as `make gen`)."""
    jobs = {
        "main": [os.path.join(game, "exefs", "main"), "0x00200000", os.path.join(work, "main")],
        "sdk": [os.path.join(game, "exefs", "sdk"), "0x02400000", os.path.join(work, "sdk"),
                "--hle", os.path.join(HERE, "hle_symbols.txt")],
    }
    done = {name: 0.0 for name in jobs}
    lock = threading.Lock()

    def run(name):
        p = subprocess.Popen([sys.executable, "-E", "-s", os.path.join(HERE, "gen.py"), name, *jobs[name]],
                             cwd=HERE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        for line in p.stdout:
            m = re.search(r"progress (\d+)/(\d+)", line)
            if m:
                with lock:
                    done[name] = int(m.group(1)) / max(int(m.group(2)), 1)
                    # (main is about five times the work of sdk)
                    say(GEN_SHARE * (done["main"] * 5 + done["sdk"]) / 6, "Translating the game's code")
            else:
                log(line)
        return p.wait()

    with concurrent.futures.ThreadPoolExecutor(2) as pool:
        if any(pool.map(run, jobs)):
            sys.exit("build_gamecode: the code generator failed")


def compile_all(work, flags, gamelib_id, cc):
    sources = sorted(glob.glob(os.path.join(work, "main", "*.c")) + glob.glob(os.path.join(work, "sdk", "*.c")))
    sources.append(os.path.join(HERE, "gamelib_entry.c"))
    objs = []

    def one(src):
        obj = os.path.join(work, "obj", os.path.basename(os.path.dirname(src)) + "_" + os.path.basename(src)[:-2] + ".o")
        extra = ['-DGAMELIB_ID="%s"' % gamelib_id] if src.endswith("gamelib_entry.c") else []
        r = subprocess.run([cc, *flags, *extra, "-c", src, "-o", obj], capture_output=True, text=True)
        if r.returncode:
            log(f"{src}:\n{r.stderr}")
            raise RuntimeError(f"{os.path.basename(src)} did not compile")
        return obj

    os.makedirs(os.path.join(work, "obj"))
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 4) as pool:
        futures = [pool.submit(one, s) for s in sources]
        for n, f in enumerate(concurrent.futures.as_completed(futures), 1):
            objs.append(f.result())
            say(GEN_SHARE + (LINK_AT - GEN_SHARE) * n / len(sources),
                f"Compiling the game's code: {n} of {len(sources)} files")
    return objs


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    game, out, exe = (os.path.abspath(a) for a in sys.argv[1:])
    with open(os.path.join(HERE, "build.json")) as f:
        conf = json.load(f)
    # clang by its path, with the SDK it finds macOS's headers and libraries in (xcrun would add it, per file)
    cc, sdk = xcrun("--find", "clang"), xcrun("--show-sdk-path")
    # the Makefile's GENFLAGS, with the runtime's headers from the app
    flags = [("-I" + os.path.join(HERE, "include")) if f == "-Iruntime" else f for f in shlex.split(conf["genflags"])]
    flags += ["-isysroot", sdk]
    work = out + ".build"
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    say(0, "Translating the game's code")
    generate(game, work)
    objs = compile_all(work, flags, conf["id"], cc)
    say(LINK_AT, "Linking")
    tmp = out + ".tmp"
    r = subprocess.run([cc, "-bundle", "-bundle_loader", exe, "-isysroot", sdk, "-mmacosx-version-min=" + conf["min_macos"],
                        "-o", tmp, *objs], capture_output=True, text=True)
    log(r.stderr)
    if r.returncode:
        sys.exit("build_gamecode: linking failed")
    os.replace(tmp, out)
    shutil.rmtree(work, ignore_errors=True)
    for old in glob.glob(os.path.join(os.path.dirname(out), "mhgugame-*.dylib")):  # (other runtime versions')
        if old != out:
            os.remove(old)
    say(1, "Done")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as e:
        sys.exit(f"build_gamecode: {e}")
