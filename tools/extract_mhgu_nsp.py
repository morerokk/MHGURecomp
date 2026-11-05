#!/usr/bin/env python3
"""Extract MHGU base + 1.4.0 update NSPs into exefs/ + romfs/ using build/hactool.

Double-click Extract MHGU NSP.bat, or:
  python tools/extract_mhgu_nsp.py
  python tools/extract_mhgu_nsp.py --cli --base game.nsp --update update.nsp --keys prod.keys --out ../mhgu/game

Never modifies the NSP files. Needs ~20 GB free while working.
"""
from __future__ import annotations

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import traceback
from pathlib import Path
from typing import Callable

TITLE_ID = 0x0100770008DD8000
# Update/patch NSPs use the app title ID with bit 0x800 set (Switchbrew).
PATCH_TITLE_ID = TITLE_ID | 0x800  # 0100770008DD8800
CNMT_VERSION = 262144  # 1.4.0
NEED_FREE = 20 * 1024**3


def title_id_ok(tid: int) -> bool:
    return tid in (TITLE_ID, PATCH_TITLE_ID, 0)

ProgressFn = Callable[[str], None]


class ExtractError(Exception):
    pass


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def find_hactool() -> Path:
    root = repo_root()
    for p in (root / "build" / "hactool.exe", root / "build" / "hactool",
              root / "hactool.exe", root / "hactool"):
        if p.is_file():
            return p
    raise ExtractError("hactool not found.\nBuild it first in MSYS2 UCRT64:\n  make -j4 hactool")


def free_bytes(path: Path) -> int | None:
    try:
        return shutil.disk_usage(path).free
    except OSError:
        return None


def run(hactool: Path, keys: Path, home: Path, *args: str, check: bool = True) -> subprocess.CompletedProcess:
    env = os.environ.copy()
    env["HOME"] = str(home)
    if os.name == "nt":
        env["USERPROFILE"] = str(home)
    cmd = [str(hactool), "-k", str(keys), "--disablekeywarns", *args]
    r = subprocess.run(cmd, env=env, capture_output=True, text=True)
    if check and r.returncode != 0:
        detail = ((r.stdout or "") + (r.stderr or "")).strip()
        raise ExtractError(f"hactool failed ({r.returncode}).\n{detail[-1500:]}")
    return r


def setup_keys(work: Path, prod_keys: Path, title_keys: Path | None) -> tuple[Path, Path]:
    home = work / "home"
    switch = home / ".switch"
    switch.mkdir(parents=True)
    dest = switch / "prod.keys"
    shutil.copy2(prod_keys, dest)
    tk_path = switch / "title.keys"
    if title_keys and title_keys.is_file():
        shutil.copy2(title_keys, tk_path)
    elif not tk_path.exists():
        tk_path.write_text("", encoding="utf-8")
    return home, dest


def parse_ticket(tik: Path) -> tuple[str, str] | None:
    """Return (rights_id_hex, encrypted_titlekey_hex) from a common ticket. hactool 1.4 has no -t tik."""
    data = tik.read_bytes()
    if len(data) < 0x2B0:
        return None
    # Common ticket: titlekey @ 0x180 (16), rights_id @ 0x2A0 (16)
    enc = data[0x180:0x190].hex()
    rights = data[0x2A0:0x2B0].hex()
    if rights.strip("0") == "" or enc.strip("0") == "":
        return None
    return rights.lower(), enc.lower()


def import_tickets(hactool: Path, keys: Path, home: Path, nsp_dir: Path, sample_nca: Path | None) -> str | None:
    """Append ticket titlekeys to title.keys and return the first encrypted titlekey.

    hactool's title.keys values are the *encrypted* titlekey (it runs titlekek itself).
    Writing the decrypted key makes basenca merges decrypt the base NCA with the wrong key.
    """
    tk_path = home / ".switch" / "title.keys"
    nca = sample_nca
    if nca is None:
        ncas = sorted(nsp_dir.glob("*.nca"), key=lambda p: p.stat().st_size, reverse=True)
        nca = ncas[0] if ncas else None
    if nca is None:
        return None
    first_enc: str | None = None
    with tk_path.open("a", encoding="utf-8") as tk:
        for tik in nsp_dir.glob("*.tik"):
            parsed = parse_ticket(tik)
            if not parsed:
                continue
            rights, enc = parsed
            if first_enc is None:
                first_enc = enc
            # Probe that prod.keys can unwrap this ticket (optional sanity check).
            r = run(hactool, keys, home, f"--titlekey={enc}", "-t", "nca", str(nca), check=False)
            text = (r.stdout or "") + (r.stderr or "")
            ok = any("titlekey (decrypted)" in line.lower() for line in text.splitlines())
            if ok:
                tk.write(f"{rights}={enc}\n")
                print(f"  imported titlekey for {rights[:16]}…")
            else:
                print(f"  warning: could not verify titlekey for {rights[:16]}…")
    return first_enc


