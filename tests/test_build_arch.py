"""Build/release regression tests using public code and explicit header fixtures."""
import itertools
from pathlib import Path
import shlex
import subprocess
import sys
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def probe(*args):
    names = ("TARGET_ARCH", "TARGET_FLAGS", "CMAKE_TARGET_FLAGS",
             "FFMPEG_TARGET_FLAGS", "GENARCH", "GAMELIB_ID", "B", "O",
             "CC", "CXX", "PKG_CONFIG_LIBDIR", "PKG_CONFIG_SYSROOT_DIR",
             "UPD_LDFLAGS", "FFMPEG_CC", "ABI", "TRIPLE", "UPDATER",
             "GLIBC_WRAP_FLAGS", "RT_SRC", "MAKEFILE_LIST", "FFMPEG_CONF")
    recipe = "probe:\n\t@printf '%s\\n' " + " ".join(
        f"'{name}=$({name})'" for name in names)
    recipe += " 'GLIBC_CHECK_ENABLED=$(if $(value GLIBC_CHECK),yes,no)'\n"
    with tempfile.TemporaryDirectory(prefix="mhgu-config-") as tmp:
        fixture_args = []
        if "PLATFORM=android" in args:
            # Empty directories only. No SDK tools or compilation run here.
            sdk = Path(tmp) / "configuration-fixture-not-sdk"
            ndk = sdk / "ndk" / "fixture"
            (ndk / "toolchains/llvm/prebuilt/test-host/bin").mkdir(parents=True)
            (sdk / "cmake/fixture/bin").mkdir(parents=True)
            fixture_args = [f"ANDROID_SDK={sdk}", f"NDK={ndk}"]
        return subprocess.run(["make", "-s", "-f", "Makefile", "-f", "-",
                               "probe", *args, *fixture_args],
                              input=recipe, cwd=ROOT,
                              capture_output=True, text=True)


def elf_header(machine):
    # Deliberate header-only fixtures, not executable files or game contents.
    header = bytearray(64)
    header[:7] = b"\x7fELF\x02\x01\x01"
    header[16] = 3
    header[18:20] = machine.to_bytes(2, "little")
    header[20] = 1
    header[52] = 64
    return header


