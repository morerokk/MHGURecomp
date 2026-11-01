"""Container interface tests. The engine and dump files are explicit test fixtures."""
import fnmatch
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

# This fixture records arguments. It does not build images or compile game code.
ENGINE = r"""#!/usr/bin/env python3
import json
import os
from pathlib import Path
import sys

args = sys.argv[1:]
with open(os.environ["ENGINE_LOG"], "a") as stream:
    stream.write(json.dumps(args) + "\n")
while args and args[0].startswith("--"):
    option = args.pop(0)
    if option == "--host":
        args.pop(0)
if args[0] == "context":
    print(os.environ.get("ENGINE_ENDPOINT", "unix:///test/docker.sock"))
elif args[0] == "info":
    if Path(sys.argv[0]).name == "podman":
        print("crun" if any("OCIRuntime" in a for a in args) else
              os.environ.get("ENGINE_ROOTLESS", "true"))
    else:
        print('["name=rootless"]' if os.environ.get("ENGINE_ROOTLESS") == "true" else "[]")
elif args[:2] == ["image", "inspect"]:
    print("sha256:" + "a" * 64)
elif args[0] == "run":
    raise SystemExit(int(os.environ.get("ENGINE_EXIT", "0")))
"""


def load_module(name):
    spec = importlib.util.spec_from_file_location(
        name, ROOT / "tools/docker" / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class InterfaceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.repo = self.root / "checkout with spaces"
        self.repo.mkdir()
        (self.repo / "tools").mkdir()
        shutil.copy(ROOT / "tools/build-linux-release.sh", self.repo / "tools")
        (self.repo / ".gitignore").write_text(
            ".linux-release/\ndist/\n")
        self.git("init", "-q")
        self.git("add", ".")
        self.git("-c", "user.name=Test Fixture",
                 "-c", "user.email=fixture@example.invalid",
                 "commit", "-qm", "Explicit test fixture")
        self.game = self.root / "private dump fixture"
        (self.game / "exefs").mkdir(parents=True)
        (self.game / "romfs").mkdir()
        for name in ("main", "sdk"):
            (self.game / "exefs" / name).write_text("test fixture, not game code")
        self.bin = self.root / "bin"
        self.bin.mkdir()
        for name in ("docker", "podman"):
            engine = self.bin / name
            engine.write_text(ENGINE)
            engine.chmod(0o755)
        self.log = self.root / "engine.jsonl"
        self.env = os.environ.copy()
        for name in ("DOCKER_HOST", "DOCKER_CONTEXT", "CONTAINER_HOST",
                     "CONTAINER_CONNECTION", "CONTAINER_ENGINE"):
            self.env.pop(name, None)
        self.env.update(PATH=str(self.bin) + os.pathsep + self.env["PATH"],
                        ENGINE_LOG=str(self.log))

    def git(self, *args):
        return subprocess.run(["git", *args], cwd=self.repo, check=True,
                              capture_output=True, text=True)

    def run_script(self, *args):
        return subprocess.run(
            ["bash", str(self.repo / "tools/build-linux-release.sh"), *args],
            cwd=self.repo, env=self.env, capture_output=True, text=True)

    def release(self, *args):
        return self.run_script("--game", str(self.game), "--version", "0.12.0",
                               *args)

    def commands(self):
        return [json.loads(line) for line in self.log.read_text().splitlines()]

    def run_command(self):
        return next(command for command in self.commands() if "run" in command)

    def test_release_paths_context_and_private_mount(self):
        (self.repo / "untracked-private.txt").write_text("must not be exported")
        result = self.release()
        self.assertEqual(result.returncode, 0, result.stderr)
        build = next(c for c in self.commands() if "build" in c)
        self.assertEqual(build[-1], "tools")
        command = self.run_command()
        self.assertIn(str(self.game) + ":/game:ro,z", command)
        self.assertIn("--user", command)
        self.assertEqual(command[-4:], ["release", "0.12.0", "2", ""])
        exported = list((self.repo / ".linux-release").glob("*/src"))
        self.assertEqual(len(exported), 1)
        self.assertTrue((exported[0] / "tools/build-linux-release.sh").is_file())
        self.assertFalse((exported[0] / "untracked-private.txt").exists())

    def test_check_has_no_game_or_output_mount(self):
        result = self.run_script("--check", "aarch64", "--version", "0.12.0",
                                 "--skip-image-build")
        self.assertEqual(result.returncode, 0, result.stderr)
        command = self.run_command()
        self.assertFalse(any(":/game:" in value or ":/out:" in value
                             for value in command))
        self.assertEqual(command[-4:], ["check", "0.12.0", "2", "aarch64"])
        self.assertFalse(any("build" in c for c in self.commands()))

    def test_stale_cached_sources_do_not_survive_export(self):
        self.assertEqual(self.release().returncode, 0)
        exported = next((self.repo / ".linux-release").glob("*/src"))
        (exported / "stale-source.c").write_text("explicit stale-source fixture")
        self.assertEqual(self.release().returncode, 0)
        self.assertFalse((exported / "stale-source.c").exists())

    def test_bad_arguments_and_incomplete_dump(self):
        for args in ((), ("--version",), ("--unknown",),
                     ("--version", "0.12.0-test"),
                     ("--version", "0.12.0", "--jobs", "0"),
                     ("--check", "mips", "--version", "0.12.0"),
                     ("--check", "aarch64", "--version", "0.12.0",
                      "--game", str(self.game))):
            with self.subTest(args=args):
                self.assertNotEqual(self.run_script(*args).returncode, 0)
        (self.game / "exefs/sdk").unlink()
        self.assertNotEqual(self.release().returncode, 0)
        self.assertFalse(self.log.exists())

    def test_dirty_tracked_sources_fail_before_engine(self):
        with (self.repo / "tools/build-linux-release.sh").open("a") as stream:
            stream.write("\n# Explicit dirty-file fixture.\n")
        result = self.release()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("tracked changes", result.stderr)
        self.assertFalse(self.log.exists())

    def test_engine_failure_propagates(self):
        self.env["ENGINE_EXIT"] = "23"
        self.assertEqual(self.release().returncode, 23)

    def test_rootless_docker_does_not_override_user(self):
        self.env["ENGINE_ROOTLESS"] = "true"
        self.assertEqual(self.release().returncode, 0)
        self.assertNotIn("--user", self.run_command())

    def test_rootless_podman_keeps_host_user(self):
        self.env["CONTAINER_ENGINE"] = "podman"
        self.assertEqual(self.release().returncode, 0)
        command = self.run_command()
        self.assertIn("--remote=false", command)
        self.assertIn("--userns=keep-id", command)
        self.assertIn("--user", command)
        self.assertIn("keep-groups", command)

    def test_remote_docker_is_rejected(self):
        self.env["ENGINE_ENDPOINT"] = "ssh://remote.example.invalid"
        self.assertNotEqual(self.release().returncode, 0)
        self.assertFalse(any("run" in c for c in self.commands()))

    def test_explicit_context_takes_precedence_over_docker_host(self):
        self.env["DOCKER_CONTEXT"] = "remote-fixture"
        self.env["DOCKER_HOST"] = "unix:///test/local.sock"
        self.env["ENGINE_ENDPOINT"] = "tcp://remote.example.invalid:2375"
        self.assertNotEqual(self.release().returncode, 0)


class ImageTests(unittest.TestCase):
    def test_ffmpeg_cross_build_selects_native_host_compiler(self):
        for arch in ("x86_64", "aarch64"):
            for compiler in ("clang", "clang-18"):
                with self.subTest(arch=arch, compiler=compiler):
                    result = subprocess.run(
                        ["make", "--no-print-directory", "-s",
                         "--eval=print-ffmpeg-host-flags:;"
                         '@printf "%s\\n" $(FFMPEG_TARGET_FLAGS)',
                         "print-ffmpeg-host-flags",
                         "HOST_ARCH=x86_64", "TARGET_ARCH=" + arch,
                         "CC=" + compiler],
                        cwd=ROOT, check=True, capture_output=True, text=True)
                    flags = result.stdout.splitlines()
                    if arch == "aarch64":
                        self.assertIn("--host-cc=" + compiler, flags)
                        self.assertNotIn("--host-cc=" + compiler +
                                         " --target=aarch64-linux-gnu", flags)
                    else:
                        self.assertFalse(any(
                            flag.startswith("--host-cc=") for flag in flags))

    def test_context_allowlist(self):
        # The allowlist has no glob exceptions. No project tree enters the image.
        patterns = (ROOT / "tools/.dockerignore").read_text().splitlines()
        self.assertEqual(patterns[0], "**")
        allowed = {p[1:] for p in patterns[1:] if p.startswith("!")}
        self.assertEqual(allowed, {
            "requirements.txt", "docker/", "docker/Dockerfile",
            "docker/prepare-sysroot.sh", "docker/normalize-sysroot.py",
            "docker/build.sh", "docker/check-elf.py"})
        for private in ("generated/main/part.c", "../data/exefs/main",
                        "docker/game.nsp", "venv/bin/python"):
            self.assertFalse(any(fnmatch.fnmatch(private, p) for p in allowed))

    def test_sysroot_links_stay_inside_root(self):
        module = load_module("normalize-sysroot")
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "usr/lib").mkdir(parents=True)
            (root / "usr/lib/target").write_text("link fixture")
            (root / "usr/lib/absolute").symlink_to("/usr/lib/target")
            (root / "usr/lib/chain").symlink_to("absolute")
            module.normalize(root)
            self.assertEqual((root / "usr/lib/chain").resolve(),
                             root / "usr/lib/target")
            (root / "usr/lib/escape").symlink_to("../../../../outside")
            with self.assertRaises(ValueError):
                module.normalize(root)
        with self.assertRaises(ValueError):
            module.normalize("/")

    def test_elf_rejects_wrong_machine_before_symbol_tool(self):
        module = load_module("check-elf")
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "header-only-fixture"
            path.write_bytes(b"\x7fELF" + bytes(60))
            with self.assertRaises(ValueError):
                module.check(path, "aarch64")

    def test_image_and_entrypoint_do_not_copy_or_package_dump(self):
        dockerfile = (ROOT / "tools/docker/Dockerfile").read_text()
        script = (ROOT / "tools/docker/build.sh").read_text()
        self.assertNotIn("COPY .", dockerfile)
        self.assertNotIn("--platform=linux/arm64", dockerfile)
        self.assertNotIn(" dist ", script)
        self.assertIn('make -j1 "${args[@]}" licenses pipelist', script)
        self.assertIn('build_target x86_64\n    build_target aarch64', script)

    def test_packager_rejects_missing_game_library_and_wrong_version(self):
        spec = importlib.util.spec_from_file_location(
            "dist", ROOT / "tools/dist.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp)
            for name in ("mhgurecomp", "mhgurecomp-updater"):
                (build / name).write_bytes(
                    b"explicit test fixture MHGURecomp-updater/0.12.0\0")
            with self.assertRaises(SystemExit):
                module.files(tmp, "linux")
            with self.assertRaises(SystemExit):
                module.check_version(tmp, "linux", "0.12.1")


if __name__ == "__main__":
    unittest.main(verbosity=2)
