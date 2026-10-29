# MHGU Recomp

A native PC port of **Monster Hunter Generations Ultimate** (Nintendo Switch, version 1.4.0), made by statically
recompiling the game's 32-bit ARM code to C and running it on a host runtime: Switch system services and the NVN
graphics API are reimplemented on Windows, with Vulkan for rendering.

**No game files are included.** You need your own copy of the game, dumped from your own Switch. This project does
not explain how to dump it and does not provide game files, keys or links to them.

Status: fully playable, but there may be some bugs. I've played a dozen hunts with no remaining issues. Multiplayer also works.

## What it adds over the Switch version

- Graphics settings (resolution, MSAA, FXAA, SMAA, Supersampling, VSync)
- Any widescreen aspect ratio (16:10, 16:9 and and anything wider than that)
- Frame rate cap anywhere between 30-240 fps
    - Bugfixes for speed-related issues that emulators normally have when using higher FPS mods/codes
- Supports almost every controller under the sun
- Experimental KB+M controls
- Optional: small monsters are properly synced between players in multiplayer
- Optional: monster HP scaling by playercount
- Fast-forward unskippable cutscenes by pressing A
- Optional: village quests are playable in multiplayer (press L/R at the quest counter), with appropriately re-scaled monster HP
- Mod support. The vast majority of mods work by dropping them into the `mods` folder, including Forge mods. (see [Mods](#mods))
- Cheat codes in the same format as Ryujinx and Atmosphère, in the `cheats` folder (see [Cheats](#cheats))
- Ingame PC Settings menu overlay (F1 to open, will be replaced with a real settings menu in the future)

## Requirements

- OS: Windows 10/11 64-bit, or Linux
- CPU: Any CPU from 2014 or newer (needs AVX2 support)
- GPU: Any GPU from 2016 or newer (needs Vulkan 1.2 support), and at least 1GB of VRAM
- RAM: 4GB
- The game files for Monster Hunter Generations Ultimate, version 1.4.0. You can extract these from your Switch or Ryujinx with patches pre-applied.
    - (Title ID `0100770008DD8000`, the `main` executable's build ID starts with `FB08F1D20FD1204F`)

## Installation

[Download the latest prebuilt release from the Releases page. Make sure you download the right ZIP file for your desired platform.](https://github.com/morerokk/MHGURecomp/releases)

You need the `exefs` and `romfs` folders from Monster Hunter Generations Ultimate 1.4.0. If you are coming from Ryujinx, you can right-click the game (make sure the 1.4.0 title update is applied), and Extract the game files with the patches pre-applied.

1. Extract the release zip anywhere, for example `C:\Games\MHGURecomp\`.
2. Put the game's `exefs` and `romfs` folders in the `data` subfolder. It should look like this:

   ```
   MHGURecomp\
     mhgurecomp.exe
     mhgugame.dll
     (other .dll files)
     data\
       exefs\
         main
         main.npdm
         rtld
         sdk
         subsdk0
       romfs\
         nativeNX\
         (the rest of the game's romfs)
   ```

   `exefs` must come from the 1.4.0 update, and `romfs` must be the base game's romfs with the update's files
   merged over it.
3. Start `mhgurecomp.exe`.

If the files are missing or the wrong version, a message box says so.

### Staying up to date

Releases may come out pretty often. To update automatically, start `mhgurecomp-updater.exe` (`mhgurecomp-updater` on Linux)
instead of `mhgurecomp.exe`: point your shortcut at it, or on a Steam Deck add it as the non-Steam game. It asks
GitHub for the latest release and, when that is newer than yours, offers to install it, then starts the game.
`mhgurecomp.exe` itself never checks or updates anything; starting it directly is always fine.

- It only ever installs the latest proper release: never a draft, a pre-release or an older version.
- The download is checked against the size and SHA-256 GitHub lists for it before anything is replaced.
- Your files stay as they are: `data\`, `mods\`, `cheats\`, `bonus\`, `cache\`, `shots\`, `mhgurecomp.ini`,
  `log.txt` and your saves. Only the program files (the exe, the updater, the DLLs / `.so`, `licenses\`) are replaced,
  and if that fails halfway the old ones are put back.
- Without an internet connection, or when GitHub's limit of 60 checks an hour from one address is reached, the game
  just starts.
- `Updates` in `[Application]` of `mhgurecomp.ini` (or the settings menu): `Ask` (default), `Auto` (install without
  asking) or `Off` (only start the game).
- Its window works with a mouse, keyboard or controller (A: choose, B: not now). What it did is in
  `update\updater.log`.

Older releases don't include the updater: if your folder has no `mhgurecomp-updater`, download the newest release by
hand once.

### Where things are

| What | Where |
| --- | --- |
| Settings | `mhgurecomp.ini` next to the exe (written with the defaults on first start; delete it to reset) |
| Save data | `%APPDATA%\MHGURecomp\save\` (kept apart from the game folder so updating the port can't touch it) |
| Log | `log.txt` next to the exe (look here first when something goes wrong) |
| Screenshots | `shots\` next to the exe (F12) |
| Mods | `mods\` next to the exe: one folder per mod (see below) |
| Cheats | `cheats\` next to the exe: `.txt` files with cheat codes (see below) |
| Shader cache | `cache\` next to the exe (safe to delete; the first minutes after that may stutter) |

To move an existing Switch save over, put its `system` and `system_backup` files in
`%APPDATA%\MHGURecomp\save\save_0\`.

## Controls

Almost every controller under the sun should work, as long as it has SDL support. With more than one connected (a Steam Deck with a controller plugged in, say), press a button on the one you want to use. If you really want to swap the face buttons on non-Nintendo controllers, open the .ini file and set `FaceButtons = Label`.

Keyboard and mouse support (not recommended):

| Key | Switch button |
| --- | --- |
| W A S D | left stick (hold Left Shift to walk) |
| Mouse | right stick (camera) |
| Left click / Right click | X / A |
| Space / R | B / Y |
| Q / E | L / R |
| Tab / T | ZL / ZR |
| Enter / Backspace | Plus / Minus |
| Arrow keys | d-pad |
| Ctrl / Alt | left / right stick click |

Click in the game window to capture the mouse for the camera; Escape lets it go (so does switching to another
window). `MouseSensitivity` in the ini's `[Mouse]` section sets its speed; the game's own camera options (speed,
inverted axes) apply to it too.

When the game asks for text (names, chat), the keyboard types: Enter confirms, Escape cancels, Ctrl+V pastes.

Other keys: F1 opens the settings menu, Alt+Enter or F11 switches between windowed and full screen, F12 takes a
screenshot, F7 cycles MSAA.

## Settings

Everything is in `mhgurecomp.ini`; each setting is explained in the file itself. The settings menu changes them in
the game: **F1**, or **Minus + Plus** on a controller (Back + Start on an Xbox controller). Frame rate, FOV, draw distance,
anti-aliasing, VSync, controls and volume apply as soon as you change them (Save keeps them; closing the menu
without saving puts them back); the settings marked * at the next start. Details:
[docs/SETTINGS_UI.md](docs/SETTINGS_UI.md). In short:

- `[Display]`: `Mode` (Borderless / Fullscreen / Windowed), `Resolution`, `RenderResolution` (the 3D resolution),
  `AspectRatio` (Auto follows the window: 16:10 and anything wider fill it, narrower gets 16:10 with bars), `FOV`
  (1.0 = the game's), `UIAnchor` (Center keeps the HUD in the middle 16:9 on an ultrawide screen, Edges spreads it
  to the sides)
- `[Anti-aliasing]`: `MSAA` (Off / 2x / 4x / 8x), `AntiAliasing` (Off / Game: GU's own FXAA / FXAA: FXAA 3.11, the
  default / SMAA: SMAA 1x / SMAAT2x: SMAA with the camera jittered and the last two frames combined along their motion,
  switchable in game like the rest; the older `FXAA` key still reads, On meaning FXAA 3.11), FXAA 3.11's `FXAAQuality` (1-5,
  Unreal's presets 10, 13, 15, 29 and 39; default 5), `FXAASubpix` (sub-pixel blur, 0-1; default 0),
  `FXAAEdgeThreshold` (default 0.063) and `FXAAEdgeThresholdMin` (default 0.0312), and SMAA's `SMAAQuality` (Low /
  Medium / High / Ultra, the default)
- `[Upscaler]`: `FSR` (Off / On, default Off), `FSRQuality` (NativeAA / Quality / Balanced / Performance /
  UltraPerformance), `Sharpness`, and `Upscaler` (FSR3, the default, or FSR4: needs an AMD RDNA 4 GPU and its driver, and an exe built with `make FSR4_DRIVER_PATCH=1`;
  falls back to FSR 3.1). FSR replaces MSAA, FXAA and SMAA while it is on. Details: `docs/FSR-PORT.md`, `docs/FSR4-PORT.md`
- `[Graphics]`: `DrawDistance` (how far away people in the villages and the Hubs are still drawn: 1 = the game's,
  2 = twice as far, 0.5 = half, 0 = no limit; up to 10)
- `[Performance]`: `FPS` (30 to 240), `VSync` (On / Off; above your monitor's refresh rate, turn VSync off), `ShaderPrewarm`
  (On / Off: translate all of the game's shaders in the background at startup, so fewer things are drawn black the
  first time they appear; the first start takes about 20 s for it, during the logos, and the cache folder needs about
  340 MB). `pipelines.list` next to the exe lists the shader combinations the common areas use; with the prewarm on,
  they are prepared in the background as well, so entering those areas for the first time no longer shows surfaces
  briefly black. Only made with MSAA Off; the rest is still prepared the first time it is needed. Recording more of
  it: [docs/PIPELINE_LIST.md](docs/PIPELINE_LIST.md).
- `[Controls]`: `FaceButtons` (Position / Label), `MenuConfirm` (the settings menu's confirm button),
  `BackgroundInput` (On / Off: the controller keeps working while the game's window is in the background, such as
  with a wiki in front of it; default On), `ComboInputWindow` (Lenient / Strict: how long the game waits for a
  second button for inputs like the Hunting Horn's X+A; Lenient, the default, as long as at 30 fps at any frame
  rate, Strict a fixed number of frames, shorter the higher the frame rate)
- `[Mouse]`: `MouseSensitivity` (1 = default, 0.1 to 10)
- `[Multiplayer]`: `Port`, `Join` (addresses of hosts outside your LAN), `Server` (hub server for internet hubs:
  empty = the default, `Off` = none, or `host[:port]`), `Key` (matchmaking key: only players with the same key see
  each other's hubs; empty = public), `SyncSmallMonsters`
- `[Monster HP]`: HP factors for 1 to 4 hunters in Hub quests, and per-quest exceptions (default: all 1, the
  original game)
- `[Village Quests]`: `VillageQuestsInHub` (default On), `HubHP` (default 2.0: Village quests posted in the Hub
  multiply their monsters' HP by this first, then by the `[Monster HP]` factor)
- `[Bonus Content]`: `BonusContent` (default On), see below
- `[Audio]`: `Volume` (0 to 100: the whole game), `MuteInBackground` (On / Off, default Off: no sound while the
  game's window is in the background)
- `[Application]`: `ConfirmExit` (ask before the window closes), and the settings menu's `Language` (English /
  Spanish), `ButtonIcons` (Xbox / Switch / PlayStation) and `MenuScale` (100 to 200 percent), and `Updates` (Ask /
  Auto / Off: what `mhgurecomp-updater` does, see "Staying up to date")

### Multiplayer

Over LAN or a VLAN, this should work out of the box.

Over the internet, through a hub server: hubs you create are listed on it and Search Hubs shows the hubs listed there, no port forwarding needed. To play only with your friends, all of you enter the same matchmaking key ("Matchmaking key" in the settings menu, or `Key =` in the .ini); without a key your hubs are public. The hub server is set with `Server =` ("Hub server" in the settings menu); anyone can run their own, see [server/README.md](server/README.md). If the hub server can't be reached, a notice at the top of the screen says so (your hub is then only visible on your local network); "Check hub server" in the settings menu tests the server and key you entered. Changes to both apply without a restart.

Without a hub server, the host needs to forward the right port (39500 TCP/UDP by default), the client needs to configure the host's IP address in their .ini file. (`Join = xxx.xxx.xxx.xxx`)

Choose the "Local Wireless" multiplayer options, as this port pretends to be a local nearby Switch with a wireless connection. The online options are unavailable and are unnecessary.

### Bonus content

By default, the `bonus` subfolder in releases contains a savegame with all the free online content (event quests, item packs, palicoes), which will be copied into your savegame when the game starts the 2nd time.

If you don't want this, set `BonusContent = Off` or don't provide a savegame in the `bonus` subfolder. You can also provide your own savegame instead if you want. Note that this port will not attempt to copy bonus content if such bonus content already exists.

### Mods

To install a mod, put the mod folder in `mods\`. Remove the folder to uninstall said mod. Mods are layered over the game at startup.
- romfs mods (files that replace or add game data) are found inside the mod's folder, including the
  `atmosphere\contents\0100770008DD8000\romfs` layout.
- When several mods ship the same game archive (such as `resident_eng.arc`), only the entries each mod changed from the
  game's are applied, so mods that change different files in it work together. If two mods change the same entry,
  the one later in load order wins, and the log says so ("changes ..., which an earlier mod changed too"). Mods load in
  alphabetical order. To choose the order, make `mods\load_order.txt` with one mod folder name per line: then only
  the mods listed load, in that order. The merged archives are kept in `cache\arcmerge\`.
- The **Mods** tab of the settings menu does this without editing files: it lists the mod folders, turns each on or
  off, moves them up or down, and writes `mods\load_order.txt` when Save is pressed (restart to apply; mods you add
  later load too, after the listed ones, until you turn them off). It also compares the mods and lists the ones that change the same game files (and which of
  the two is used), and can apply a suggested order, with the mods that change the most files first.
- exefs patches (`.pchtxt`, `.ips`) are applied when they change data. Patches to game code can't be applied, because
  the code is precompiled; the log lists them.
- Forge plugins (`.nro` in `romfs\nativeNX\plugins\`) are loaded. Forge's menu with the plugins' settings opens with
  Insert (or the key in Forge's own `config.ini`, if a mod includes Forge's files, which can also set `[log] level`);
  use the mouse, keyboard or controller in it. While it is open the game gets no controller input. Window positions
  are kept in `forge_imgui.ini`.
- You do not need any resolution, FPS cap or connection fix mods. These features are included with this port.

If you are newer to the game, I very strongly recommend the [Key Quest Markers](https://gamebanana.com/mods/727960) mod by PurplefinNeptuna. This marks out the Village and Hub quests you need to complete for the next tier directly in the quest menu, which means you no longer need a wiki to play the damn game.

**Good places to get mods:**

- [The MHGURecomp-Mods repo](https://github.com/Oddity-git/MHGURecomp-mods)
- [GameBanana](https://gamebanana.com/mods/games/17088)

### Cheats

Put `.txt` files with cheat codes in `cheats\`. Every cheat in them is on from the start. Codes made for Ryujinx or a
modded Switch work as they are, as long as they are for version 1.4.0. A file can hold any number of cheats, each
starting with its name in square brackets:

```
[Skill points x2]
04000000 000DFAB4 E1A00080
```

- To turn off a file without deleting it, start its name with `_`.
- Cheats that change the game's code also work: the port runs each changed function in an interpreter. That
  function runs slower, which only matters for functions the game calls very often.
- `log.txt` lists the cheats it loaded, and any it couldn't read.
- `extras\cheats\` in the source has two cheats that double the skill points from armor, weapon and talisman
  decorations, and talismans (with Double Talisman, talisman points go x4). `Skill points x2` doubles negative
  points too; `Skill points x2 (positive only)` leaves them as they are. Use only one of the two.
- `Valor Sheathe original damage reduction` sets the damage you take in a Valor Sheathe evade back to the game's
  own values (70% less for Blademasters, 85% for Gunners), for mods that change it, such as GU Rebalanced (50% / 65%).
  The rest of the mod stays as it is. To pick other values, see `docs/research/valor-sheathe.md`.

## Building from source

The build recompiles the game code from your own copy of the game. Nothing in this repository contains game code or
data, and the build never uploads anything.

### 1. Tools (Windows)

1. Install [MSYS2](https://www.msys2.org/) to `C:\msys64`.
2. In the **MSYS2 UCRT64** shell, install the compiler and libraries:

   ```bash
   pacman -S --needed make git diffutils \
     mingw-w64-ucrt-x86_64-clang mingw-w64-ucrt-x86_64-lld mingw-w64-ucrt-x86_64-pkgconf \
     mingw-w64-ucrt-x86_64-sdl3 mingw-w64-ucrt-x86_64-shaderc mingw-w64-ucrt-x86_64-nasm \
     mingw-w64-ucrt-x86_64-vulkan-headers mingw-w64-ucrt-x86_64-vulkan-loader \
     mingw-w64-ucrt-x86_64-vulkan-memory-allocator
   ```

   FFmpeg (for the game's movies) isn't installed: the first build downloads its source and builds the small part the
   movies need into the executable (see "FFmpeg" below).

3. Install [Python](https://www.python.org/) 3.10 or newer for Windows. Then, in a Command Prompt in the repository
   folder, make the tools' virtual environment (the Makefile uses `tools\venv` when it exists):

   ```bat
   py -3 -m venv tools\venv
   tools\venv\Scripts\pip install -r tools\requirements.txt
   ```

### 2. Recompile and build

From the UCRT64 shell, in the repository folder, with your game files in a folder holding `exefs\` and `romfs\` (as
under "Installing a release"):

```bash
make gen GAME=/c/path/to/game      # translate the game code to C in generated/ (~300 MB)
make -j4 GAME=/c/path/to/game      # compile everything into build/
```

The first full build takes a while (about 8 minutes on a 4-core Haswell with `-j4`; more cores: higher `-j`). Later
builds only recompile what changed.

`make` turns `build\` into a ready-to-run game folder, and puts nothing else there (the compiler's own files go
to `obj\`):
- `mhgurecomp.exe`, the runtime;
- `mhgugame.dll`, the recompiled game code;
- the MSYS2 DLLs they need;
- `data\exefs` and `data\romfs`, hard links to your game files, so they take no extra disk space.

To play from another folder, copy the contents of `build\` there; to update that folder, copy them over it (`data\`
can be skipped when it is already there). The build never contains settings, caches, mods or logs, so your
`mhgurecomp.ini` and the rest stay as they are. Or start `run-game.bat`: it runs `build\mhgurecomp.exe` with the
settings, caches, `mods\` and `log.txt` in `run\` (`MHGU_HOME`), keeping `build\` clean. Starting
`build\mhgurecomp.exe` directly also works, but then it writes those into `build\`.

Useful targets and options:

- `make test-lift`: checks the recompiler against the Unicorn CPU emulator on thousands of the game's instructions.
- `make clean`: removes the compiled files (`obj\` and the program files in `build\`). It keeps `build\data`.
- `GENARCH=-march=x86-64-v2`: compiles the game code without AVX2, for older CPUs. Untested.
- `FFMPEG=system`: links the system's FFmpeg libraries instead of building them (see below).

### FFmpeg

The game's movies are MP4 files with H.264 video and AAC audio. The first build downloads FFmpeg's source
(`third_party/ffmpeg/`, a pinned release checked against its SHA-256), builds only the MP4 reader and those two
decoders into `obj/`, and links them into the executable. Nothing of FFmpeg needs to be installed or shipped, and the
Linux build runs on any distribution whatever FFmpeg version it has (or none). This needs `curl` and, for FFmpeg's
fast x86 code, `nasm` (without it FFmpeg uses plain C, which is fast enough for these movies). It takes a few minutes,
once. FFmpeg is LGPL 2.1; its license goes into `build/licenses/`.

`make FFMPEG=system` links the system's avformat / avcodec / avutil instead (FFmpeg 5.1 or newer), as a
distribution package would; that build then only runs with that FFmpeg version.

### Making a release

`make dist VERSION=<the release's tag>` builds everything with that version and packs it for GitHub:

```bash
git fetch --tags                          # (so builds between releases know the last one)
make dist VERSION=0.10.4 BONUS=<path to the bonus savegame's system file>
```

On Windows that writes `dist\Windows-MHGURecomp.0.10.4.zip`; on Linux (or in WSL, with `B=build-linux
O=obj-linux`) `dist/ArchLinux-MHGURecomp.0.10.4.tar.gz`, which keeps the programs executable. Each holds one folder
with `mhgurecomp(.exe)`, `mhgugame.dll` / `.so`, `mhgurecomp-updater(.exe)`, the bundled DLLs, `licenses\`,
`bonus\system` (from `BONUS`, else left empty), and empty `data\`, `mods\` and `cheats\` folders; never anything of
the game's, or the build folder's settings, caches or mods. `make dist` prints each archive's SHA-256.

Upload both archives to a GitHub release whose tag is exactly that version (`0.10.4`, no `v`), as a normal release,
not a pre-release. The updater only looks at GitHub's "latest" release, compares its tag with its own version as
numbers (0.10.10 is newer than 0.10.9), and picks the asset starting with `Windows-` (`.zip`) or `ArchLinux-`
(`.tar.gz`, else `.zip`). GitHub records each upload's SHA-256, which the updater checks. To hold a broken release
back, make it a draft or pre-release: the updater then sees the previous one as the latest.

The version comes from `git describe --tags`. A build that isn't exactly a tag (`0.10.3-4-gabc1234`) never updates
itself, so a development build is never replaced by a release (`--update-force` overrides that). Options for testing
the updater: `--update-no-launch` (don't start the game afterwards) and `MHGU_UPDATE_API=<url>` (another "latest
release" address).

`make` fills `licenses\` with everything a copy has to carry: this project's `LICENSE.txt`, `THIRD_PARTY_NOTICES.txt`
(code compiled in), FFmpeg's license, and on Windows the licenses of the MSYS2 packages behind the bundled DLLs and
the libraries linked in. `licenses\README.txt` lists those with their versions and where their exact source is, and
holds a written offer for the source of the LGPL DLL among them (libiconv, which SDL3 uses): whoever publishes the
release makes that offer, so keep those sources at hand for three years (the MSYS2 address it gives is one copy).

The exe creates the `mods` and `cheats` folders and the ini on first start. Players then add their own `data\` as
described above.

### Linux

The Windows build runs under Proton / Wine. A native Linux build is supported as well: the Makefile picks
`runtime/platform/linux.c` when `uname -s` isn't Windows and everything else is shared. Tested on Gentoo
x86_64 with clang under Wayland (NVIDIA). Besides `make`, `git`, `clang`, `lld` and Python 3.10 or newer it
needs:

- SDL3, the Vulkan loader and headers and shaderc — the same libraries the MSYS2 list above installs, under your
  distro's names — plus `curl` and optionally `nasm` for building FFmpeg (see "FFmpeg" above; no FFmpeg package
  needed);
- `vk_mem_alloc.h` (3.4.0) in `third_party/vk_mem_alloc/`, which the Makefile has on the include path (a
  distro package that installs the header where the compiler looks by default works just as well);
- `zenity` or `kdialog` for the dialog on a fatal error. Without either, the message goes to the terminal and
  to `log.txt` as usual;
- for the updater: zlib's headers to build it, and `libcurl.so.4` (curl) when it runs, which SteamOS and the
  usual desktop distributions have. It loads libcurl itself, so without it the updater just starts the game.

```bash
python3 -m venv tools/venv && tools/venv/bin/pip install -r tools/requirements.txt
make gen GAME=../mhgu/game       # as above: the folder holding exefs/ and romfs/
make -j8 GAME=../mhgu/game
cd build && ./mhgurecomp
```

`build/` becomes the game folder exactly as on Windows: `mhgurecomp`, `mhgugame.so` with the recompiled game
code and `data/` as hard links to your game files, nothing else; the ini, caches and `mods/` are written beside
them when it runs (`MHGU_HOME=<folder>` puts those there instead). Saves go to
`$XDG_DATA_HOME/MHGURecomp/save`, default `~/.local/share/MHGURecomp/save` — the folder the Windows build
keeps them in under `%APPDATA%`, same layout, so moving a save across is copying `save_0`.

From Windows, the Linux build can be made in WSL with Arch Linux (`wsl --install -d archlinux`), straight from the
Windows checkout, into its own folders so the Windows build is left alone:

```bash
pacman -S --needed make git clang lld python sdl3 vulkan-headers vulkan-icd-loader shaderc nasm vulkan-swrast
mkdir -p /usr/local/include/vma && cp /mnt/c/msys64/ucrt64/include/vma/vk_mem_alloc.h /usr/local/include/vma/
make -j16 B=build-linux O=obj-linux     # in the repository, under /mnt/c/...
```

Arch has no package for `vk_mem_alloc.h`; MSYS2's (3.4.0) is the one the Windows build uses. `build-linux/data` can
hold links to the game files (`ln -s /mnt/c/.../exefs build-linux/data/exefs`, same for `romfs`, then
`touch build-linux/data/.complete`) instead of a second copy. It also runs there, in a WSLg window, slowly: WSL's
own Vulkan driver (dzn, Vulkan on Direct3D 12) can't create the renderer's pipeline layouts, so use Mesa's software
one with `VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json`. WSLg can't capture the mouse (clicks still work as
buttons), and screenshots come out black there.

## Repository layout

| Folder | What's in it |
| --- | --- |
| `tools/` | the recompiler: `lifter.py` (ARM to C), `neon.py` (NEON / VFP), `gen.py` (whole modules), `nso.py`, test and research tools |
| `runtime/` | the host side: CPU state, kernel and threads, Switch services (`services/`, `hle/`), networking (`net/`), the NVN graphics API on Vulkan (`nvn/`), settings, input, mods |
| `updater/` | `mhgurecomp-updater`: installs the latest GitHub release over the game folder, then starts the game |
| `docs/` | `STATUS.md`: progress, design notes, reverse-engineering findings, debug options |
| `extras/` | `cheats/`: cheat files players can copy into their `cheats` folder |

## License

MHGU Recomp's own code is under the MIT License (`LICENSE`). It builds on code from LAS Recomp and Forge, and
bundles cimgui and Dear ImGui, AMD's FidelityFX SDK (FSR 3.1) and FSR-Vulkan, all under the MIT License too: see
`THIRD_PARTY_NOTICES`. The license does not cover
the game or its code, including the recompiled game code a build produces (`mhgugame.dll` / `mhgugame.so`).

## Legal

This project is not affiliated with or endorsed by Capcom or Nintendo. Monster Hunter is a trademark of Capcom. The
repository contains no game code, data or keys; the build reads them from a copy you provide. Please support the
developers by owning the game.