class BuildTests(unittest.TestCase):
    def test_defaults_and_aliases(self):
        for alias, expected in (("arm64", "aarch64"), ("aarch64", "aarch64"),
                                ("amd64", "x86_64"), ("x86_64", "x86_64")):
            result = probe(f"TARGET_ARCH={alias}")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(f"TARGET_ARCH={expected}\n", result.stdout)
            self.assertIn("B=build\nO=obj\n", result.stdout)
        result = probe("B=my-build", "O=my-objects")
        self.assertIn("B=my-build\nO=my-objects\n", result.stdout)

    def test_cross_flags_and_identity(self):
        ids = []
        for root in ("/tmp/sysroot-one", "/tmp/sysroot-two"):
            result = probe("HOST_ARCH=x86_64", "TARGET_ARCH=aarch64",
                           f"SYSROOT={root}")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("--target=aarch64-linux-gnu", result.stdout)
            self.assertIn(f"--sysroot={root}", result.stdout)
            self.assertIn("CMAKE_SYSTEM_PROCESSOR=aarch64", result.stdout)
            self.assertIn("--arch=aarch64", result.stdout)
            self.assertIn("-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld", result.stdout)
            self.assertIn("--extra-ldflags=-fuse-ld=lld", result.stdout)
            self.assertIn(f"PKG_CONFIG_SYSROOT_DIR={root}\n", result.stdout)
            self.assertNotIn("x86-64-v3", result.stdout)
            ids.append(next(x for x in result.stdout.splitlines()
                            if x.startswith("GAMELIB_ID=")))
        self.assertEqual(*ids)
        self.assertNotIn(ids[0], probe("TARGET_ARCH=x86_64").stdout)
        self.assertIn("TARGET_FLAGS=\n", probe("TARGET_ARCH=x86_64").stdout)
        self.assertIn("TARGET_FLAGS=\n", probe("PLATFORM=windows",
                                             "TARGET_ARCH=x86_64").stdout)

    def test_native_arm_and_compilers(self):
        result = probe("HOST_ARCH=aarch64", "TARGET_ARCH=aarch64")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("CMAKE_TARGET_FLAGS=\nFFMPEG_TARGET_FLAGS=\n", result.stdout)
        self.assertNotIn("--sysroot=", result.stdout)
        self.assertIn("CC=clang\nCXX=clang++\n", result.stdout)
        result = probe("CC=my-clang", "CXX=my-clang++")
        self.assertIn("CC=my-clang\nCXX=my-clang++\n", result.stdout)

    def test_unsupported_targets(self):
        self.assertNotEqual(probe("TARGET_ARCH=mips").returncode, 0)
        self.assertNotEqual(probe("PLATFORM=windows",
                                 "TARGET_ARCH=aarch64").returncode, 0)

    def test_android_configuration_does_not_inherit_desktop_flags(self):
        # This probes shared variables, not an Android build or an SDK.
        for host in ("x86_64", "aarch64"):
            for abi in ("arm64-v8a", "x86_64"):
                with self.subTest(host=host, abi=abi):
                    result = probe("PLATFORM=android", f"HOST_ARCH={host}",
                                   f"ABI={abi}", "SYSROOT=/desktop/sysroot")
                    self.assertEqual(result.returncode, 0, result.stderr)
                    for name in ("TARGET_FLAGS", "CMAKE_TARGET_FLAGS",
                                 "FFMPEG_TARGET_FLAGS", "GENARCH",
                                 "PKG_CONFIG_LIBDIR", "PKG_CONFIG_SYSROOT_DIR"):
                        self.assertIn(f"{name}=\n", result.stdout)
                    self.assertNotIn("--target=aarch64-linux-gnu", result.stdout)
                    self.assertNotIn("/desktop/sysroot", result.stdout)
                    values = dict(line.split("=", 1)
                                  for line in result.stdout.splitlines())
                    arch = "aarch64" if abi == "arm64-v8a" else "x86_64"
                    triple = f"{arch}-linux-android29"
                    self.assertIn("android/android.mk",
                                  values["MAKEFILE_LIST"].split())
                    self.assertNotIn("tools/desktop-arch.mk",
                                     values["MAKEFILE_LIST"].split())
                    self.assertEqual(values["ABI"], abi)
                    self.assertEqual(values["TRIPLE"], triple)
                    self.assertEqual(values["CC"], values["FFMPEG_CC"] +
                                     f" --target={triple} -fPIC")
                    self.assertEqual(values["CXX"], values["FFMPEG_CC"] +
                                     f"++ --target={triple} -fPIC")
                    self.assertEqual(values["B"], "build-android")
                    self.assertEqual(values["O"], f"obj-android/{abi}")
                    self.assertEqual(values["UPDATER"], "")
                    self.assertEqual(values["GLIBC_WRAP_FLAGS"], "")
                    self.assertNotIn("linux_glibc.c", values["RT_SRC"])
                    flags = shlex.split(values["FFMPEG_CONF"])
                    self.assertIn("--host-cc=clang", flags)
                    self.assertIn("--target-os=android", flags)
                    self.assertIn(f"--arch={arch}", flags)
                    self.assertIn(f"--extra-cflags=--target={triple}", flags)

    def test_glibc_check_is_linux_only(self):
        for platform in ("linux", "windows", "android"):
            with self.subTest(platform=platform):
                result = probe(f"PLATFORM={platform}", "TARGET_ARCH=x86_64")
                self.assertEqual(result.returncode, 0, result.stderr)
                expected = "yes" if platform == "linux" else "no"
                self.assertIn(f"GLIBC_CHECK_ENABLED={expected}\n", result.stdout)

    def test_ffmpeg_compiler_interface_preserves_target_flags(self):
        result = probe("HOST_ARCH=x86_64", "TARGET_ARCH=aarch64",
                       "SYSROOT=/desktop/sysroot", "CC=my-clang")
        self.assertEqual(result.returncode, 0, result.stderr)
        compiler = next(line for line in result.stdout.splitlines()
                        if line.startswith("FFMPEG_CC="))
        self.assertIn("FFMPEG_CC=my-clang --target=aarch64-linux-gnu", compiler)
        self.assertIn("--sysroot=/desktop/sysroot", compiler)
        result = probe("FFMPEG_CC=explicit-ffmpeg-cc")
        self.assertIn("FFMPEG_CC=explicit-ffmpeg-cc\n", result.stdout)

    def test_updater_system_sdl_keeps_libm(self):
        # SDL3=system does not add -lm through SDL_LIBS; the updater must.
        for arch in ("x86_64", "aarch64"):
            result = probe("PLATFORM=linux", f"TARGET_ARCH={arch}",
                           "SDL3=system")
            self.assertEqual(result.returncode, 0, result.stderr)
            flags = next(line.removeprefix("UPD_LDFLAGS=")
                         for line in result.stdout.splitlines()
                         if line.startswith("UPD_LDFLAGS=")).split()
            self.assertIn("-lSDL3", flags)
            self.assertIn("-lm", flags)
            self.assertIn("-Wl,--wrap=sqrtf", flags)

    def test_updater_headers(self):
        for machine, flags in ((62, []), (183, []), (62, ["-D_WIN32"])):
            with tempfile.TemporaryDirectory() as tmp:
                exe = str(Path(tmp) / "test")
                subprocess.run(["clang", "-std=c11", "-Wall", "-Wextra",
                                "-Werror", f"-DMHGU_TARGET_MACHINE={machine}",
                                *flags, "-I.", "tests/test_update_arch.c",
                                "-o", exe],
                               cwd=ROOT, check=True)
                subprocess.run([exe], check=True)
        # The new name must also be safe for *old* deployed updaters.
        assets = ["Linux-MHGURecomp.1.2.3.tar.gz",
                  "LinuxARM64-MHGURecomp.1.2.3.tar.gz",
                  "Windows-MHGURecomp.1.2.3.zip"]
        for order in itertools.permutations(assets):
            chosen = [x for x in order if x.startswith(("Linux-", "ArchLinux-"))]
            self.assertEqual(chosen, [assets[0]])

    def test_dist_architecture_and_legacy_invocation(self):
        for arch, machine, top in (
            ("aarch64", 183, "LinuxARM64-MHGURecomp"),
            (None, 62, "Linux-MHGURecomp"),
        ):
            with tempfile.TemporaryDirectory() as tmp:
                build = Path(tmp) / "build"
                build.mkdir()
                (build / "licenses").mkdir()
                (build / "licenses/LICENSE.txt").write_text("test fixture")
                (build / "pipelines.list").write_text("test fixture")
                for name in ("mhgurecomp", "mhgugame.so", "mhgurecomp-updater"):
                    content = elf_header(machine)
                    if name.endswith("updater"):
                        content += b"MHGURecomp-updater/1.2.3\0"
                    (build / name).write_bytes(content)
                cmd = [sys.executable, str(ROOT / "tools/dist.py"),
                       str(build), tmp, "linux", "1.2.3"]
                if arch:
                    cmd += ["--arch", arch]
                result = subprocess.run(cmd, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                archive = Path(tmp) / f"{top}.1.2.3.tar.gz"
                with tarfile.open(archive) as tar:
                    self.assertEqual(tar.getmember(f"{top}/mhgurecomp").mode, 0o755)
                    self.assertNotIn(f"{top}/data/exefs/main", tar.getnames())
                (build / "mhgugame.so").write_bytes(elf_header(183 if machine == 62 else 62))
                self.assertNotEqual(subprocess.run(cmd, capture_output=True).returncode, 0)
                (build / "mhgugame.so").write_bytes(elf_header(machine)[:20])
                self.assertNotEqual(subprocess.run(cmd, capture_output=True).returncode, 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
