# MHGU Recomp

A native PC port of **Monster Hunter Generations Ultimate** (Nintendo Switch, version 1.4.0), made by statically
recompiling the game's 32-bit ARM code to C and running it on a host runtime: Switch system services and the NVN
graphics API are reimplemented on Windows, with Vulkan for rendering.

**No game files are included.** You need your own copy of the game, dumped from your own Switch. This project does
not explain how to dump it and does not provide game files, keys or links to them.

Status: fully playable, but there may be some bugs. I've played a dozen hunts with no remaining issues. Multiplayer also works.

## What it adds over the Switch version

- Graphics settings (resolution, MSAA, FXAA, Supersampling, VSync)
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
- Ingame PC Settings menu overlay (F1 or `Plus`+`Minus` to open, will be replaced with a real settings menu in the future)
- Working online multiplayer servers, in public or with friends (use "Local Wireless" menu)

## Requirements

- OS: Windows 10/11 64-bit, Linux 64-bit with glibc 2.39 or newer (Ubuntu 24.04, Linux Mint 22, Debian 13,
  SteamOS 3.7, Arch, Fedora 40 or newer) and a Vulkan driver, or macOS 13 or newer on an Apple Silicon Mac
  (see [macOS](#macos-apple-silicon))
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
   merged over it: about 35,500 files, 12.3 GB.
3. Start `mhgurecomp.exe`.

If the files are missing, the wrong version or incomplete (for example the base game's romfs without the update's
files, or the update's alone), a message box says so.

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

Keyboard and mouse support (not recommended). The default keys:

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

Change them in Settings (F1), Controls, Keybinds: select a button, then Set key and press a key or click a mouse
button (away from the menu's buttons), or Add a second key (each takes up to two). They are kept in the ini's
`[Keybinds]` section (`B = Space`, `Plus = Return, Keypad Enter`, `X = Mouse Left`; key names as the menu shows
them, `None` for none). A key that does something else too shows red. Escape, Insert, F1 and F7 to F12 are the
recomp's own and can't be used.

Click in the game window to capture the mouse for the camera; Escape lets it go (so does switching to another
window). `MouseSensitivity` in the ini's `[Mouse]` section sets its speed; the game's own camera options (speed,
inverted axes) apply to it too.

When the game asks for text (names, chat), the keyboard types: Enter confirms, Escape cancels, Ctrl+V pastes. `/` opens a line of chat anywhere, also in a quest (where the game itself only has shoutouts): type (up to 120 characters), Enter sends it to everyone as a shoutout is sent, split between words into lines of the chat window, Escape cancels. While typing, the mouse drags the box elsewhere (kept in `chat_box.txt`). Change the key in Settings (F1), Controls, Keybinds, Chat key: it shows red when a keybind already uses that key.

Other keys: F1 opens the settings menu, Alt+Enter (Option+Enter on a Mac) or F11 switches between windowed and full
screen, F12 takes a screenshot, F7 cycles MSAA.

## Settings

Everything is in `mhgurecomp.ini`; each setting is explained in the file itself. The settings menu changes them in
the game: **F1**, or **Minus + Plus** on a controller (Back + Start on an Xbox controller). Frame rate, FOV, draw distance,
anti-aliasing, VSync, controls and volume apply when you save; the settings marked * at the next start. Details:
[docs/SETTINGS_UI.md](docs/SETTINGS_UI.md). In short:

- `[Display]`: `Mode` (Borderless / Fullscreen / Windowed), `Resolution`, `RenderResolution` (the 3D resolution),
  `AspectRatio` (Auto follows the window: 16:10 and anything wider fill it, narrower gets 16:10 with bars), `FOV`
  (1.0 = the game's), `UIAnchor` (Center keeps the HUD in the middle 16:9 on an ultrawide screen, Edges spreads it
  to the sides)
- `[Anti-aliasing]`: `MSAA` (Off / 2x / 4x / 8x), `FXAA` (On / Off), `FXAAMode` (Original / 3_11: FXAA 3.11 in
  place of GU's FXAA, sharper textures; default Original)
- `[Upscaler]`: `FSR` (Off / On, default Off), `FSRQuality` (NativeAA / Quality / Balanced / Performance /
  UltraPerformance), `Sharpness`, and `Upscaler` (FSR3, the default, or FSR4: needs an AMD RDNA 4 GPU and its driver, and an exe built with `make FSR4_DRIVER_PATCH=1`;
  falls back to FSR 3.1). FSR replaces MSAA and FXAA while it is on. Details: `docs/FSR-PORT.md`, `docs/FSR4-PORT.md`
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
  later load too, after the listed ones, until you turn them off; a mod turned off keeps its place in the order). It also compares the mods and lists the ones that change the same game files (and which of
  the two is used), and can apply a suggested order, with the mods that change the most files first.
- exefs patches (`.pchtxt`, `.ips` / IPS32) are applied, to data and to code. The game's code is precompiled, so a
  function a patch changes runs in an interpreter instead (a little slower, only that function). A mod's own code
  modules (`exefs\subsdk1` and so on, as in the
  [MH4U Port](https://github.com/Scrimas/MH4U-port-to-MHGU)) are loaded and run in the interpreter too. The log says
  what was patched and loaded (`[mods]` lines).
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
- Linux: `SDL3=system`, `SHADERC=system`: link the system's SDL3 / shaderc instead of building them (see Linux).

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
O=obj-linux`) `dist/Linux-MHGURecomp.0.10.4.tar.gz`, which keeps the programs executable. Each holds one folder
with `mhgurecomp(.exe)`, `mhgugame.dll` / `.so`, `mhgurecomp-updater(.exe)`, the bundled DLLs, `licenses\`,
`bonus\system` (from `BONUS`, else left empty), and empty `data\`, `mods\` and `cheats\` folders; never anything of
the game's, or the build folder's settings, caches or mods. `make dist` prints each archive's SHA-256.

Upload both archives to a GitHub release whose tag is exactly that version (`0.10.4`, no `v`), as a normal release,
not a pre-release. The updater only looks at GitHub's "latest" release, compares its tag with its own version as
numbers (0.10.10 is newer than 0.10.9), and picks the asset starting with `Windows-` (`.zip`) or `Linux-`
(`.tar.gz`, else `.zip`; `ArchLinux-`, the name up to 0.11.4, as well). GitHub records each upload's SHA-256, which the updater checks. To hold a broken release
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

- the Vulkan loader and headers, plus `curl` and optionally `nasm` for building FFmpeg (see "FFmpeg" above; no
  FFmpeg package needed);
- `cmake` and the development files of what SDL uses on a desktop: X11 (with Xcursor, Xext, Xfixes, Xi, Xrandr,
  Xss, Xtst), Wayland (wayland-protocols, libxkbcommon, libdecor), PipeWire, PulseAudio, ALSA, libudev and D-Bus.
  Like FFmpeg, the first build downloads pinned releases of SDL3 and shaderc (with glslang and SPIRV-Tools) to
  `third_party/`, checked against their SHA-256, and links them in statically, so the game runs on distributions
  that have neither (Ubuntu 24.04 has no SDL3) and the release needs nothing besides glibc, libstdc++ and the
  Vulkan loader. SDL still loads X11 / Wayland and the audio server from the system when it runs, and when the system
  has an SDL3 of the same version or newer (new controllers, fixes), the game uses that one instead: `log.txt` says
  which (`[ui] SDL ...`), and `MHGU_SDL=bundled` keeps the built-in one. That takes a few
  minutes, once; the build stops if SDL didn't find one of those (`SDL3_NEED`). `make SDL3=system SHADERC=system`
  links the system's `libSDL3.so.0` / `libshaderc_shared.so.1` instead, as a distribution package would;
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
pacman -Syu --needed make git clang lld python cmake vulkan-headers vulkan-icd-loader nasm vulkan-swrast \
    libx11 libxcursor libxext libxfixes libxi libxrandr libxss libxtst wayland wayland-protocols libxkbcommon \
    libdecor pipewire libpulse alsa-lib systemd-libs dbus
mkdir -p /usr/local/include/vma && cp /mnt/c/msys64/ucrt64/include/vma/vk_mem_alloc.h /usr/local/include/vma/
make -j16 B=build-linux O=obj-linux     # in the repository, under /mnt/c/...
```

Arch has no package for `vk_mem_alloc.h`; MSYS2's (3.4.0) is the one the Windows build uses. `build-linux/data` can
hold links to the game files (`ln -s /mnt/c/.../exefs build-linux/data/exefs`, same for `romfs`, then
`touch build-linux/data/.complete`) instead of a second copy. It also runs there, in a WSLg window, slowly: WSL's
own Vulkan driver (dzn, Vulkan on Direct3D 12) can't create the renderer's pipeline layouts, so use Mesa's software
one with `VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json`. WSLg can't capture the mouse (clicks still work as
buttons), and screenshots come out black there.

### macOS (Apple Silicon)

A native macOS build for Apple Silicon Macs: the Makefile picks `runtime/platform/darwin.c` when `uname -s` is
`Darwin`, and rendering goes through MoltenVK (Vulkan on Metal). Tested on an M1 Pro at 60 fps: hunts, movies,
audio, a DualSense with rumble, Local Play with a Windows PC, mods, cheats and Forge plugins. Building needs Xcode's
command line tools and [Homebrew](https://brew.sh).

Everything is built for macOS 13 and newer (`MACOS_MIN`), so a build runs on other Macs too. Homebrew's libraries
load only on the macOS they were installed on, so `make macos-deps` builds the ones the game loads from source, once,
into `third_party/macos-deps` (`tools/macos_deps.sh`): SDL3, the Vulkan loader, shaderc and a small LGPL FFmpeg with
only the demuxers and decoders the movies need. MoltenVK and the Vulkan headers come from Homebrew.

```bash
brew install cmake pkg-config vulkan-headers molten-vk python@3.12
make macos-deps                  # once: third_party/macos-deps/
mkdir -p third_party/vk_mem_alloc && curl -fL -o third_party/vk_mem_alloc/vk_mem_alloc.h \
  https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/v3.4.0/include/vk_mem_alloc.h
python3.12 -m venv tools/venv && tools/venv/bin/pip install -r tools/requirements.txt
make gen GAME=../mhgu/game       # as above: the folder holding exefs/ and romfs/
make -j10 GAME=../mhgu/game      # NULL_GPU=1: without the renderer
./run-game.sh                    # settings, caches and log.txt in run/, as run-game.bat
```

`build/` holds `mhgurecomp`, `mhgugame.dylib` and `data/` (APFS clones of your game files: no extra disk space).
Saves go to `~/Library/Application Support/MHGURecomp/save`, same layout as on Windows and Linux. Under lldb, the
first touch of each guest page stops with `EXC_BAD_ACCESS`; let the runtime's handler take it with
`process handle SIGSEGV SIGBUS -n false -p true -s false`.

To play from Finder, make an app of the build:

```bash
make app GAME=../mhgu/game       # app/MHGURecomp.app
```

`app/MHGURecomp.app` is self-contained: the runtime, the game code, your game files (APFS clones), the libraries it
loads and MoltenVK, signed ad hoc for your Mac. **It contains game code and data made from your dump: never share
it.** Everything it writes goes to `~/Library/Application Support/MHGURecomp`: `mhgurecomp.ini`, `mods/`, `cheats/`,
`log.txt`, the shader caches and the saves.

On the Mac, **Settings…** (⌘,) opens a window with every setting of `mhgurecomp.ini`, in tabs, each with the file's
explanation; the **Settings** menu in the menu bar changes the common ones, even in full screen (most take effect at
once, the rest after **Restart to Apply**) and opens the settings, mods, cheats and saves folders. Option+Enter or
F11 switches between windowed and full screen. The first start warms the game's shaders in the background (about 6
minutes on an M1 Pro; a line in the window's corner says how far), so new areas don't stutter later.

#### Making a macOS release

A release is the runtime as an app, with no game code and no game files in it, zipped like the Windows and Linux
ones: `macOS-MHGURecomp.<version>.zip` holding `MHGURecomp.app` (about 8 MB). The game code is built on each
player's Mac from their own game files the first time the app starts, the same steps as `make gen` and `make`: the
app carries the code generator, the runtime headers the code includes and `tools/build_gamecode.py`, which runs them
with macOS's own `python3` and `clang` (Apple's Command Line Developer Tools) into
`~/Library/Application Support/MHGURecomp/gamecode/`. Making a release needs no game files or generated code.

```bash
make release VERSION=0.9.11 BONUS=<bonus folder>   # release/macOS-MHGURecomp.0.9.11.zip
```

`BONUS` is the folder holding the donor save's `system` file (the `bonus` folder of the other releases); the app
carries it in `Contents/Resources/bonus`. Leave `BONUS` out to make a release without bonus content. The app carries
the license of every library in it in `Contents/Resources/Licenses` (with FFmpeg's source and configure line, as the
LGPL asks); `make macos-deps` keeps them. `make_app` refuses to finish if one is missing, if the app holds game code,
if any binary in it needs a newer macOS than `MACOS_MIN` or loads a library from outside the app and macOS, or if it
names a folder in your home folder. The app is signed ad hoc, not notarized.

#### Installing a macOS release

1. Extract the zip and move `MHGURecomp.app` to Applications.
2. Start it. The first time, macOS refuses to open an app from the internet that isn't notarized: open **System
   Settings → Privacy & Security**, click **Open Anyway** next to MHGU Recomp, and confirm. (Or, in Terminal:
   `xattr -dr com.apple.quarantine /Applications/MHGURecomp.app`.) Once it starts, it says the game files are missing
   and opens `~/Library/Application Support/MHGURecomp/data`.
3. Put your game's `exefs` and `romfs` folders in that `data` folder (laid out as in [Installation](#installation))
   and start the app again.
4. The first start builds the game's code from your game files, once (about 2½ minutes on an M1 Pro; a window shows
   how far). It needs Apple's free Command Line Developer Tools: if they aren't installed, the app offers Apple's
   installer and continues when they are in place. After an update of the app it builds the code again.
5. For Local Play multiplayer, allow MHGU Recomp on the local network when macOS asks (or later in **System Settings
   → Privacy & Security → Local Network**).

Everything else is in `~/Library/Application Support/MHGURecomp` too: `mhgurecomp.ini`, `mods/`, `cheats/`,
`log.txt`, the game code (`gamecode/`, with its `build-log.txt`), the shader caches and the saves (`save/save_0`, as
on Windows). Updating means replacing the app; your files stay where they are.

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