def unpack_nsp(hactool: Path, keys: Path, home: Path, nsp: Path, outdir: Path) -> str | None:
    with nsp.open("rb") as f:
        if f.read(4) != b"PFS0":
            raise ExtractError(f"Not a valid NSP (missing PFS0 header):\n{nsp}")
    outdir.mkdir(parents=True, exist_ok=True)
    # hactool accepts --outdir=path (equals form is reliable with spaces)
    run(hactool, keys, home, "-t", "pfs0", f"--outdir={outdir}", str(nsp))
    # Prefer Program NCA as the sample for titlekey probe (largest .nca that isn't meta)
    sample = None
    for nca in sorted(outdir.glob("*.nca"), key=lambda p: p.stat().st_size, reverse=True):
        if "cnmt" not in nca.name.lower():
            sample = nca
            break
    return import_tickets(hactool, keys, home, outdir, sample)


def nca_info(hactool: Path, keys: Path, home: Path, nca: Path) -> tuple[str, int]:
    r = run(hactool, keys, home, "-t", "nca", str(nca), check=False)
    text = (r.stdout or "") + (r.stderr or "")
    ctype, tid = "", 0
    for line in text.splitlines():
        if line.lower().startswith("content type"):
            ctype = line.split(":", 1)[1].strip()
        if line.lower().startswith("title id"):
            try:
                tid = int(line.split(":", 1)[1].strip(), 16)
            except ValueError:
                pass
    return ctype, tid


def find_program_and_meta(hactool: Path, keys: Path, home: Path, folder: Path) -> tuple[Path, Path | None]:
    program = meta = None
    program_size = -1
    for nca in folder.glob("*.nca"):
        ctype, _ = nca_info(hactool, keys, home, nca)
        size = nca.stat().st_size
        if "Meta" in ctype or "cnmt" in nca.name.lower():
            meta = nca
        if "Program" in ctype and size > program_size:
            program, program_size = nca, size
    if not program:
        # Fallback: largest non-cnmt NCA (titlekey may still be missing)
        candidates = [p for p in folder.glob("*.nca") if "cnmt" not in p.name.lower()]
        if candidates:
            program = max(candidates, key=lambda p: p.stat().st_size)
        else:
            raise ExtractError(f"No Program NCA found in:\n{folder}\nIs this the right NSP?")
    return program, meta


def read_cnmt(hactool: Path, keys: Path, home: Path, work: Path, meta: Path | None) -> tuple[int, int]:
    if not meta:
        return 0, 0
    sec = work / "cnmt_sec"
    if sec.exists():
        shutil.rmtree(sec)
    sec.mkdir()
    run(hactool, keys, home, "-t", "nca", f"--section0dir={sec}", str(meta), check=False)
    files = [p for p in sec.rglob("*") if p.is_file()]
    if not files:
        return 0, 0
    data = files[0].read_bytes()[:16]
    if len(data) < 16:
        return 0, 0
    # CNMT: title_id u64 @0, title_version u32 @8
    tid = struct.unpack_from("<Q", data, 0)[0]
    ver = struct.unpack_from("<I", data, 8)[0]
    return tid, ver


