"""Check real build outputs against the release architecture and ABI ceilings."""
from pathlib import Path
import re
import subprocess
import sys


def check(path, arch):
    path = Path(path)
    with path.open("rb") as stream:
        header = stream.read(64)
    machine = {"x86_64": 62, "aarch64": 183}[arch]
    if not (len(header) == 64 and header[:7] == b"\x7fELF\x02\x01\x01"
            and int.from_bytes(header[18:20], "little") == machine):
        raise ValueError(f"build: wrong ELF architecture: {path.name}")
    symbols = subprocess.run(
        ["llvm-objdump", "-T", str(path)], check=True,
        capture_output=True, text=True).stdout
    ceilings = {"GLIBC": (2, 39), "GLIBCXX": (3, 4, 32)}
    for family, version in re.findall(
            r"\b(GLIBCXX|GLIBC)_([0-9]+(?:\.[0-9]+)+)", symbols):
        if tuple(map(int, version.split("."))) > ceilings[family]:
            raise ValueError(
                f"build: {path.name} requires {family}_{version}")
    print(f"build: {path.name}: {arch}, ABI checks passed")


if __name__ == "__main__":
    for filename in sys.argv[2:]:
        check(filename, sys.argv[1])
