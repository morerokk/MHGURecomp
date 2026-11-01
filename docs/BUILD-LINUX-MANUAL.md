# Manual Linux builds

Use this advanced guide for native Linux builds and WSL.
For the recommended container path, see [Build both Linux packages](BUILD-LINUX-DOCKER.md).
These instructions moved from the README.
They do not replace the detailed [ARM64 guide](BUILD-LINUX-ARM64.md).

Run build commands from the repository root.
Use your own complete MHGU 1.4.0 dump.
See the [source prerequisites](../README.md#building-from-source) before a manual build.
The Windows build can also run under Proton or Wine.

## Native Linux dependencies

The Makefile selects `runtime/platform/linux.c` on Linux.
The original native recipe was tested on Gentoo x86-64 with Clang, Wayland, and NVIDIA.

Install `make`, `git`, `clang`, `lld`, and Python 3.10 or newer.
You also need these dependencies:

- The Vulkan loader and Vulkan headers.
- `curl` and, optionally, `nasm` for the bundled FFmpeg build.
  You do not need a system FFmpeg package.
  See [FFmpeg](../README.md#ffmpeg).
- `cmake` and the desktop development files for SDL.
  These include X11, Xcursor, Xext, Xfixes, Xi, Xrandr, Xss, and Xtst.
  Also include Wayland, wayland-protocols, libxkbcommon, libdecor, PipeWire,
  PulseAudio, ALSA, libudev, and D-Bus.
- VMA 3.4.0: put `vk_mem_alloc.h` in `third_party/vk_mem_alloc/`.
  A distribution package can provide it on the compiler's default include path instead.
  The [ARM64 guide](BUILD-LINUX-ARM64.md#1-host-tools-and-source-prerequisites)
  has the official download and hash check.
- `zenity` or `kdialog` for fatal-error dialogs.
  Without either program, errors still appear in the terminal and `log.txt`.
- zlib headers to compile the updater.
  The updater loads `libcurl.so.4` at run time.
  Without libcurl, the updater starts the game without an update check.

The first build downloads pinned SDL3 and shaderc sources into `third_party/`.
It also downloads glslang and SPIRV-Tools.
The Makefile checks their SHA-256 hashes and links these libraries statically.
This permits builds for systems without SDL3, including Ubuntu 24.04.
The executable still needs glibc, libstdc++, and the Vulkan loader.
SDL loads the system's desktop and audio libraries at run time.

If a compatible system SDL3 is available, the game uses it.
Its version must match or exceed the bundled version.
The `[ui] SDL ...` entry in `log.txt` identifies the selected library.
Set `MHGU_SDL=bundled` to force the bundled version.
The first dependency build takes a few minutes.
The build stops if SDL cannot find a required backend (`SDL3_NEED`).

Use `make SDL3=system SHADERC=system` for distribution-style linking.
This selects `libSDL3.so.0` and `libshaderc_shared.so.1` from the system.
That build needs those libraries at run time.

## Build and run

Replace `../mhgu/game` with the folder that contains `exefs/` and `romfs/`.
The following commands retain the original manual recipe:

```bash
python3 -m venv tools/venv && tools/venv/bin/pip install -r tools/requirements.txt
make gen GAME=../mhgu/game       # as above: the folder holding exefs/ and romfs/
make -j8 GAME=../mhgu/game
cd build && ./mhgurecomp
```

The `build/` folder contains `mhgurecomp`, `mhgugame.so`, and `data/`.
The game library contains the recompiled game code.
The build uses hard links for the game files under `data/`.
The game writes settings, caches, and `mods/` beside the executable.
Use `MHGU_HOME=<folder>` to put those files elsewhere.

Saves use `$XDG_DATA_HOME/MHGURecomp/save`.
The default is `~/.local/share/MHGURecomp/save`.
The Windows equivalent is under `%APPDATA%`.
Both use the same save layout.
To transfer a save, copy `save_0`.
Back up saves before a test.

## WSL with Arch Linux

Install Arch Linux in WSL with `wsl --install -d archlinux`.
Use the Windows checkout under `/mnt/c/`.
Separate build and object folders keep the Windows build unchanged.
The package and header installation commands need root access inside WSL.

```bash
pacman -Syu --needed make git clang lld python cmake vulkan-headers vulkan-icd-loader nasm vulkan-swrast \
    libx11 libxcursor libxext libxfixes libxi libxrandr libxss libxtst wayland wayland-protocols libxkbcommon \
    libdecor pipewire libpulse alsa-lib systemd-libs dbus
mkdir -p /usr/local/include/vma && cp /mnt/c/msys64/ucrt64/include/vma/vk_mem_alloc.h /usr/local/include/vma/
make -j16 B=build-linux O=obj-linux     # in the repository, under /mnt/c/...
```

The original recipe uses MSYS2's VMA 3.4.0 header.
It does not assume an Arch VMA package.
Run the `make` command as your normal user after dependency installation.
Prepare Python and generate the game code as in the native recipe.

You can link the dump instead of making a second copy.
Create `build-linux/data/` first.
Use `ln -s /mnt/c/.../exefs build-linux/data/exefs`.
Create the equivalent link for `romfs`.
Then use `touch build-linux/data/.complete`.

The original WSLg test needed Mesa's software Vulkan driver.
WSL's dzn driver could not create the renderer's pipeline layouts.
Use `VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json` for that software driver.
The game runs slowly with software rendering.
In that test, WSLg could not capture the mouse.
Mouse clicks still worked as buttons, but screenshots were black.

## Linux ARM64

`TARGET_ARCH` defaults to the host architecture: `x86_64` or `aarch64`.
The aliases `amd64` and `arm64` also work.
Native ARM64 builds use the Linux dependencies above.
Manual cross-builds need host Clang/LLD, LLVM tools, CMake, Python,
pkg-config, and the Wayland scanner.
They also need an **external AArch64 Linux sysroot** with target headers and libraries.
Do not use the x86-64 host's `/usr` as the sysroot.

See [Building for Linux ARM64](BUILD-LINUX-ARM64.md) for the detailed Fedora x86-64 recipe.
It covers host packages, Python, VMA 3.4.0, compiler checks, tests, and device use.
It also covers optional rootless Podman preparation of an Ubuntu 24.04 ARM64 sysroot.
In that manual recipe, Podman prepares dependencies only.
The host Makefile compiles the programs.
An existing compatible sysroot does not need Podman.

```bash
export ARM_SYSROOT=/absolute/path/to/aarch64-sysroot
export GAME_DUMP=/absolute/path/to/game
python3 -m venv tools/venv
tools/venv/bin/python -m pip install -r tools/requirements.txt
make gen GAME="$GAME_DUMP"
make -j4 JOBS=4 TARGET_ARCH=aarch64 SYSROOT="$ARM_SYSROOT" \
    B=build-arm64 O=obj-arm64 GAME="$GAME_DUMP"
```

Use separate `B` and `O` directories for each architecture and sysroot.
Their defaults remain `build` and `obj`.
`JOBS` limits each bundled dependency build, not the total process count.
The `-j` option controls outer Make jobs.
ARM game code defaults to `-march=armv8-a`.
x86-64 game code retains `-march=x86-64-v3`.
CPU flags enter the game-library identity.
The absolute sysroot path does not.

The release ceilings remain glibc 2.39 and GLIBCXX_3.4.32.
A newer native distribution can exceed them.
This targets Linux with glibc, not Android.
The device still needs the renderer's Vulkan features and enough virtual address space.
QEMU regression tests do not validate graphics or gameplay.
Test the complete build on the target device before an ARM64 release.