def extract_mhgu(
    base: Path,
    update: Path,
    keys: Path,
    out: Path,
    title_keys: Path | None = None,
    progress: ProgressFn | None = None,
) -> None:
    def say(msg: str) -> None:
        print(msg)
        if progress:
            progress(msg)

    for label, p in (("base NSP", base), ("update NSP", update), ("prod.keys", keys)):
        if not p.is_file():
            raise ExtractError(f"Missing {label}:\n{p}")
    if base.resolve() == update.resolve():
        raise ExtractError(
            "Base game and update are the same file.\n"
            "Pick the big base NSP (~12 GB, title …8000) and the small update NSP (~200 MB, title …8800)."
        )

    hactool = find_hactool()
    say(f"Using {hactool.name}")
    out.mkdir(parents=True, exist_ok=True)

    free = free_bytes(out)
    if free is not None and free < NEED_FREE:
        raise ExtractError(
            f"Not enough free space.\nNeed about 20 GB free (have {free / 1024**3:.1f} GB)."
        )

    work = Path(tempfile.mkdtemp(prefix="mhgu_nsp_"))
    try:
        home, keyfile = setup_keys(work, keys, title_keys)
        base_dir = work / "base_nsp"
        upd_dir = work / "upd_nsp"

        say("1/5 Unpacking base game NSP…")
        base_enc = unpack_nsp(hactool, keyfile, home, base, base_dir)
        say("2/5 Unpacking 1.4.0 update NSP…")
        upd_enc = unpack_nsp(hactool, keyfile, home, update, upd_dir)

        say("3/5 Checking this is MHGU 1.4.0…")
        base_prog, _ = find_program_and_meta(hactool, keyfile, home, base_dir)
        upd_prog, upd_meta = find_program_and_meta(hactool, keyfile, home, upd_dir)
        tid, ver = read_cnmt(hactool, keyfile, home, work, upd_meta)
        # Update CNMT title ID is usually …8800 (patch); base is …8000. Both are MHGU.
        if tid and not title_id_ok(tid):
            raise ExtractError(
                f"Wrong game.\nTitle ID {tid:016X}\n"
                f"Expected 0100770008DD8000 (base) or 0100770008DD8800 (update)."
            )
        if ver and ver != CNMT_VERSION:
            raise ExtractError(
                f"Wrong update version (v{ver}).\nNeed update 1.4.0 (v{CNMT_VERSION})."
            )
        if not base_enc or not upd_enc:
            raise ExtractError(
                "Could not read title keys from the NSP tickets.\n"
                "Make sure each NSP includes a .tik and that prod.keys can decrypt them."
            )

        exefs = out / "exefs"
        romfs = out / "romfs"
        if exefs.exists():
            shutil.rmtree(exefs)
        if romfs.exists():
            shutil.rmtree(romfs)
        exefs.mkdir()
        romfs.mkdir()

        say("4/5 Extracting game files (this can take a while)…")
        # ExeFS and Patch-RomFS must be separate hactool passes. Combining
        # --exefsdir + --romfsdir + --basenca often finishes with an empty exefs.
        say("  ExeFS (update)…")
        run(
            hactool, keyfile, home,
            f"--titlekey={upd_enc}", "-t", "nca", f"--exefsdir={exefs}", str(upd_prog),
        )
        if not (exefs / "main").is_file():
            listing = ", ".join(p.name for p in exefs.iterdir()) or "(empty)"
            raise ExtractError(
                "Update ExeFS extract finished but main is missing.\n"
                f"exefs contains: {listing}\n"
                "Check that the small NSP is the 1.4.0 update and prod.keys is correct."
            )
        # RomFS merge: do NOT pass --titlekey= on the CLI. A single CLI titlekey is
        # applied to both the update NCA and --basenca, which corrupts BKTR output.
        # title.keys (encrypted keys per rights id) supplies each NCA its own key.
        say("  RomFS (base + update merge)…")
        basenca = f"--basenca={base_prog}"
        r = run(
            hactool, keyfile, home,
            "-t", "nca", basenca, f"--romfsdir={romfs}", str(upd_prog),
            check=False,
        )
        romfs_ok = any(romfs.iterdir()) if romfs.is_dir() else False
        mfx = romfs / "nativeNX" / "system" / "app_shader" / "AppShaderPackage.mfx"
        mfx_ok = mfx.is_file() and mfx.read_bytes()[:3] == b"MFX"
        if r.returncode != 0 or not romfs_ok or not mfx_ok:
            say("  Retrying RomFS via full base extract, then update patch…")
            if romfs.exists():
                shutil.rmtree(romfs)
            romfs.mkdir()
            run(
                hactool, keyfile, home,
                f"--titlekey={base_enc}", "-t", "nca", f"--romfsdir={romfs}", str(base_prog),
            )
            run(
                hactool, keyfile, home,
                "-t", "nca", basenca, f"--romfsdir={romfs}", str(upd_prog),
            )
            if not (mfx.is_file() and mfx.read_bytes()[:3] == b"MFX"):
                raise ExtractError(
                    "RomFS merge finished but AppShaderPackage.mfx is missing or still encrypted.\n"
                    "Check prod.keys and that both NSPs include tickets."
                )

        say("5/5 Verifying build ID…")
        main_nso = exefs / "main"
        if not main_nso.is_file():
            listing = ", ".join(p.name for p in exefs.iterdir()) or "(empty)"
            raise ExtractError(
                "Extraction finished but exefs/main is missing.\n"
                f"exefs contains: {listing}\n\n"
                "Usually title keys did not apply. Use:\n"
                "• Base NSP named …8000 (~12 GB)\n"
                "• Update NSP named …8800 (~200 MB)\n"
                "• Your prod.keys (and optional title.keys)"
            )
        hdr = main_nso.read_bytes()[:0x48]
        want = bytes.fromhex("fb08f1d20fd1204f")
        if hdr[:4] != b"NSO0" or hdr[0x40:0x48] != want:
            raise ExtractError(
                "exefs/main is not MHGU 1.4.0\n(build id should start with FB08F1D20FD1204F)."
            )

        install_note = install_into_build_data(out)
        say(
            f"Done!\n\nFiles are in:\n{out}"
            + (f"\n\n{install_note}" if install_note else "")
            + (
                f"\n\nFrom-source next (MSYS2 UCRT64), if you still need to compile:\n"
                f"  make gen GAME={out.as_posix()}\n  make -j4 GAME={out.as_posix()}"
            )
        )
    finally:
        shutil.rmtree(work, ignore_errors=True)


