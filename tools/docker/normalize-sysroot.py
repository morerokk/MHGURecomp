"""Make package symlinks relocatable without resolving them against the host."""
import os
from pathlib import Path
import sys


def normalize(root):
    root = Path(root).resolve(strict=True)
    if root == Path("/"):
        raise ValueError("sysroot: refusing the system root")
    for name in ("lib", "lib64", "bin", "sbin"):
        link = root / name
        if not os.path.lexists(link) and (root / "usr" / name).is_dir():
            link.symlink_to("usr/" + name)
    links = []
    for directory, dirs, files in os.walk(root, followlinks=False):
        for name in dirs + files:
            link = Path(directory) / name
            if link.is_symlink():
                links.append(link)
    # Convert all absolute links before checking chains through other links.
    for link in links:
        target = os.readlink(link)
        if os.path.isabs(target):
            destination = root / os.path.normpath(target).lstrip("/")
            link.unlink()
            link.symlink_to(os.path.relpath(destination, link.parent))
    for link in links:
        if not link.resolve().is_relative_to(root):
            raise ValueError(f"sysroot: symlink escapes the root: {link}")


if __name__ == "__main__":
    normalize(sys.argv[1])
