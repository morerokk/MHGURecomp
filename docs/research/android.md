# Android build (planned 2026-10-25, assessed and started 2026-10-31: runs in the emulator)

Goal: the port on Android phones / tablets, played with a controller (Bluetooth or USB). No touch controls: the game
doesn't suit them, so none are planned. Players bring their own dump as on PC; the APK holds no game files.

Status (2026-10-31, branch feat/android-build): the APK builds (x86_64 for the emulator, arm64-v8a for phones), and in
the Android 15 emulator the game boots, translates its shaders, and quick-hunts into quest 101 with everything drawn
as on PC (also with BC textures decoded on the CPU, as a phone needs); the intro movie plays. Not yet tried: a real
phone, controls by hand, sound, multiplayer, pausing / resuming the app. Released from 0.13.0 on (make dist, below);
the player's side is README.md "Android".

Status (2026-11-02, branch feat/vulkan-1.1): first real device, an NVIDIA Shield TV Pro 2019 (Tegra X1+, Android 11, 3 GB
RAM, Vulkan 1.1.178): the arm64 build boots, translates its shaders and quick-hunts into quest 101, drawn correctly,
41-45 fps in the hunt at 1080p with FXAA. Still not tried by hand: controls, sound, multiplayer, pausing / resuming. See
"Vulkan 1.1" below.

## Building and running

Needs the Android SDK (default /c/Android/Sdk, or ANDROID_HOME) with `ndk`, `cmake`, `build-tools`,
`platforms/android-35`, and for testing `platform-tools`, `emulator` and an x86_64 system image; a JDK 17+ (default
/c/Android/jdk-21, or JAVA_HOME). No Gradle: android/apk.mk packages the APK with the SDK's own tools.

    make PLATFORM=android ABI=x86_64 -j16        # the emulator's ABI: build-android/lib/x86_64, the APK
    make PLATFORM=android ABI=arm64-v8a -j16     # phones; the APK then holds both ABIs
    make PLATFORM=android ABI=x86_64 install     # adb install -r build-android/mhgurecomp.apk