def _link_or_copy_dir(src: Path, dest: Path) -> None:
    if dest.exists() or dest.is_symlink():
        if dest.is_dir() and not dest.is_symlink():
            shutil.rmtree(dest)
        else:
            dest.unlink()
    if os.name == "nt":
        r = subprocess.run(
            ["cmd", "/c", "mklink", "/J", str(dest), str(src)],
            capture_output=True,
            text=True,
        )
        if r.returncode != 0:
            shutil.copytree(src, dest)
    else:
        dest.symlink_to(src, target_is_directory=True)


def install_into_build_data(game: Path) -> str:
    """If a mhgurecomp build/release is present, ensure its data/ points at the extracted dump."""
    if not (game / "exefs" / "main").is_file() or not (game / "romfs").is_dir():
        return ""
    root = repo_root()
    exe_name = "mhgurecomp.exe" if os.name == "nt" else "mhgurecomp"
    candidates = [
        (root / "build" / exe_name, root / "build" / "data"),
        (root / exe_name, root / "data"),
    ]
    notes: list[str] = []
    for exe, data in candidates:
        if not exe.is_file():
            continue
        # Already extracted straight into this data/ folder.
        if game.resolve() == data.resolve():
            notes.append(f"Ready to start:\n  {exe}")
            continue
        try:
            data.mkdir(parents=True, exist_ok=True)
            for name in ("exefs", "romfs"):
                _link_or_copy_dir((game / name).resolve(), data / name)
            (data / ".complete").touch()
            notes.append(f"Also installed into:\n{data}\n\nYou can start:\n  {exe}")
        except OSError as e:
            notes.append(f"(Could not install into {data}: {e})")
    return "\n\n".join(notes)


