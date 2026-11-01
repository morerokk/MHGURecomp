# Building for Linux ARM64

These instructions accompany the Linux ARM64 port, now integrated with
MHGU Recomp 0.12.0. The manual recipe was validated on 0.11.8.
For the container path, see [Build both Linux packages](BUILD-LINUX-DOCKER.md).
Run commands
from the repository root unless a section says otherwise. Use Bash and paths
without spaces for the checkout, sysroot, build and object directories.

You need your own complete Monster Hunter Generations Ultimate 1.4.0 dump.
No game files, keys, generated game source or game library are provided.
This targets **AArch64 Linux with glibc**, not Android.

## Choose a build path

- **Native AArch64 Linux:** install the Linux development dependencies in
  the [manual Linux guide](BUILD-LINUX-MANUAL.md#native-linux-dependencies), including Clang/LLD, LLVM tools, CMake,
  Python, pkg-config, VMA and the Vulkan/SDL desktop development files.
  Prepare Python and VMA as below. Run `make gen`, then
  `make -j4 JOBS=2 TARGET_ARCH=aarch64 B=build-arm64 O=obj-arm64 GAME=...`.
  No `SYSROOT` or QEMU runner is needed. Native compilation has not been
  validated for this patch; only its Makefile configuration has been checked.
  A newer distribution can exceed the glibc/libstdc++ ceilings below.
- **Cross-build on x86-64 Linux:** follow the remaining sections. Host
  programs execute on x86-64; target headers, libraries and pkg-config
  metadata come from an external ARM64 sysroot. Never add the sysroot's
  `usr/bin` to `PATH`. The Makefile builds the bundled libraries on the host
  for the target; it does not prepare a sysroot.

The cross-build recipe was validated by a contributor on Fedora 44 KDE
x86-64 with Clang/LLD 22.1.8, CMake 4.3.0, Python 3.14.8 and QEMU 10.2.2.
It produced the runtime, a real game library and updater. Execution on an
Odin 3 was contributor-confirmed, not an exhaustive device test.

## 1. Host tools and source prerequisites

On a traditional, non-OSTree Fedora host:

```bash
sudo dnf install git make cmake pkgconf-pkg-config wayland-devel podman \
  clang lld llvm gcc-c++ python3-pip qemu-user curl tar xz file binutils
```

`wayland-scanner` must run on the host. Target Wayland headers and protocols
belong in the sysroot. Podman is optional if you already have a compatible
sysroot. Other distributions need equivalent host packages; this is not a
package recipe for immutable Fedora variants. `nasm` is optional for x86
FFmpeg builds, not required for this ARM64 recipe.

Use an isolated Python environment; the Makefile selects it automatically:

```bash
python3 -m venv tools/venv
tools/venv/bin/python -m pip install -r tools/requirements.txt
tools/venv/bin/python -m pip check
tools/venv/bin/python -c 'import lz4.block, capstone, unicorn'
```

Obtain VMA 3.4.0 from its official tagged source. The header is a local
prerequisite, not a file to add to this PR:

```bash
mkdir -p third_party/vk_mem_alloc
curl --fail --location --proto '=https' \
  https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/v3.4.0/include/vk_mem_alloc.h \
  -o third_party/vk_mem_alloc/vk_mem_alloc.h
printf '%s  %s\n' \
  8487b7995ad3b263eb73bc5b9a77d71aa69b6bef5d58a715c02d2663afd81f1a \
  third_party/vk_mem_alloc/vk_mem_alloc.h | sha256sum -c -
```

## 2. Prepare an external sysroot

Skip to section 3 if a compatible sysroot is already available. It needs
glibc startup files, GCC 13 target runtimes and C++ headers, plus the
development packages below, with Ubuntu's multiarch layout:
`usr/lib/aarch64-linux-gnu` and `usr/lib/gcc/aarch64-linux-gnu/13`.
The release ceilings are glibc 2.39 and GLIBCXX_3.4.32.

### Host: create a dedicated, empty work directory

Keep the same host shell for the later sections. Do not reuse a partly
extracted sysroot or mix packages from different distributions.

```bash
set -euo pipefail
export ARM_WORK="$HOME/.local/share/mhgu-arm64-0118"
export ARM_SYSROOT="$ARM_WORK/sysroot"
test ! -e "$ARM_WORK"
mkdir -p "$ARM_SYSROOT" "$ARM_WORK/apt/lists/partial" \
  "$ARM_WORK/apt/archives/partial"
export UBUNTU_IMAGE='docker.io/library/ubuntu@sha256:78d4744ead1f5b19b04d0df558ab182a5300903900755e515b2ca4d10eb2044d'
podman info --format '{{.Host.Security.Rootless}}'
podman run --rm -it --platform linux/amd64 \
  -v "$ARM_WORK:/work:Z" "$UBUNTU_IMAGE" bash
```

The rootless check must print `true`. The auxiliary Ubuntu container is
**amd64**, not an emulated ARM container. Do not use `sudo podman`,
`--privileged`, or mount your whole home directory. On SELinux hosts,
`:Z` relabels only this dedicated work directory. The image digest records
the image used in the validated recipe; Ubuntu package repositories still
change, so keep the package manifests too.

### Container: download signed ARM64 packages, then extract them

The following blocks run **inside the temporary container**. The first
APT command installs only the host container's HTTPS certificates. The
isolated APT configuration then downloads ARM64 packages without installing
them or running their maintainer scripts.

```bash
set -euo pipefail
test "$(dpkg --print-architecture)" = amd64
test -r /usr/share/keyrings/ubuntu-archive-keyring.gpg
touch /work/.write-test
rm /work/.write-test
apt-get update
apt-get install -y --no-install-recommends ca-certificates
cat > /work/apt/sources.list <<'SOURCES'
deb [arch=arm64 signed-by=/usr/share/keyrings/ubuntu-archive-keyring.gpg] https://ports.ubuntu.com/ubuntu-ports noble main universe
deb [arch=arm64 signed-by=/usr/share/keyrings/ubuntu-archive-keyring.gpg] https://ports.ubuntu.com/ubuntu-ports noble-updates main universe
deb [arch=arm64 signed-by=/usr/share/keyrings/ubuntu-archive-keyring.gpg] https://ports.ubuntu.com/ubuntu-ports noble-security main universe
SOURCES
cat > /work/apt/arm64.conf <<'APTCONF'
APT::Architecture "arm64";
APT::Architectures { "arm64"; };
APT::Sandbox::User "root";
Dir::Etc::sourcelist "/work/apt/sources.list";
Dir::Etc::sourceparts "-";
Dir::Etc::trustedparts "-";
Dir::State::lists "/work/apt/lists";
Dir::State::status "/work/apt/status";
Dir::Cache::archives "/work/apt/archives";
Acquire::Languages "none";
APTCONF
: > /work/apt/status
apt-get -c /work/apt/arm64.conf update
apt-cache -c /work/apt/arm64.conf policy \
  libc6-dev libgcc-13-dev libstdc++-13-dev
apt-get -c /work/apt/arm64.conf \
  --download-only --no-install-recommends -y install \
  libc6-dev libgcc-13-dev libstdc++-13-dev \
  zlib1g-dev libvulkan-dev libegl-dev \
  libwayland-dev wayland-protocols \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev \
  libxi-dev libxfixes-dev libxss-dev libxtst-dev libxkbcommon-dev \
  libasound2-dev libpulse-dev libpipewire-0.3-dev \
  libudev-dev libdbus-1-dev libdecor-0-dev libcurl4-openssl-dev
```

Do not disable signature verification. Do not replace the official keyring
with an unverified key. Stop if an update or download fails.

```bash
for deb in /work/apt/archives/*.deb; do
  dpkg-deb --show --showformat='${Package}\t${Version}\t${Architecture}\n' \
    "$deb"
done | sort > /work/packages.tsv
while IFS=$'\t' read -r package version arch; do
  case "$arch" in
    arm64|all) ;;
    *) printf 'Unexpected architecture: %s %s\n' "$package" "$arch"; exit 1 ;;
  esac
done < /work/packages.tsv
(
  cd /work/apt/archives
  sha256sum ./*.deb > /work/packages.sha256
  sha256sum -c /work/packages.sha256
)
test -z "$(find /work/sysroot -mindepth 1 -maxdepth 1 -print -quit)"
for deb in /work/apt/archives/*.deb; do
  dpkg-deb --extract "$deb" /work/sysroot
done
exit
```

Keep the `.deb` files, manifests and source/configuration files outside
the repository. In the validated download there were 212 packages
(192 `arm64`, 20 `all`), libc 2.39-0ubuntu8.9 and GCC target files
13.3.0-6ubuntu2~24.04.1; the extracted tree was about 492 MB. These are
observations, not assertions for future repository updates. Hashes record
the download; APT's signed metadata supplies its repository verification.
This is not a promise of bit-for-bit reproducibility.

### Host: make symlinks relocatable

Back in the **host shell**, retain `ARM_WORK` and `ARM_SYSROOT`. Debian
packages can contain absolute links. Convert all of them before auditing
chains, otherwise an intermediate absolute link can resolve on the host.
This one-off normalization is not a repository build tool.

```bash
python3 - "$ARM_SYSROOT" <<'PY'
import os
from pathlib import Path
import sys

root = Path(sys.argv[1]).resolve(strict=True)
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
converted = 0
for link in links:
    target = os.readlink(link)
    if os.path.isabs(target):
        destination = root / os.path.normpath(target).lstrip("/")
        relative = os.path.relpath(destination, link.parent)
        link.unlink()
        link.symlink_to(relative)
        converted += 1
for link in links:
    if not link.resolve().is_relative_to(root):
        raise SystemExit(f"Symlink escapes sysroot: {link}")
print(f"Converted {converted} absolute links; all links stay in sysroot")
PY
test -r "$ARM_SYSROOT/lib/ld-linux-aarch64.so.1"
test -r "$ARM_SYSROOT/usr/lib/aarch64-linux-gnu/crt1.o"
test -r "$ARM_SYSROOT/usr/lib/gcc/aarch64-linux-gnu/13/libstdc++.so"
```

Do not use this on your system root. In the validated tree, four absolute
links were converted.

## 3. Check the compiler and target dependencies

Use host Clang and LLD, not a compiler from the sysroot. These small
programs catch missing loaders, C++ headers or GCC runtime files before a
large build. They use the same target/sysroot selection as the Makefile.

```bash
mkdir -p "$ARM_WORK/pilots"
cat > "$ARM_WORK/pilots/check.c" <<'C'
#include <stdio.h>
int main(void) {
    puts("C ARM64: OK");
    return 0;
}
C
cat > "$ARM_WORK/pilots/check.cpp" <<'CPP'
#include <iostream>
#include <numeric>
#include <vector>
int main() {
    std::vector<int> values{1, 2, 3};
    if (std::accumulate(values.begin(), values.end(), 0) != 6) return 1;
    std::cout << "C++ ARM64: OK" << std::endl;
    return 0;
}
CPP
clang --target=aarch64-linux-gnu --sysroot="$ARM_SYSROOT" \
  --gcc-toolchain="$ARM_SYSROOT/usr" -fuse-ld=lld \
  "$ARM_WORK/pilots/check.c" -o "$ARM_WORK/pilots/check-c"
clang++ --target=aarch64-linux-gnu --sysroot="$ARM_SYSROOT" \
  --gcc-toolchain="$ARM_SYSROOT/usr" -fuse-ld=lld -v \
  "$ARM_WORK/pilots/check.cpp" -o "$ARM_WORK/pilots/check-cxx"
file "$ARM_WORK/pilots/check-c" "$ARM_WORK/pilots/check-cxx"
qemu-aarch64 -L "$ARM_SYSROOT" "$ARM_WORK/pilots/check-c"
qemu-aarch64 -L "$ARM_SYSROOT" "$ARM_WORK/pilots/check-cxx"
```

Both files must be AArch64 ELF executables and print their `OK` markers.
The verbose C++ output should select GCC from the sysroot, not host x86
headers. Inspect target pkg-config metadata in a **subshell** so this
environment does not affect other host builds:

```bash
(
  unset PKG_CONFIG_PATH
  export PKG_CONFIG_SYSROOT_DIR="$ARM_SYSROOT"
  export PKG_CONFIG_LIBDIR="$ARM_SYSROOT/usr/lib/aarch64-linux-gnu/pkgconfig:$ARM_SYSROOT/usr/lib/pkgconfig:$ARM_SYSROOT/usr/share/pkgconfig"
  for module in zlib libcurl vulkan wayland-client x11 xkbcommon alsa \
    libpulse libpipewire-0.3 libudev dbus-1 libdecor-0; do
    directory="$(pkg-config --variable=pcfiledir "$module")"
    case "$directory" in
      "$ARM_SYSROOT"/*) ;;
      *) printf 'Host metadata or missing module: %s\n' "$module"; exit 1 ;;
    esac
    printf '%s: ' "$module"
    pkg-config --modversion "$module"
  done
)
```

The Makefile applies this target pkg-config isolation when `SYSROOT` is
set. It still finds executable build tools on the host.

## 4. Generate and build

Set `GAME_DUMP` to your own folder containing `exefs/` and `romfs/`.
The required `main` build ID begins `FB08F1D20FD1204F`. The small check
below only checks that prefix and a few paths, not the integrity of the
entire dump. It does not explain how to obtain game files.

```bash
export GAME_DUMP=/absolute/path/to/your/game
test -r "$GAME_DUMP/exefs/main"
test -r "$GAME_DUMP/exefs/sdk"
test -r "$GAME_DUMP/romfs/nativeNX/system/app_shader/AppShaderPackage.mfx"
tools/venv/bin/python - "$GAME_DUMP/exefs/main" <<'PY'
from pathlib import Path
import sys
header = Path(sys.argv[1]).open("rb").read(0x48)
if header[:4] != b"NSO0" or header[0x40:0x48].hex().upper() != "FB08F1D20FD1204F":
    raise SystemExit("Unexpected main NSO header/build ID; check the 1.4.0 dump")
print("main build-ID prefix: OK")
PY
make gen GAME="$GAME_DUMP"
```

Review generation diagnostics; a successful exit does not imply every
instruction was supported. Before the full build, compile two real chunks:

```bash
make -j2 TARGET_ARCH=aarch64 SYSROOT="$ARM_SYSROOT" \
  B=build-arm64 O=obj-arm64 GAME="$GAME_DUMP" \
  obj-arm64/gen/main/main_000.o obj-arm64/gen/sdk/sdk_000.o
file obj-arm64/gen/main/main_000.o obj-arm64/gen/sdk/sdk_000.o
```

Then build all three programs. This is the contributor-tested command;
reduce `-j8` and `JOBS=4` for hosts with less memory:

```bash
make -j8 JOBS=4 TARGET_ARCH=aarch64 SYSROOT="$ARM_SYSROOT" \
  B=build-arm64 O=obj-arm64 GAME="$GAME_DUMP" \
  VERSION=0.11.8-arm64-pr all
file build-arm64/mhgurecomp build-arm64/mhgugame.so \
  build-arm64/mhgurecomp-updater
```

`-j` controls outer make jobs. `JOBS` limits **each** bundled dependency
subbuild (default: `nproc`), not a global process budget: subbuilds can
overlap. Start with `-j2 JOBS=2` when unsure. Do not run the build as root.

SDL3, shaderc and FFmpeg sources are downloaded and hash-checked by the
existing Makefile. Their first build takes more work than an incremental
build. The Makefile requires all `SDL3_NEED` backends; if configuration
fails, read the corresponding `obj-arm64/*/configure.log` and `build.log`.
Do not bypass missing backends just to obtain a binary.

Always use different `B`/`O` directories for another target, sysroot or
incompatible compiler/flag configuration; make does not track every flag
change. Generated C can be shared if the game and generator are unchanged.
Reuse objects for the same configuration; no `make clean` is needed after
a successful pilot. `all` only includes `mhgugame.so` when generated source
exists: check all three files, not only the runtime. The build folder can
also contain hard links or copies of your private game data.

## 5. Regression and differential tests

These tests need no game dump. Settings/panel tests may build bundled
dependencies on first use; locale coverage is a host Python test even
when invoked with ARM64 flags:

```bash
tools/venv/bin/python tests/test_build_arch.py
make -j2 JOBS=2 TARGET_ARCH=aarch64 SYSROOT="$ARM_SYSROOT" \
  B=build-arm64 O=obj-arm64 \
  TEST_RUNNER="qemu-aarch64 -L $ARM_SYSROOT" \
  test-linux-fault test-settings test-settings-panel test-settings-locale
```

Expect seven architecture/build test methods, the Linux fault test,
`PASS: INI round trip`, `settings panel: ... OK` and locale `OK`.
The fault test covers real lazy-page faults and fatal-signal context;
QEMU is not a Vulkan, audio, controller or gameplay test.

With your dump and Python requirements, run the default lifted-C
differential test against Unicorn:

```bash
mkdir -p "$ARM_WORK/differential-6000"
TEMP="$ARM_WORK/differential-6000" \
  make TARGET_ARCH=aarch64 SYSROOT="$ARM_SYSROOT" \
  B=build-arm64 O=obj-arm64 GAME="$GAME_DUMP" VERSION=0.11.8-arm64-pr \
  TEST_RUNNER="qemu-aarch64 -L $ARM_SYSROOT" test-lift
```

This selects 6000 instructions per module with the harness defaults
(four requested vectors, seed 1). Some vectors are skipped. Check both
module summaries for executed vectors, failing instructions and NaN
payload differences; the latter are reported separately and do not make
the exit status fail. The contributor observed:

| Module | Executed vectors | Failing instructions | NaN payload differences |
| --- | ---: | ---: | ---: |
| main | 21742 | 0 | 0 |
| sdk | 22193 | 0 | 0 |

This tests isolated lifted-C functions, not execution of the full built
game library. It excludes special instruction classes and skips some
Unicorn/memory cases; it does not cover all paths or generation's
unsupported diagnostics. Do not add `--interp` to reproduce this result.
That optional upstream standalone mode has unresolved runtime dependencies
on both tested architectures and is outside this patch.

## 6. Inspect imports and test on the device

The Makefile checks runtime/updater imports against glibc 2.39 and
GLIBCXX_3.4.32. Inspect **required undefined dynamic symbols**, not the
versions exported by the sysroot. Include the real game library:

```bash
for binary in build-arm64/mhgurecomp build-arm64/mhgugame.so \
  build-arm64/mhgurecomp-updater; do
  printf '\n%s\n' "$binary"
  symbols="$(llvm-objdump -T "$binary")" || exit 1
  for namespace in GLIBC GLIBCXX; do
    versions="$(
      printf '%s\n' "$symbols" |
        awk '/\*UND\*/ { print }' |
        grep -oE "${namespace}_[0-9]+(\.[0-9]+)+" |
        sort -Vu || true
    )"
    if test -n "$versions"; then
      printf '%s\n' "$versions" | tail -n 1
    else
      printf '%s: no versioned imports\n' "$namespace"
    fi
  done
done
```

The contributor's build required at most GLIBC 2.38 / GLIBCXX 3.4.32 for
the runtime, GLIBC 2.17 with no GLIBCXX imports for the real library, and
GLIBC 2.38 with no GLIBCXX imports for the updater. This does not test the
target GPU driver or all of its system libraries.

Transfer only the programs, real library, `licenses/` and `pipelines.list`
for a private device test; supply your own game data separately. Do not
archive the build folder indiscriminately: it can include `data/`, settings
or caches. On the **ARM64 device**, use the runtime directly at first
(no QEMU), from the copied program directory:

```bash
export DEVICE_TEST="$HOME/.local/share/mhgu-arm64-0118-test"
mkdir -p "$DEVICE_TEST/home" "$DEVICE_TEST/xdg-data" \
  "$DEVICE_TEST/xdg-config" "$DEVICE_TEST/xdg-cache"
MHGU_HOME="$DEVICE_TEST/home" XDG_DATA_HOME="$DEVICE_TEST/xdg-data" \
  XDG_CONFIG_HOME="$DEVICE_TEST/xdg-config" \
  XDG_CACHE_HOME="$DEVICE_TEST/xdg-cache" ./mhgurecomp
```

Keep `data/exefs` and `data/romfs` beside the programs. `MHGU_HOME` isolates
settings, logs and runtime caches; `XDG_DATA_HOME` isolates saves. Use a
fresh test directory if you need a fresh profile. Do not point this first
test at existing saves. Check graphics, input, sound, save/load and
stability before making broad compatibility claims. CPU architecture alone
does not guarantee the required Vulkan features or guest address space.

## Release and known validation limits

For a maintained release, the existing `make dist` target accepts the same
target/sysroot/build options and a **numeric release version**. It emits
`LinuxARM64-MHGURecomp.<version>.tar.gz`, never `Linux-aarch64-...`:
older x86 updaters accept any `Linux-` name. Keep Windows and x86 Linux
names unchanged. ARM has no fallback to an x86 asset.

Packaging/updating check ELF architecture for the runtime, game library
and updater. This is not a full executable-integrity or authenticity
verifier. Existing download size/SHA-256 checks remain in place.
Development versions such as `0.11.8-arm64-pr` retain the existing
no-auto-update behavior. Never publish your dump, generated game source,
game library or saves as part of a source PR.

Still unvalidated: native ARM64 compilation, native Windows/MSYS2,
a real distribution-provided shared SDL3, and a complete target-device
update/install/rollback flow. Contributor-confirmed Odin execution is not
a controlled performance, multiplayer or long-duration stability test.