The release (`make dist VERSION=...` on Windows, or `make PLATFORM=android dist VERSION=...`) is
`dist/Android-MHGURecomp.<version>.apk`: arm64-v8a only (x86_64 is for the emulator; it would double the download),
signed with the release key in `%USERPROFILE%\.android\mhgurecomp-release.keystore` (+ `.pass`; README.md "Making a
release"). The development APK is signed with the SDK's debug key instead, so the two can't update each other: switching
between them on one device means uninstalling (and losing the app's folder).

The first build of an ABI also builds SDL3 (shared, for SDL's Java activity), shaderc and FFmpeg for it (~3 min);
objects go to obj-android/<abi>/. The APK is ~115 MB per ABI, nearly all of it the game code (libmhgugame.so).

On the device everything lives in the app's folder, /sdcard/Android/data/io.github.morerokk.mhgurecomp/files (Android
creates it at the first start, which then says the game files are missing): the game files in `data/exefs` and
`data/romfs`, and beside them mhgurecomp.ini, log.txt, save/, mods/, cheats/, cache/, shots/. Copying the game files
with adb (from MSYS2: `MSYS_NO_PATHCONV=1`, Windows paths for the local side):

    adb push C:/MHGURecompile/mhgu/game/exefs /sdcard/Android/data/io.github.morerokk.mhgurecomp/files/data/
    adb push C:/MHGURecompile/mhgu/game/romfs /sdcard/Android/data/io.github.morerokk.mhgurecomp/files/data/

(12.3 GB: ~3 minutes into the emulator.) Android starts apps without environment variables, so the debug ones come
from `env.txt` in that folder, one `NAME=value` per line (runtime/platform/android.c): e.g. `MHGU_QUICKHUNT=101`,
`MHGU_SHOT_AT=...`, `MHGU_NO_BC=1`, `MHGU_FPS=1`, `MHGU_SAVE_DIR=<a folder adb made>` for a test save. Screenshots:
`adb exec-out screencap -p > shot.png`.

The emulator used here (avdmanager, `system-images/android-35/google_apis/x86_64`, device "mhgu"): 8 GB RAM, a
48 GB data partition (`disk.dataPartition.size`), 1920x1080 landscape, `hw.gpu.mode=host`, `hw.keyboard=yes`; started
with `emulator -avd mhgu -gpu host -no-snapshot`. Its Vulkan is the host GPU's through gfxstream (here the RTX 5070 Ti:
Vulkan 1.3, BC included, so MHGU_NO_BC=1 stands in for a phone's GPU).

The emulator is for seeing things work, not for speed: the game ran at ~11 fps in a hunt. Its render thread spends
its time in the emulator's GPU pipe (goldfish_pipe, copying every Vulkan call to the host), and its only clock is the
HPET, so every clock read traps to the hypervisor (the event loop's thread: 100% of a core in read_hpet, at 234
loops a second as on PC). A phone has neither cost. Measure on a phone.

## How it is put together

- The APK: SDL's own Java activity (org.libsdl.app.SDLActivity, from SDL's android-project, compiled with javac + d8;
  the manifest names android/java's MHGUActivity, a subclass that also hides the status and navigation bars the
  Android 11+ way, as SDL's flags alone left them over the game on some devices)
  loads libSDL3.so and libmain.so (the runtime with shaderc and FFmpeg linked in) and calls SDL_main (main.c includes
  SDL_main.h on Android). The runtime then loads libmhgugame.so by name (runtime/gamelib.c), which is linked against
  libmain.so for the runtime's functions. Libraries are stored uncompressed and 16 KB aligned
  (extractNativeLibs="false", zipalign -P 16, linked with max-page-size 16384), without their debug info (the build
  folder keeps them whole). android/AndroidManifest.xml: landscape, controller / keyboard, Vulkan 1.1, network
  permissions for multiplayer. Signed with the SDK's debug key (~/.android/debug.keystore).
- minSdk 29 (Android 10), targetSdk the newest installed platform (35). What decides whether a phone runs the game is
  its GPU driver (Vulkan 1.1, shaderInt64), not the Android version; nothing in the runtime needs
  a newer API (execinfo's backtrace() is Android 13+: the unwinder's _Unwind_Backtrace instead).
- runtime/platform/linux.c serves Android too, with android.c for the app's folder (SDL_GetAndroidExternalStoragePath),
  env.txt and the message box. `umask(007)` at start: Android gives apps 077, and folders the game made (save, mods,
  cheats...) could then not be written over USB or adb.
- GUEST_BASE is 64 GB on Android (runtime/cpu.h): most Android kernels give a process 512 GB (39-bit addresses).
- The fault handler (backing guest pages on first touch) reads the interrupted registers per architecture: x86-64's
  gregs, arm64's pc / sp / x30 and the write bit from the signal frame's ESR record. Android's libsigchain runs ART's
  handler first; guest page faults reach ours (seen working on x86_64; arm64 not yet run).
- Relative paths given to SDL's file functions open the APK's assets on Android: the screenshot writer makes its path
  absolute (device.c vk_save_png). Other file access goes through stdio from the app's folder (the working directory).

## Left to do

1. A real phone (arm64): performance, thermal throttling, a mobile GPU's driver (Adreno, Mali). The fault handler's
   arm64 path works (Shield TV, see "Vulkan 1.1").
2. Shader int64: every translated shader has `#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require`
   (maxwell/translate.c), while device.c only logs a warning without shaderInt64. Not every phone driver has it: there
   every pipeline would fail. Check the target GPUs (gpuinfo.org), or make the translator do without int64.
3. Controls by hand. Note: SDL took the emulator's keyboard ("qwerty2", 0627:0001) for a gamepad ("[input] gamepad
   in use"); a real controller should be preferred over it. The on-screen keyboard comes up only for a real text box
   (the game's entries, the chat, a Forge plugin's field: textinput.c), not for the settings menu with its own keys.
4. (done) pipelines.list and bonus/system: in the APK's assets, copied into the app's folder at start (android.c:
   pipelines.list whenever it differs, bonus/system only when missing); exec.c reads pipelines.list from there.
5. (done) Getting the game files and saves onto a phone and out again (the settings menu's Android tab; Save folder
   keeps the save in a folder the player picked, so file syncing software can keep it in step: the picker's tree id is turned into a /storage path,
   which takes MANAGE_EXTERNAL_STORAGE, "All files access"; config.save_dir, ui.c ui_init): Android/data can't be reached from a phone's own file
   managers since Android 11, only over USB (MTP) or adb. The app imports them itself through the Storage Access
   Framework (android/java: ImportActivity.java, started from runtime/platform/android.c): the game files from a
   folder the player picks (offered at the start when they are missing or wrong, and in the settings menu), a save's
   system / system_backup files (settings menu; the old save moved aside). Copied into a .part folder, then moved in.
   Mods come in as a zip or a folder the same way (ImportActivity.plan checks the layout against what
   mods_find_dir finds: a romfs / exefs / nativeNX folder at most 3 deep; buried ones are moved up, loose
   .pchtxt / .ips go into exefs/, and the player is asked before either). From the settings menu the app closes afterwards (the game has the old files open), and the renderer draws
   nothing meanwhile (ui_window_away: the picker takes the window's surface, item 6).
6. Lifecycle. The app leaving the screen and coming back works (2026-11-05, emulator): Android takes the window away and
   makes a new one, which killed the Vulkan surface (every frame VK_ERROR_SURFACE_LOST_KHR, a black screen for good), so
   device.c surface_ready closes the surface and swapchain and makes new ones. SDL pauses and resumes the sound. Tried: Home,
   five quick round trips, 45 s away, the screen off and on, Recents, Home right at the start (backend_init waits for
   the window), all in a hunt but the last. The game keeps running while it is away, the hunt's clock too, with no input (the
   event loop's thread waits in SDL): decided 2026-11-05, as freezing it could drop the player from a multiplayer session.
   Left: Android may kill a backgrounded app, the back button.
7. LAN multiplayer: broadcast discovery needs Android's WifiManager.MulticastLock (a Java call). The hub server should
   work as it is.
8. Left in but useless on Android: Discord Rich Presence (finds no Discord), the settings menu's Restart (can't start a
   new process: it reports failure). The updater isn't built.
9. 16 KB pages: PLAT_PAGE is fixed at 4 KB. The libraries are 16 KB aligned, but a 16 KB page kernel would also need
   plat_protect / commit at that granularity.
10. (done) Movies: FFmpeg is built in (as on PC); the intro movie plays in the emulator.
11. Uninstalling deletes the app's folder with the game files and saves; android:hasFragileUserData makes Android
    offer to keep it. A folder outside Android/data (all files access) would survive, at the cost of a scarier
    permission.

## Performance (expectations, not measured)

- CPU: the Switch runs the game on Cortex-A57 cores at ~1 GHz. Ahead-of-time compiled C on a recent 2.4-3 GHz phone
  core has a lot of headroom: expected 30 fps comfortable on 2021+ mid / high range, 60 plausible on flagships.
  The arm64 build uses the ABI's baseline (ARMv8.0; Makefile GENARCH): `-march=armv8.2-a` would allow more.
- GPU, the real risk: every render pass loads and stores its attachments (exec.c: LOAD_OP_LOAD / STORE_OP_STORE).
  Cheap on desktop GPUs; on tile-based phone GPUs each pass reads and writes every attachment through memory. Levers:
  RenderResolution, the FPS cap, CLEAR / DONT_CARE where the game clears anyway.
- Thermal throttling over a long hunt.

## BC textures

Phone GPUs (Adreno and Mali on Android) have no textureCompressionBC. `MHGU_NO_BC=1` makes a desktop GPU act the
same. The game uses BC1, BC3, BC4 and BC5 (no BC6H / BC7 / ASTC seen); runtime/nvn/bcn.c decodes them on upload, and
the hunt and the village look the same as with the GPU's decode, on PC and in the emulator. Details: docs/STATUS.md
"BC textures without GPU support". Left for a phone: the decode costs CPU time at loads, and the decoded images take
2x (BC4 / BC5) to 8x (BC1) the memory of the compressed ones. If that hurts: transcode to ETC2 (which every phone GPU
samples), or decode in a compute shader.

## Vulkan 1.1 (2026-11-02)

Many Android devices stay on Vulkan 1.1, so the backend runs there too. Everything hangs on `g_vk.api11` (the device
is below Vulkan 1.2, or `MHGU_VK11=1`) and `g_vk.fences` (no timeline semaphores: always with api11); on a Vulkan 1.2
device neither is set and the same Vulkan calls and shaders are made as before.

- Submissions: a fence per submission keeps the numbers (exec.c): a signalled fence also covers everything submitted
  before it, as a timeline value does.
- Shaders: SPIR-V 1.3 instead of 1.5, in other files of cache/spirv/. The translated GLSL needs nothing newer.
- Features: VkPhysicalDevice16BitStorageFeatures for what the Vulkan11 / Vulkan12 structures held; sampler mirror clamp
  from VK_KHR_sampler_mirror_clamp_to_edge (without it: mirrored repeat). shaderInt8 and scalarBlockLayout aren't asked for.
- FSR 3.1 stays off (fsr.c says so in the log and the picture is scaled from the smaller 3D size). The other picture
  settings are unchanged. A PC with both picks a Vulkan 1.2 GPU over a 1.1 one.
- `MHGU_VK11=1` takes this path on a PC. R9 270X, Windows, quest 101, 14 minutes each: 53.6 fps on the 1.2 path, 53.7
  on the 1.1 path, the same picture, nothing in the log.

Measured on the Shield TV Pro 2019 (BC textures native, D24S8, 1080p, FXAA, no MSAA): 41-45 fps in quest 101, render
thread ~1.4 s of every 2 s (~600 MB of guest memory copied in that time, so it is CPU bound there, not GPU). First start:
the prewarm translated 5518 programs in 138 s, 0 failed, 29-35 fps meanwhile; cache/ 343 MB. Peak resident 1.15 GB (887 MB
without the prewarm), 620 MB still available to the system, no crash in 9 minutes. `[vk] no page write tracking (-1)` in
its log: textures are checked by hashing, which may be part of that CPU time.

## Android TV and game files on a drive (2026-11-02, branch feat/android-tv)

- Home screen: a LEANBACK_LAUNCHER category beside LAUNCHER on the one activity, `android.software.leanback` not required
  (so phones are unaffected), and android:banner = android/res/drawable/banner.xml, a vector drawing of "MHGU" with
  nothing of the game's (apk.mk: aapt2 compile, then link). Android TV lists it under Installed Games.
- `[Android] GameFolder` (config.c) is the folder holding exefs/ and romfs/, used in place: a Shield has less room than the
  12.3 GB they need, and adb can't create files in the app's Android/data folder on a USB drive set up as portable storage.
  Set from Game files folder in the settings' Android tab, or Use where they are at the first start (android.c
  android_offer_import; ImportActivity GAME_FOLDER). It takes "All files access". main.c needs the value before the settings
  load, so android_game_folder reads that one line of the ini. An import clears it, and always copies into the app's own
  folder. `MHGU_DATA_DIR` (env.txt) still wins, for tests.
- The picker's document ids: a volume id may be any hex, not only an SD card's 1234-ABCD; an NTFS drive's has 16 digits
  (this also let Save folder use such a drive).
- "Don't keep activities" (Developer options; it was on on the Shields tested): Android destroys the game's activity as
  soon as the picker opens, SDL ends the process when its activity is created a second time, and the choice is lost. The
  picker now says so instead of opening. The same would happen if Android reclaimed the activity under memory pressure;
  not seen otherwise, and a real fix means surviving SDL's onDestroy (it sends the game a quit event).
- In the message box a remote's or controller's select takes the highlighted button; a keyboard's Enter always takes the
  default button (SDL's).
- From a USB NTFS drive, a first start took 108 s to the first frame on the Shield, with a black screen.

Tested on the Shield: the banner; a saved GameFolder, the game running from the drive with nothing in env.txt; the settings row;
the message for a folder that isn't there; the picker opening on the drive, using a remote's keys. Not tested end to end:
returning from the picker and Android's "All files access" page, which need "Don't keep activities" off.

## Open questions

- Minimum device: arm64; Android 10+, Vulkan 1.1 with shaderInt64; a Shield TV (3 GB RAM) runs it; other devices roughly Snapdragon 865 /
  Adreno 650 class or better, 6 GB RAM.
- Multiplayer works the same over Wi-Fi (Local Play over IP; the internet hubs plan in internet-hubs.md).