def run_gui() -> None:
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk

    win = tk.Tk()
    win.title("MHGU Recomp — Extract from NSP")
    win.geometry("640x420")
    win.minsize(520, 380)

    root = repo_root()
    # Release zip: extract straight into data/ next to mhgurecomp. Source tree: mhgu_game/.
    exe_name = "mhgurecomp.exe" if os.name == "nt" else "mhgurecomp"
    default_out = root / "data" if (root / exe_name).is_file() else root / "mhgu_game"
    paths: dict[str, tk.StringVar] = {
        "base": tk.StringVar(),
        "update": tk.StringVar(),
        "keys": tk.StringVar(),
        "title": tk.StringVar(),
        "out": tk.StringVar(value=str(default_out)),
    }
    status = tk.StringVar(value="Pick your files, then click Extract.")
    busy = {"on": False}

    def browse_file(key: str, title: str, patterns: list[tuple[str, str]]) -> None:
        p = filedialog.askopenfilename(parent=win, title=title, filetypes=patterns + [("All files", "*.*")])
        if p:
            paths[key].set(p)

    def browse_dir(key: str, title: str) -> None:
        p = filedialog.askdirectory(parent=win, title=title)
        if p:
            paths[key].set(p)

    def set_status(msg: str) -> None:
        win.after(0, lambda: status.set(msg))

    def finish_ok(msg: str) -> None:
        busy["on"] = False
        extract_btn.configure(state=tk.NORMAL)
        status.set(msg)
        messagebox.showinfo("Done", msg, parent=win)

    def finish_err(msg: str) -> None:
        busy["on"] = False
        extract_btn.configure(state=tk.NORMAL)
        status.set("Failed.")
        messagebox.showerror("Extract failed", msg, parent=win)

    def do_extract() -> None:
        if busy["on"]:
            return
        base = Path(paths["base"].get().strip())
        update = Path(paths["update"].get().strip())
        keys = Path(paths["keys"].get().strip())
        out = Path(paths["out"].get().strip())
        title_s = paths["title"].get().strip()
        title_keys = Path(title_s) if title_s else None

        if not base.is_file() or not update.is_file() or not keys.is_file():
            messagebox.showwarning(
                "Missing files",
                "Please select:\n• Base game NSP\n• 1.4.0 update NSP\n• prod.keys",
                parent=win,
            )
            return
        if not str(out):
            messagebox.showwarning("Missing folder", "Choose where to put exefs and romfs.", parent=win)
            return

        busy["on"] = True
        extract_btn.configure(state=tk.DISABLED)
        status.set("Starting…")

        def worker() -> None:
            try:
                extract_mhgu(base, update, keys, out, title_keys, progress=set_status)
                install = install_into_build_data(out)
                extra = f"\n\n{install}" if install else (
                    f"\n\nThen in MSYS2 UCRT64:\n"
                    f"make gen GAME={out.as_posix()}\n"
                    f"make -j4 GAME={out.as_posix()}"
                )
                finish_ok(f"Extraction finished.\n\nOutput:\n{out}{extra}")
            except ExtractError as e:
                finish_err(str(e))
            except Exception:
                finish_err(traceback.format_exc()[-2000:])

        threading.Thread(target=worker, daemon=True).start()

    pad = {"padx": 12, "pady": 4}
    frm = ttk.Frame(win, padding=12)
    frm.pack(fill=tk.BOTH, expand=True)

    ttk.Label(
        frm,
        text="Extract Monster Hunter Generations Ultimate 1.4.0 from NSP files.\n"
        "You need the base game NSP, the 1.4.0 update NSP, and your own prod.keys.\n"
        "About 20 GB free space is required. Original NSPs are not changed.",
        justify=tk.LEFT,
    ).pack(anchor=tk.W, pady=(0, 10))

    rows = ttk.Frame(frm)
    rows.pack(fill=tk.X)

    def add_row(label: str, key: str, browse) -> None:
        r = ttk.Frame(rows)
        r.pack(fill=tk.X, pady=3)
        ttk.Label(r, text=label, width=18).pack(side=tk.LEFT)
        ttk.Entry(r, textvariable=paths[key]).pack(side=tk.LEFT, fill=tk.X, expand=True, padx=6)
        ttk.Button(r, text="Browse…", width=10, command=browse).pack(side=tk.RIGHT)

    add_row("Base game NSP", "base", lambda: browse_file("base", "Select base game NSP", [("NSP", "*.nsp")]))
    add_row("1.4.0 update NSP", "update", lambda: browse_file("update", "Select 1.4.0 update NSP", [("NSP", "*.nsp")]))
    add_row("prod.keys", "keys", lambda: browse_file("keys", "Select prod.keys", [("Keys", "*.keys")]))
    add_row("title.keys (optional)", "title", lambda: browse_file("title", "Select title.keys (optional)", [("Keys", "*.keys")]))
    add_row("Output folder", "out", lambda: browse_dir("out", "Select output folder"))

    extract_btn = ttk.Button(frm, text="Extract", command=do_extract)
    extract_btn.pack(pady=14)

    ttk.Label(frm, textvariable=status, wraplength=600, justify=tk.LEFT).pack(anchor=tk.W, fill=tk.X)

    # hactool hint
    try:
        find_hactool()
    except ExtractError as e:
        status.set(str(e))

    win.mainloop()


def run_cli(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cli", action="store_true", help="Force command-line mode")
    ap.add_argument("--gui", action="store_true", help="Force GUI mode")
    ap.add_argument("--base", type=Path)
    ap.add_argument("--update", type=Path)
    ap.add_argument("--keys", type=Path)
    ap.add_argument("--title-keys", type=Path)
    ap.add_argument("--out", type=Path)
    args = ap.parse_args(argv)

    if args.gui or (not args.cli and not any([args.base, args.update, args.keys, args.out])):
        run_gui()
        return 0

    if not args.base or not args.update or not args.keys or not args.out:
        ap.error("--cli needs --base --update --keys --out")
    try:
        extract_mhgu(args.base, args.update, args.keys, args.out, args.title_keys)
    except ExtractError as e:
        print(e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    # Default: GUI. Use --cli for scripts.
    if len(sys.argv) == 1:
        try:
            run_gui()
        except Exception:
            print("GUI failed; use: python tools/extract_mhgu_nsp.py --cli ...", file=sys.stderr)
            traceback.print_exc()
            sys.exit(1)
    else:
        sys.exit(run_cli())
