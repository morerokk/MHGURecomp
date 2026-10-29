# MHGU Recomp: status and working notes

Static recompilation of *Monster Hunter Generations Ultimate* (Switch, v1.4.0, build id `FB08F1D2...`) to a native
Windows executable, modeled on LAS Recomp (`../LASRecomp`). Goal: easy modding of game data (weapon / monster
values etc.). No game files or generated code are ever committed (see `.gitignore`).

## Key facts

- The game is **32-bit ARM** (AArch32, ARM mode only, `NX-NXFP2-a32`, NintendoSDK 4.4.2), not AArch64 like LAS.
  Modules: `main` (~4.9M reachable instructions), `sdk` (nnSdk, fully symbolized), `subsdk0` (Android Stagefright
  movie player: **not recompiled**; `movie::` API to be implemented on the host with FFmpeg, as LAS does).
- Guest memory: a 4 GB window at `GUEST_BASE` (0x10000000000); guest address `a` = host `GUEST_BASE + a`
  (`G2H` / `H2G` in `runtime/cpu.h`; the name `GPTR` clashes with windows.h). Pages are committed on first touch.
- Layout (`runtime/rt.h`): main @0x200000, sdk @0x2400000, HLE fake functions @0x3000000, TLS @0x3100000,
  HLE heap @0x4000000, heap @0x8000000 (SetHeapSize gets 2.5 GB: GetInfo total memory is reported so),
  alias @0xA8000000, stacks @0xD0000000.
- 32-bit SVC ABI derived from the SDK's svc stubs (see `runtime/kernel.c` comments): 64-bit values in register
  pairs, 5th argument in r4.
- Target hardware of the owner: i7-4770K, 8 GB RAM, AMD R9 200 series (GCN, driver 2020, **Vulkan 1.2.133**,
  1.8 GB VRAM). `build/vkprobe.exe` (tools/vkprobe.c) output, 2026-10-20:
  - missing: dynamic_rendering, synchronization2, extended_dynamic_state 1/2/3, push_descriptor,
    custom_border_color, maintenance4, index_type_uint8, robustness2, provoking_vertex, ASTC, ETC2, shaderFloat16;
  - present: imageless_framebuffer, create_renderpass2, separate_depth_stencil_layouts, timeline semaphores,
    shader_demote_to_helper_invocation, scalarBlockLayout, shaderInt8 + 8/16-bit storage, samplerMirrorClampToEdge,
    vertex_attribute_divisor, external_memory_host, line_rasterization, depth_clip_enable, BC textures,
    descriptor_indexing, inline_uniform_block, shaderInt64, robustBufferAccess, dualSrcBlend, logicOp.
  The renderer must be Vulkan 1.2 baseline (works on GTX 1080 too). LAS's Vulkan backend is 1.3: reuse its pieces,
  not wholesale.

## Recompiler (tools/)

- `nso.py` ELF32/REL loader; `lifter.py` A32 + VFP; `neon.py` Advanced SIMD (+ AES, CRC32 via helpers);
  `gen.py` function discovery (.ARM.exidx, symbols, relocs, call targets, fixpoint), recursive descent (literal pools
  never lifted; pc-relative literal loads folded to constants), jump tables (`add rB, pc, #imm / ldr rT, [rI, rB] /
  add pc, rT, rB`, with scheduled instructions in between), computed gotos ("all labels" switch fallback).
- `test_lift.py` + `test_harness.c`: differential test against Unicorn (UC_CPU_ARM_MAX). 0 mismatches over tens of
  thousands of sampled instructions (incl. all reachable VFP / NEON). Run after every lifter change:
  `make test-lift`.
- `hle_list.py` writes `runtime/hle/hle_symbols.txt` from `HLE_SDK(...)` uses (SDK functions replaced on the host:
  division helpers, memcpy & co).
- `mtarc.py`: MT Framework ARC v17 list / extract / pack / index.

## Runtime (runtime/)

Ported from LAS, adapted to 32-bit: `kernel.c` (SVCs), `ipc.c`, `loader.c` (REL relocations), `dispatch.c`,
`romfs.c` (**virtual RomFS** built in memory from the loose `romfs/` folder; mod folders `mods/<name>/romfs/`
layer on top: `mods.c`), services (`fs`, `am`, `misc` (acc, set, pctl, aoc, hid service), `time`, `audio` (LAS's
audren mixer; REV4 output layout fixed)), HLE: `vi.c`, `nvn_boot.c`, `ngc.c` (no profanity filter), `input.c`
(nn::hid at API level), `offline.c` (bcat / friends / applets as an offline console), `libc.c`.
NVN layer: `nvn/nvn.h` (argument adapter `nvn_args.c`: signature strings, 'l' = 64-bit, 'f' = float),
`objects.c`, `cmdbuf.c` ported from LAS. Renderer: `nvn/vulkan/` (Vulkan 1.2, see `vk.h`): render pass objects +
framebuffers cached by view, every image in GENERAL layout with full barriers between passes, all fixed-function
state in the pipeline key, descriptor sets from per-submission pools, guest vertex / index / uniform ranges copied
per submission (the 2.5 GB heap is not imported), textures change-detected with page write tracking
(MEM_WRITE_WATCH). `NULL_GPU=1 make` builds the null renderer instead. Shader global memory (LDG/STG) reads zeros
(logged if a shader uses it). Window and controls: `ui.c`, `host_input.c`.

## Progress (2026-10-20)

Plays: health & safety -> opening movie (FFmpeg) -> title -> character creation (name typed: see "Text entry") ->
style / arts -> save slot (save written to run/save/) -> Bherna village intro cutscene -> village with player
control, ~30 fps on the R9 (the game's own rate). Everything seen so far renders correctly.

Fixed on the way (2026-10-20):
- jump tables: the longer of bound-based and heuristic lengths; backward entries; scheduled instructions (VFP
  `vmov`, up to ~20 constant loads) between `add rB, pc` and `add pc`. `generated/*/undetected_tables.txt` lists
  `add pc, rX, rY` jumps not understood as tables (currently none).
- `movie::` HLE (`runtime/hle/movie.c`): deep decoded-frame queues (video held until its time on the audio clock),
  the output-format-changed event before the first frame, and GetOutputBuffer copies into the game's own Buffer.
- InlineKeyboard: DecidedEnterArgUtf8 = text inline at +0, length at +0x7d4; Calc returns the state (0 none,
  1 ready / hidden, 3 shown) - the game waits for 1 after a text entry.
- NVN fixed-function alpha test (ColorState alpha func + SetAlphaRef) in the translator (foliage, impostors).
- Vulkan loader: System32\vulkan-1.dll of older drivers has no 1.3 entry points (VMA_VULKAN_VERSION 1.2).
- Audio: MHGU sends every voice through a splitter (now implemented, plus the BufferMixer effect) - silence before.
- Movies: tight NV12 (the game copies Y then UV right after it) into the game's own Buffer memory.
- Performance: device-local vertex cache refreshed per written page (page write tracking): guest copies went from
  ~880 MB/s to ~8 MB/s; mailbox presentation (FIFO with 2 images held the game at ~26 fps).
- Settings: mhgurecomp.ini in the game folder (runtime/config.c; written with the defaults when missing): Mode = Fullscreen /
  Windowed / Borderless, Resolution = WxH or Desktop, RenderResolution = Auto or WxH, FPS = 30..240, VSync. Alt+Enter
  toggles windowed / full screen. The 3D resolution is the game's own docked render size (main 0xdbcd34 / 0xdbcd38,
  `mov` 1920 / 1080, the constants the 4K-2K mod patches): tools/gen.py PATCHES makes those two instructions read
  g_render_size. Overrides for test runs: MHGU_CONFIG=<file>, MHGU_WINDOW=WxH (windowed), MHGU_RENDER=WxH,
  MHGU_MSAA=1|2|4|8, MHGU_FXAA=0|1, MHGU_PAD_POSITIONAL=0|1. [Controls] FaceButtons = Position (default: the Switch
  layout, right button A) or Label (the letter on the button, via SDL_GetGamepadButtonLabel).
- MSAA (settings MSAA = Off / 2x / 4x / 8x, F7 or MHGU_CONTROL "msaa <n>" switches it live; exec.c "MSAA"): passes
  whose depth target has the 3D resolution render into multisampled companions (TexBackend.ms) while those hold the
  target's contents or are cleared at the pass start (a look-ahead over the command list: leading_clears); a target
  is resolved only when something reads it (lazy: vk_ms_resolve). Colour: vkCmdResolveImage; depth / stencil: a copy
  into a sampleable multisampled image + a shader writing sample 0 (stencil via VK_EXT_shader_stencil_export). NVN
  alpha tests (> >= < <=) become sample coverage (gl_SampleMask, ShaderKey.alpha_coverage). Cut-outs the game does
  with its own discard stay hard-edged. R9 270X, village, 1080p: 56 fps off, 35 at 2x, 28 at 4x, 18 at 8x. Measured
  with MHGU_CONTROL "gpuprof" (GPU timestamps per pass): the multisampled passes cost ~3 ms more at 4x, a colour
  resolve ~1.6 ms, the depth / stencil resolve ~9 ms (the driver decompressing it; Vulkan 1.2 render pass resolves
  were as slow, and a sampleable multisampled image loses its compression: passes several times slower).
- FXAA (settings FXAA = Off / Game / On): the game's FXAA pass is recognised by its fragment shader's code hash
  (FXAA_FS_HASH, exec.c). Game draws it as it is: an early FXAA 3 Quality reading luma from alpha, edge threshold 1/6,
  sub-pixel blur capped at 0.75, 6 looks per side in 2-pixel steps. Off makes it a copy of its input. On draws FXAA
  3.11 Quality instead (exec.c fxaa_311, shaders.c "fxaa.frag") over the same input, with FXAAQuality (UE5's
  r.FXAA.Quality 1-5: presets 10, 13, 15, 29, 39), FXAASubpix, FXAAEdgeThreshold and FXAAEdgeThresholdMin (defaults:
  preset 39, 0, 0.063, 0.0312).
- Frame rate: the settings' FPS or MHGU_FRAMERATE (default 60, 30..240) writes the engine's target fps (float at +0x3c of the object in
  global 0x2326764, the field theboy181's 60fps patch sets) each frame. Above the monitor's refresh rate it needs
  VSync = Off: see "Frame rates above 60". MHGU_VSYNC=0/1 overrides the setting: no vsync / classic vsync.
- Open: a white flashing bar the user saw in pause menu > Status (not found yet: need a screenshot); lens flare
  blocks when first entering an area (draws skipped while shaders compile: much reduced, see "Fidelity audit").

## Fixed (2026-10-20): shimmer / light-shaft "triangle" / blocky bloom (pending the user's in-game check)

Symptom: flickering 16x8 blocks in bright areas, a wedge from the sun in hunts (light shafts), sparkle on sunlit stone.
Cause: AMD's 2020 Windows driver for GCN1 (R9 200) does not flush the colour-attachment cache for a barrier that only
says ALL_COMMANDS / MEMORY_WRITE (what vk_full_barrier and the render-pass dependencies used). The next pass sampling
the image just rendered read stale 8x8 tiles, mostly of the last-drawn triangle of a full-screen quad; lockstep dumps
hid it. Fix: barriers and dependencies name every stage and access type (vk.h VK_STAGES_ALL / VK_ACCESS_ALL*).
How it was found: F10 dumps in the hunt (light-shaft chain: pass 023 extract 960x540 -> 240x135, 024-026 radial blur
fs 7a142300 with sun position in cb4[0].xy and length cb4[2].z, offsets / weights in cb5, 027 upscale, 029 composite);
a CPU emulation of the blur matched the GPU except in those tiles; a standalone Vulkan replay (tools/vkreplay.c:
shaders from run/build/glsl, dumped input, the copy + blur ping-pong) reproduced the tiles on this GPU and
showed explicit barrier flags remove them. Ruled out on the way: clears (MHGU_CLEAR_DRAW / F9), depth / stencil,
alpha test / alpha-to-coverage, primitive restart, vertex buffer size, mip data, samplers, the 60 fps patch.
- The user's Game Bar videos are much brighter than the real image (capture encoding); on-screen pixels == "shot".
- Debug aids: F10 / "dump [n]" (all passes of n frames, depth-only passes as grey + raw float32), run-debug.bat,
  MHGU_DUMP_VERBOSE=1 (targets, pipeline state, sampler, 16 floats of the first fragment uniform buffers per pass),
  MHGU_CB_WATCH=<fs addr>, MHGU_SAMPLER_LOG=1, MHGU_TEX_LOD=n, MHGU_CLEAR_DRAW=1 / F9 (clears as draws).

## Fixed (2026-10-20): holes in dynamic shadows (pending the user's in-game check)

Hard shadows had straight-edged, camera-dependent gaps next to the sharp near cascade (a dinosaur in a hunt; villagers
in Bherna). Cause: the translator ignored condition codes. The receivers' shadow lookup (e.g. fs 7abfef00, the
ground, in the screen-space shadow-mask pass) picks a cascade with `FSET.CC RZ, |x - 0.5| < c[1][1]` then
`@P1 BRA NEU`: branch when cascade 0's y range (P1) AND its x range (CC) hold. Without the CC test, pixels inside
cascade 0's y range but outside its x range jumped with an unset depth reference -> no shadow. Now FSET .CC sets the
zero / sign flags, and BRA / EXIT / KIL / SYNC / BRK / CONT test them (translate.c cc_test; 26 village shaders use it).
Also fixed on the way: nvnCommandBufferSetDepthClamp was a no-op; the game enables depth clamp for the shadow-map pass
(now PipeKey.depth_clamp -> depthClampEnable).
Debug aids added: MHGU_GLSL_OVERRIDE=1 (run/glsl_override/<addr>_fs.glsl replaces a translated fragment shader),
MHGU_FORCE_WMASK=<fs addr> (all channels written), MHGU_DUMP_DRAWS=1 (every colour draw's program in a dump),
MHGU_SAVE_MSL=1 (instruction words as comments in saved GLSL), F8 (shadow-pass scissor: as set / none / mirrored).

## Local Play over IP (2026-10-20)

The game's Local Play (Switch local wireless: Multiplayer -> Play with People Nearby) works between PCs. The game
uses nn::ldn for the wireless network and plain nn::socket for its own traffic (one UDP socket, port 12345, with
broadcasts); MT Framework's nNetwork session layer sits on top (NEX is linked too, for online play only). Both
APIs are HLE'd at the function level, so no Switch service (ldn:u, bsd:u) is involved:
- runtime/net/ldn.c: nn::ldn. Create Hub -> the host listens on ini [Multiplayer] Port (default 39500, TCP) and
  answers LAN search probes on the same UDP port. Search Hubs -> Scan asks the LAN (broadcast + this computer) and
  every ini Join address (host[:port], comma separated) for the host's NetworkInfo. Joining keeps one TCP connection
  to the host: it assigns node numbers / addresses (169.254.x.n, the host n = 1), pushes NetworkInfo on every change
  (joins, leaves, advertise data) with the state change event signalled, and relays packets between players. The
  game's packets themselves go by UDP to/from the same port when that gets through ("MHGD" datagrams with a per-
  station token; station hello every 2 s keeps NAT mappings open), else - and after 15 s without UDP - through the
  TCP link. Only the host needs a reachable port (forwarded TCP + UDP; TCP alone works). Host gone -> the stations go to state Error (reason SignalLost / DestroyedByUser):
  the game says "Connection lost." and returns to offline.
- runtime/net/vsock.c: nn::socket on that virtual network (UDP datagrams, TCP emulated with SYN/SYNACK/data/FIN/RST
  messages - the link is reliable and ordered). The Switch's values: BSD socket options, Linux errno numbers,
  O_NONBLOCK 0x800. Nothing reaches the host's real network.
- Tested on one PC with two instances (run/ hosts, run2/ joins, 640x360, MHGU_LDN_PORT / MHGU_LDN_JOIN env
  overrides): hub created, found by LAN search and by Join address, joined, both see each other walk in the hub,
  Disconnect and a killed host handled. Not tested yet: two PCs, quests together, more than 2 players.
- The game's network library is Nintendo's Pia (statically linked; RTTI names nn::pia::local / inet / session /
  transport). Local and Online play share Pia's transport (stations, ReliableSlidingWindow, RttProtocol, keep-alive /
  life check); only the session layer differs (pia::local on LDN vs pia::inet on NEX). So Local Play already adapts
  to the measured round trip like Online does - there is nothing low-ping-only to swap out. What did matter: Pia
  expects lossy datagrams, and carrying them over TCP turns a lost packet into a stall of everything behind it,
  hence the UDP path. Tested with MHGU_LDN_LAG=<ms> (received packets held back; on both ends: round trip +2x) and
  MHGU_LDN_LOSS=<percent> (received packets dropped): 300 ms, and 800 ms with 5 % loss: join, both see each other
  move, still connected after a minute idle.
- Online Play entries: nn::nifm is HLE'd as "no internet connection" (runtime/hle/offline.c); Create Hub / Search
  Hubs / Search by ID go back to the menu (the system's error popup is not shown), Friend List is empty. Before,
  nifm's SDK code called a null function (crash).
- MHGU_LOG_NET=1 logs socket setup, =2 every nn::ldn / nn::socket call.
- Kernel fix found on the way: svcWaitProcessWideKeyAtomic re-took the mutex on timeout; Horizon returns without it
  and nn::os takes it itself, so every timed-out condition variable wait (TimedWaitEvent...) left the thread waiting
  on a mutex it owned - forever. Single player rarely times out (a thread or two hung unnoticed); the network
  threads do it constantly. MHGU_DUMP_THREADS dumps now show mutex owners and a heuristic list of callers.

## Internet hubs through a hub server (2026-10-29, branch feat/ldn-server)

Local Play over the internet without port forwarding, for friends (matchmaking key) and strangers (public), through
our own lobby / relay server (server/, Go, stdlib only; server/README.md: build, run, self-host, Docker, systemd).
Plan and security notes: docs/research/internet-hubs.md.
- ini [Multiplayer] Server (empty: LDN_DEFAULT_SERVER in config.h, the public server below;
  Off: none; host[:port], default port 39510) and Key (matchmaking key; empty: public). Both in the settings menu
  (Multiplayer page) and live (config_live_ldn; until 2026-10-29 the menu saved them but the game kept the start-up
  values): Scan / Join read them each time, a hub at each (re)connection to the server. MHGU_LDN_SERVER /
  MHGU_LDN_KEY env overrides for tests.
- What the player sees of the server (ldn.c hub_report): a notice at the top of the screen for 7 s (overlay_notice,
  drawn on ImGui's foreground draw list, so it shows with or without a menu open and takes no input) when the server
  is not found / unreachable / lost / busy / another version / refuses: hosting ("your hub is not listed online",
  once per failure, and "listed online again" when it comes back), searching ("only hubs on your local network",
  every search, but not twice within 10 s), joining (every failure; "the host did not answer" too). Texts are in
  settings_locale.h (UI_TEXT in ldn.c; tests/test_settings_locale.py checks them). The Multiplayer page has a
  "Check hub server" row: LIST with the server / key being edited (ldn_hub_check, a thread), showing "Online, N ms,
  N hub(s)" or the failure; before a check it shows the last state the game's own searches / hub saw for that server.
- Key -> room: first 16 bytes of SHA-256("MHGURecomp matchmaking key:" + key, spaces around it trimmed), all zero
  without a key (updater/sha256.c is linked into the game for it). The server lists a hub only for requests with the
  same room and link PROTO_VERSION; JOIN with another room answers "not found".
- Host (ldn.c lobby_thread): Create Hub also lists the hub (HOST with its NetworkInfo; UPDATE on every
  push_info_locked change; PING every 15 s; reconnects after 10 / 30 s). For each INCOMING ticket it opens a second
  connection (ATTACH), and the spliced socket goes to the same station_thread as a LAN connection. If the ini Port is
  taken, hosting with a hub server falls back to a system-chosen port (online only).
- Station: Scan asks the server (in a thread while the LAN is searched); a hub found both ways is joined directly.
  Connect to a listed hub = JOIN, then the usual F_HELLO handshake over the spliced connection.
- UDP through the server: datagrams prefixed "MHGR" + pair id + own secret (Link relay_pair / relay_secret); the
  server passes the rest to the other end. The host sends a bare prefix every second until the station's hellos come
  through, so the server knows where its end is. TCP fallback as before.
- Hardening on the way: every NetworkInfo from the network goes through info_sane (counts / sizes in range, strings
  terminated, node ids = index) before the game sees it; a WELCOME's node number is range-checked (was an out-of-
  bounds read); F_REFUSE results other than nn::ldn's become ConnectionRejected.
- Tested on one PC (server on 127.0.0.1, run/ hosts on port 39500, run2/ on 39600 so LAN search can't find it, key
  "friends"): hub listed, found by Search Hubs only through the server, joined (game packets by UDP through the
  relay), both see each other in the Hub, the listing follows the player count (1/4 -> 2/4 -> 1/4), a killed station
  is seen leaving, Disconnect delists the hub, other keys / no key see nothing. Server: `go test ./...` (keys,
  versions, splice, UDP relay incl. a forged secret, rate and per-IP limits, a host that never attaches). Not tested
  yet: two PCs / real NAT, a hunt through the relay, more than 2 players. Note the joiner needs the host's hub type
  unlocked (Hunters' Pub = G Rank).
- Public server (2026-10-29): morerokk-mhgurecomp-ldnserver.duckdns.org (DuckDNS name -> the VM's public IP), an Oracle
  Cloud Always Free VM (AMD micro, Oracle Linux 9). /usr/local/bin/mhgurecomp-hubserver (GOOS=linux GOARCH=amd64
  build), systemd unit mhgurecomp-hubserver (-v, DynamicUser, sandboxed), firewalld + VCN security list open 39510
  TCP / UDP. Log: `sudo journalctl -u mhgurecomp-hubserver`. Update: copy the new binary in, restorecon, restart.
  Checked from outside: list, join, splice, UDP relay (scratchpad hubcheck.py <host>), ~4 MB RAM.
- Two-instance test: scratchpad mp_start.ps1 (-port / -server / -key), hub_route.ps1 (loads the save; -mode host
  stops at Create Hub settings: A on "Hunters Hub", LEFT to "Wycademy Hub", A, X; -mode join opens Search Hubs),
  hublist.py <key> (what the server lists). The run\ save starts on the airship: ZL = Multiplayer menu.

## Event camera pans skippable with A (2026-10-21)

Entering a village / hub area when something is new plays an event (rScheduler, arc/ev/*.arc, path
event\village\event_vNNN...) that pans the camera past the NPCs; the original can't skip it. runtime/hle/evskip.c,
hooked at uScheduler's update (gen.py PATCHES 0xb66164): A sets a playing event scheduler's speed to 30 (uScheduler:
+0x34 frame, +0x3c speed, +0x40 paused, +0x41 loop, +0x54 rScheduler, path at +0x08), so it plays out in a moment
and what it does at the end still happens (log: "[evskip] skipping <path>"). Looping stage schedulers and paused
ones are left alone. Tested with a new character's arrival in Bherna (event_v002): A during the pan -> the chief's
greeting about a second later, the dialogue then advances normally. Other events (new facility / NPC pans, story
scenes with dialogue in the middle) not tested.

## Small monsters synced in multiplayer (2026-10-21)

The original game syncs small monsters' HP / deaths only; each player's game runs their AI, so players see them in
different places. runtime/net/emsync.c makes the first player in a zone their master (the game's own large-monster
mechanism: flag 8 sends netEmAction, the others replay it); ini [Multiplayer] SyncSmallMonsters (default Off since 2026-10-27:
players report synced small monsters that stop attacking them; not investigated yet).
Tested with two instances: positions agree within centimetres, hand-over when the master leaves works. Combat,
3-4 players and two PCs not tested yet. Research, data layout and test details: docs/research/small-monster-sync.md.

## Text entry (2026-10-21)

Text entries use the inline keyboard (nn::swkbd::InlineKeyboard: the only one seen called; the game draws its own box
and shows the text it gets through the changed-string callback). runtime/hle/swkbd.c + runtime/textinput.c: while
it is shown, the physical keyboard types (SDL text input: any layout, accents; Backspace, Ctrl+V paste), Enter
confirms (decided-enter callback), Escape cancels; the keys don't act as controller buttons meanwhile, and the
controller can confirm with Plus or cancel with B. AppearArg +0x20 = max length (names: 10); SetInputText gives the
current text as UTF-16 inline (re-editing starts from it). Callback argument: UTF-8 text at +0, length +0x7d4, cursor
+0x7d8. Invalid UTF-8 handed to the game hangs it (seen: its decoder loops), so only valid UTF-8 goes there. Tested
on character creation: typing, Backspace over an accent, the 10-character limit, re-editing, Escape; the Palico
name entry (same calls) and chat not tested. The full-screen keyboard applet (ShowKeyboard) is not used by the game
as far as seen; it still answers "Hunter". MHGU_CONTROL "type <text>" / "textkey enter|escape|backspace" for tests.

User words (fixed 2026-10-24): the game hands the keyboard the character's earlier entries as suggestions
(InlineKeyboard::SetUserWordInfo; its keyboard manager, state machine f_a62570, state 8) and waits in state 5 for the
released-user-word-info callback before Appear, giving up after 0x38e frames (~15 s: a dimmed screen, no box, and
Edit Name dead until a restart). The callback was never called, so item set names (and any entry with user words)
failed for characters with entries typed on a Switch; characters made in the port have none and went straight to
Appear. Now Calc calls it right after SetUserWordInfo. Red herrings on the way: the same dialog loads
nativeNX/arc/swkbd_eu.arc (not in the game's RomFS on a Switch either: "path not found", harmless), and the bug
looked Linux-only (it is per character: the Linux tester and the test save had Switch-made characters).

Hidden entry lock-up (fixed 2026-10-26): the 10-24 fix also called the released callback after UnsetUserWordInfo,
which the game's keyboard manager calls after every entry (state 9). The callback (f_a63d00) sets state 6, "words
back: show the keyboard", so every entry reopened 4 frames after it closed, with nothing on screen: the keys typed
into it instead of moving the menu (character creation right after the name), the manager stayed busy, and Escape
only closed it for it to reopen (the players' logs: "inline keyboard shown" again right after each "entered" /
"cancelled"). Unset releases nothing now (the words went back right after Set). Manager states (+0x2ec8 of the
keyboard object, f_a62570): 2/4 open (4: Set the user words -> 5 waits for the callback), 6 show (f_a62d88: input
text, cursor, Appear) -> 7 -> 8 shown until Calc returns 1 -> 9 (Unset) -> 3 ready; 10-12 finalize and re-initialize.

## Monster HP by the number of hunters (2026-10-21)

ini [Monster HP]: factors on large monsters' HP in Hub-type quests for 1-4 hunters (default 1 = original), plus
`Quest<number> = f1, f2, f3, f4` to set one quest apart. Only HP changes. A quest gives each large monster a row
of the difficulty table (resident.arc enemy\resident\em_nando_tbl, "NAN": u32 magic, version, row count, then rows
of 8 floats: HP, attack, defense...; cQuestData mVitalTblNo_N / mAttackTblNo_N / mOtherTblNo_N pick rows, and the
game moves the HP row a little per quest, e.g. 17 -> 16..19, with the size). Max HP = base HP (dtbase) x row[0], set in 0x2c04a0 /
0x2c04d8, both through the row getter 0x2baaac (monster, column 0 HP / 1 attack / 2 other), which is hooked
(runtime/hle/hpscale.c, fnhook): for column 0 it returns a copy of the row with only the HP float scaled. Village
quests are left alone by number (below 10000, Village Prowler 100000-109999); hunters = sQuestSelector's join count
(+0xd750). Multiplayer: each game computes HP itself, so the hub's host sends its settings (F_AUX, magic 0xE6) and
clients use them while in that hub. Data: sQuest *0x1a8ad90 (+0xa4 cQuestData, +8 quest number), sQuestSelector
*0x1a8ae38. Tested (two instances, Tackling the Tetsucabra, quest 10107, base HP 4000): solo x0.6 -> max HP 1392
(= 4000 x 0.58 x 0.6); two hunters, host 0.5 / client's own 3.0 -> both 1340 (= 4000 x 0.67 x 0.5). Village quest
exclusion and 3-4 hunters not tested in game. MHGU_LOG_HPSCALE=1 also logs the resulting max HP.

## Village quests in the Hub (2026-10-22)

ini [Village Quests] VillageQuestsInHub = On (default): at the Hub's or the Hunters' Pub's quest counter, L / R in
the rank menu or the star menu switch to the Village quests (the village counter's own menus: Training, Low 1-6,
High 7-10 stars, Urgent; what the save's village progress has unlocked) and back. runtime/hle/villagehub.c has the
mechanism: the counter's quests are sQuestSelector's list, built by a job from quest\quest_group through the filter
0x5b8090 with the area type (sGameControl +0x4c: 0 Hub, 1-6 villages - all Village quests are pooled -, 7 Pub) as
the counter type; categories and the counter UI's layout also go by the area type, so in Village mode those
functions run with it reading a village. A switch runs the list job again (0x5b7fd8 / 0x5b8070), builds the
categories anew (0x5ba8fc: they hold quest numbers, made from quest_group and the area type, not from the list) and
puts the UI back to its first state (+0x3c = 0). The counter UI sees a village only in its menus (states 0-2): with
the whole UI flipped, a Local Play hub's list came out empty (and the rows held the old categories' Hub quests).
Joining a posted quest looks its data up in the list (0x5ba50c): a quest from the other list is loaded on demand
(rQuestData loc\quest\questData\questData_<n>, resource manager *0x2326a64 vtable +0x30, mode 2) and appended -
before that the joiner had to switch to the same list ("You do not meet the requirements"). Quest numbers: group *
10000 + star * 100 + index (0 Village, 1 Hub, 2 Arena, 3 Village training, 4 Special Permit). HP: a Village quest
started from the Hub or Pub gets [Village Quests] HubHP (2.0) first, then the [Monster HP] factor for the number of
hunters (runtime/hle/hpscale.c; the host's values in a hub, sent with the others). Tested (2026-10-22, two
instances, the user driving): the switch both ways in the Pub and a Local Play Hub, the Village ★1 list in an online
hub, a village quest (101) posted by the host, joined by the client from the Quest Board, both departed together;
then (HR72 saves, both in the Pub) Vaulting Outlaw (206) posted from the Village list and joined by a client in Hub
mode (loaded on demand): Great Maccao max HP 860 in both games (row x0.43, x2.0 HubHP, x0.5 the host's 2-hunter
factor). Not checked yet: that clearing such a quest counts in the village (the clear is kept per quest number, so
it should), finishing a quest together, 3-4 hunters. Unlocks: a village's counter lists all Village quests of its
star ranges and only hides the locked ones in its menus (0x74af8c(quest, 0xff, 0) = unlocked in this save), and
the board's join only asks the list, so (a village never being online) nothing stopped joining a locked one. In
the Hub / Pub, locked Village quests (groups 0, 3, 10) are kept out of the list and out of the on-demand load: the
board then says "You don't meet the requirements to join this Quest" and shows it greyed. Tested (2026-10-23, host
HR72 hosting a Wycademy Hub, client a new HR1 save with only Village 1 star): Defiant Maccao (728, 7 stars) and A
Fungal Hunt (105, a 1-star quest the client hadn't unlocked) refused; Find the Ferns (101) joined. A Forge plugin
could do the same (same hooks), but the HP part is the port's own.

## Save data safety (2026-10-22)

Reported: saves in a folder symlinked to a NAS (Linux, Proton) were lost. The game saves by DeleteFile(system),
CreateFile(system, 0x4eb8bc), one 0x24 + one 0x4eb898 write, Commit, then the same for system_backup (MHGU_LOG_FILES=1
now logs every IFileSystem call). On the Switch nothing is real before Commit; the port deleted and created the
host files at once, and the SDK aborts the game on an unexpected fs result (nn::diag Abort, svcBreak: seen by making
the create fail). So any failed call between the delete and the commit (a share hiccup, a permission, a short write
that wasn't retried) killed the game with `system` already gone; at the next start the game finds no `system`,
makes a new empty save, and that also replaces system_backup. Fix (runtime/services/fs.c): save mounts are
transactional (new content in <name>.mhgu-tmp, deletes only marked; Commit renames to .mhgu-commit, then copies it
over the real file in place, so symlinked files stay symlinks; first mount finishes a .mhgu-commit and drops a
.mhgu-tmp; an unmount without commit drops the changes), reads and writes loop until complete, and failures are
logged. Tested: new save flow (4 commits, save loads again), leftover .mhgu-commit applied / .mhgu-tmp removed, a
create made to fail after the delete (game aborts as on the Switch, `system` keeps its committed content). Not
tested: a real NAS, Linux, Proton.

## Frame rates above 60 (2026-10-21)

ini [Performance] FPS = 30..240 (default 60), VSync = On / Off (default On). The engine's own limiter (main
0xda5d00) sleeps to the target in its system object (+0x3c; the 90 / 120 fps cheat codes, `[main+0x18A6210]+0x243C`,
are this same float) and gives game logic the measured frame time as a step in 60 fps frames (+0x68: 1.0 at 60, 2.0
at 30, ~0.42 at 144; capped at +0x70 = 16). There is no fixed timestep: one game update per presented frame.
The old "cap at 60 somewhere in the engine" was the presentation: the R9's driver has no mailbox mode, so VSync
falls back to FIFO and waits for the 60 Hz monitor. With VSync = Off (immediate present, tearing possible) the
village at 640x360 runs ~138 fps at a 144 target on the R9 (CPU-bound there). Walking speed checked at 30, 60 and
~133 fps (player position 2 s apart: ~355 units each time): normal game speed.

Theory, not tested: Pierce ammo hitting more often at high frame rates. Anything the game counts per update rather
than by the step behaves differently above 60 (and at 30 the other way). Two likely ways for a piercing shot: (1) a
per-target hit cooldown counted in updates: at 144 fps it would run out 2.4x sooner in real time, so the shot can
hit the same hit zones again as it passes through; (2) collision is tested once per update at the shot's new
position: smaller steps sample its path more finely, so fewer hits are skipped on thin parts. Both mean more hits per
shot, but only up to the ammo's hit count, so damage would rise towards the most a shot can do rather than without
bound. Other per-update counters (multi-hit attacks, status ticks) could act the same way. To check later: a save
with a bowgun and Pierce ammo, the same shots into a still monster at 60 and at 144 fps, and count hits (a hook on
the damage function, or the damage numbers); the shell data (shell\pl\pl_w<weapon>_<n>\..._hit<n> files,
rBowgunShellData) would show which field holds the interval. A fix would scale that counter by the step.

Monster AI and the frame rate (2026-10-25): measured, no difference. f_276154(monster, group, index, flag) starts an
enemy's new action (stores it at monster +0x73e0, sends it to the other players) for small and large monsters alike;
testsave/ai_rate.sh counts its calls per monster with MHGU_FNTRACE over 120 s of the hunter standing still. Quest 201
zone 1 (three Larinoth): 52 / 52 / 51 actions at 30 / 60 / 120 fps, 17 / 18 / 17 per monster each time (the AI's
timers run on the step, not per update). Quest 305 (two Great Maccao plus small monsters, all over the map): 340 and
311 at 30 fps, 270 and 313 at 120 fps (the Maccao 70-87 each in every run): noise, nothing rising with the frame
rate. Not measured: what happens inside an action (turning toward a target, collision, hit cooldowns per update).

Monster jumps and the frame rate (2026-10-27): fixed. Reported: Volvidon's body press (it leaps at the hunter and
lands on its back) far too fast, faster the higher the frame rate. Some actions move the monster with the velocity and
acceleration in its status block (*(monster + 0x1428) = monster + 0x1440: +0x40 position, +0x10 velocity, +0x20
acceleration) and integrate them once per update with no step: `pos += vel; vel += acc; acc.y += -0.2`, tuned for the
Switch's 30 updates a second. Measured (Volvidon, actions forced with emact, MHGU_EMTRACE=2): body press 7:92 / 7:93 at
30 fps in the air 1.0-2.2 s, 900 up; at 120 fps the same 900 in 1.0-1.4 s; everything step-driven around it
(wind-up, lying on its back) the same, so the whole action lasts as long - only the jump itself plays 4x fast. All
121 group 7 actions were timed at 30 and 120 fps: the rest last the same. The writer was found with interpwatch
(below): 0x1122e98 / 0x1122f34 (rising / falling), called with r0 = the status block. The same inlined integrator
(searched for: vel += acc stores on a status block) is in four blocks: 0x1122b40 / 0x1122b90 (Arzuros / Lagombi /
Volvidon hops, vertical only), 0x1122e3c / 0x1122ed8 (body press), 0x10f6ea8 (Zinogre, no jerk term). Fix
(runtime/hle/emjump.c, tools/gen.py EMJUMP): their 26 stores advance the sequence by s = step / 2 updates (1 at 30 fps;
at most 1) with Newton's forward differences, exact for that cubic sequence, so the monster is where the 30 fps game
has it at every 30 fps frame. Checked: 7:92 / 7:93 at 120 fps now in the air 1.05-2.2 s, apex ~900 at 1.7 s, as at 30
fps. MHGU_NO_EMJUMP=1 turns it off (for comparing). Tested by the user at 120 fps: the body press looks right now.
Volvidon's 7:98 (a hop, then a drop of ~5000) is the same code: at 120 fps it now follows the 30 fps curve within a
few units (before: fell 3-4x fast). Other position updates, read (2026-10-27): a looser search (a store to [rX,
#0x44] with a load of [rX, #0x14] and an add within 8 instructions before it) also finds 0x27b39c, 0x27b624,
0x27c754 (shared monster code), 0x10f740c, 0x10fa484, 0x11002dc, 0x129de8c: `pos += vel * step`, the step being the
monster's own +0x1c (2.0 at 30 fps, 0.5 at 120: the engine step); 0x1242e5c, 0x12fbb68, 0x1313c50, 0x11d1354: also
scaled; 0x11bcd84 / 0x11bd044 set the height from a curve function (not accumulated); 0x21a554 is stack temporaries.
None unscaled. Duramboros (suspected by the user: a jump like Zinogre's): quest 404 moved to the Arena with em055
(test mod DuramborosArena, the spawn from q0010749's b_m09em055_00), all group 7 actions (0-51; its AI only uses
groups 1, 2, 7) swept at 30 and 120 fps with positions every update: every action lasts the same, and none rises or
falls fast at either rate (fastest ~800 units/s; Volvidon's body press: 2900 at 30 fps, 7600 at 120 before the fix).
Not covered by the search: gravity added per update to a velocity whose position use is scaled (jumps would come out
lower and shorter at high frame rates rather than faster) - the vertical speed comparison would show that too.
Tools for the next suspect: scratchpad-style test mods (quest 404 in the Arena with another em), MHGU_EMTRACE=2 sweeps
at 30 and 120 fps, then compare the fastest vertical speed per action.
Test setup (scratchpad, not in the repo): quest 404 (Hunt a Volvidon) moved to the Arena by a test mod (stage byte
+0x14 of questData_0000404 = 9 in its arc and in loc\arc\quest\vq.arc, the Village list's copy the counter takes the
stage from; the Arena's b_m09em062_00 spawn), MHGU_QUICKHUNT=404, then the hunter walks from the waiting room (north
2 s, east 2.7 s, north through the gate: an area change, so the hunter can't be placed there by poking its position).

Buff icons and the frame rate (2026-10-28): fixed. Reported: with two or more buffs the icon beside a name on the HUD's status plates (yours, the other hunters', the Palicoes') takes turns far too fast at 240 fps. The plates (cUIOStatusPlate / cUIOStatusPlateGuests, update 0x834314) call 0x834d00 once per update for each of their two icon slots ({index, countdown, shown} at plate +0xf0 / +0xfc; tables of 54 and 36 icons at image 0x1839250 / 0x18395b0, tested against the player's buff flags +0x2840..+0x2864, rebuilt from the buff timers each update at 0x49f0xx): the countdown drops by 1 per update and is reloaded with 20, 0.67 s at 30 fps, 0.08 s at 240. Fix (runtime/hle/buffcycle.c, tools/gen.py PATCHES): the decrement (0x834d20) takes step / 2 updates (at most 1) in 1/65536 units and the reload (0x834ef4) is 20 of those. Checked at 240 fps (test save, quest 101, two buffs from their timers: player +0x287c and +0x2880 poked to 3000): the two icons change every 0.667 s, 12 times in 8 s.

KO (stun) and the frame rate (2026-10-29): not found yet. Reported: KO from head hits with blunt weapons much harder at
high frame rates (a 240 fps player: Alatreon hammer solo, horns broken early, 0-1 KOs). Suspected status decay counted
per update: it isn't. Status data (dt_tune, rEnemyDtTune; monster +0x75ec -> the resource, entry pointers at +0x98:
poison, sleep, paralysis, stun, exhaust, mount, blast, ...): per status s16 initial threshold, decay interval (60 fps
frames), decay amount, duration, increase per KO, max (poison two more). The status block *(monster + 0x1428): stun
buildup +0x5d76, threshold +0x5d78, decay timer f32 +0x5d7c, KO count +0x5d80, stunned bit 0x10 of +0x5e08, time left
f32 +0x5e0c. A head hit adds KO x part multiplier / 100 (at least 1) to +0x4d2 (0x29db54); the monster's update applies
it (0x29a34c -> 0x2a2e18: buildup += it, decay timer reloaded; at the threshold the KO starts, 0x2a2f0c, unless one of
the u8 counters +0x3a2..+0x3a4 (set to 2 by 0x2c0410 / 0x2c0438 / 0x2c044c, -1 per update at 0x2c957c) or vtable
+0x39c (overridden by 3 monsters: 0x110d8f4, 0x11c266c, 0x11e3298) holds it back at threshold - 1); after a KO the
threshold rises (0x2a2f80). Decay (0x2a398c, all statuses) and the KO's duration (0x2c9254) count down by the
monster's step (+0x1c, via 0x272128 / 0x73aa84). Measured (Arena Volvidon, buildup poked to 90): 5 every 10 s at 30 and
at 120 fps; a KO (pending KO poked) lasts 10.0 s at 30 and 60 fps. Next: MHGU_KOTRACE=1 (runtime/hle/kotrace.c) logs
every hit's KO, the buildup / threshold, KOs held back at the threshold, KO start / end and decay, for a player's
30 vs 60 fps hammer session. Debug aid added: the control file's "interpread <lo>:<hi> <file of entries>" (interp.c:
logs the instructions of the listed functions and their callees that read a guest range).

Two-button inputs and the frame rate (2026-10-28): fixed. Reported: the Hunting Horn's X+A (and the Lance's R+X+A)
need both buttons almost together at high frame rates. The player's input update (0x47ca28, every weapon; called
from the player update at 0x4a7dec) holds a new attack-button press (the key config's mask from 0x85f93c, | 0x1b0)
to see whether another joins it: bit 0 of the player's input bits +0x1444 and a countdown +0x1448 = 2.0
(0x47d558), less 1.0 each update (0x47ca84 `vmov.f32 s2, #-1.0`, 0x47ca90 `vadd`); at 0 the press counts alone.
Measured (MHGU_LOG_MOVES, quest 101 with the user's HH, X then A d frames later): at 30 fps X+A (0010:006) for
d <= 2, X alone (0010:000) 2 frames after the press otherwise; at 120 fps the same 2 frames (17 ms). The command
tables (rPlBaseCmd: act_st / act_no / act_param, cmd_type, cmd_param0..3; cmd_type 8 = key param0 just pressed)
read logical keys the weapon's key function (0x1360058 and its siblings, one per weapon) sets from +0x1444: held
+0x1414, pressed +0x1424 (0x485e2c / 0x485e78 test, 0x485de4 / 0x485e54 set). Fix (runtime/hle/combowin.c, tools/gen.py
PATCHES): the constant is -step / 2 updates (at most 1). Checked: at 120 fps X+A for d <= 8 and X alone 8 frames
(67 ms) after the press; 30 fps unchanged. A lone attack button is read 67 ms after the press at any frame rate,
as on the Switch (it was 17 ms at 120 fps).

## Fidelity audit (2026-10-20)

Silent-bug sweep after the shimmer and shadow fixes (the two classes found: a driver needing explicit barriers, and a
translator ignoring condition codes). Diagnostics now built in: MHGU_NVN_NOOP_LOG=1 logs the first call of every NVN
command the port ignores; the translator warns when a branch tests zero / sign flags last written by a .CC
instruction whose flags it doesn't model ("[shader] ... condition-code test"); MHGU_VK_STATS=1 counts skipped draws.
Village run + the user's hunt logs: the game calls only nvnCommandBufferAddControlMemory and SetShaderScratchMemory
(both harmless); no CC warnings, translation failures or unsupported formats. Audio effects are now complete for
MHGU: I3DL2 reverb (type 5; mix 1 chains three mono I3DL2 reverbs with buffer mixers, the environmental reverb) is a
port of Ryujinx's Reverb3d, with yuzu's max + 1 delay-line ring (Ryujinx's re-reads a stale sample every 400 ms); Aux
(type 2, 6 channels on the final mix) sends each frame into the game's guest ring and plays its return, as Ryujinx's
AuxiliaryBuffer: the game keeps up in lockstep (MHGU_AUDIO_LOG=1 logs the ring offsets; MHGU_NO_AUX=1 bypasses it).
Splitter updates skipped the 12 unused bytes after each splitter's destination ids, so everything after the first
splitter in an update was dropped: stereo sounds made of two mono voices (menu confirm, load fanfare) played left only.
docs/SWITCH_COMPARE.md lists what to check against the Switch version by ear.
Draws skipped while their shaders / pipeline build (the main remaining cause of transient glitches: a flashing status
bar, missing lens-flare pieces on entering an area). MHGU_VK_STATS=1 now also logs a "2s builds" line (skips by
cause and in how many frames, the waits, build times per stage, request -> ready latency, the slowest frame). Entering
the village needed 112 new programs and ~140 pipelines at once: translation (one thread, the translator isn't
re-entrant, ~2.3 ms each) + GLSL -> SPIR-V / modules (~3.6 ms, three workers) dominate, pipelines take ~0.2 ms (Vulkan
pipeline cache), so ~1100-1230 draws were skipped over 13 frames. Now (exec.c):
- a draw whose program / pipeline is still building waits for it, up to MHGU_PIPE_WAIT_MS (default 6) per frame in
  all, and its job moves to the front of its queue;
- cache/programs.bin keeps every translated program (GLSL + reflection, keyed by a hash of the guest code the
  translator reads + the shader keys; tied to the translator: the Makefile's SHADER_ID, a hash of
  runtime/nvn/maxwell/translate.c + shader.h, and exec.c's CACHE_FORMAT, so updates that leave the translator alone
  keep it, as Ryujinx's cache does; until 2026-10-28 any rebuild started it afresh), and
  cache/pipelines.bin each pipeline's state (program by content key, render pass by formats). The next run builds them
  all on the workers at its first draw (~130 programs + ~170 pipelines: well under a second, during the logos), like
  Ryujinx's shader cache. Both are appended as they grow, so a killed run keeps them. MHGU_NO_PROGRAM_CACHE=1: off.
Village load on the R9 (warm caches): 0 skipped draws on entering (was 1064 / 1230), 6 in a whole 60 s run (the first
logo frames), slowest frame 56-62 ms (was 83, the load hitch itself), steady 57 fps unchanged. First run ever (empty
caches): 761 skipped over 9 frames (was 1230 over 13): translation throughput bound, waits cap at 6 ms a frame.
Known gaps that remain:
- A program never seen before (first run, or after the translator changed) can still be missing for a few frames when many arrive
  at once (fewer since the start-up prewarm: next section).
- R9 200 approximations: no custom border colours (nearest standard colour), D24S8 stored as D32F_S8 (polygon-offset
  units differ for decals).

## Fixed (2026-10-28): black quest-end photo; shader prewarm at start-up

The Quest Rewards / Reward Box screen shows a photo of the moment the quest was cleared. A player saw it fully black
on a first run, with the user's screenshot also showing junk blocks. How the game does it: sScreenShot (cSystem,
setup 0xa6f854 at quest start) makes its buffers (nScreenShot::Buffer, 0x2c bytes; Buffer::create 0xe48814: an
rTexture of the 3D size / 1, 2 or 4); at the clear, uFestaScreenShot (0x7628d8) calls the request 0xa6f97c, and the
unit's draw copies the scene once into the buffer: one draw, never repeated. The reward screen draws the buffer each
frame (pass vs 77e9f300 fs 79c6ef00). Evidence (R9, Harvest Tour: Jurassic Frontier, quest 11119, finished by
delivering the Paw Pass Ticket from the camp's supply box): with empty caches the capture draw (vs 78515c00 fs 7a2ac600,
into a 960x540 RGBA8 target) was skipped two frames after the delivery, its program needed 9 ms and the per-frame
wait budget is 6 ms; the buffer was never drawn, and a texture never drawn is read from its guest memory (zeros or
whatever the pool held before: black, or junk). With that program and its pipeline in cache/ (the next run) the photo
was right.

Fix: prewarm every program at start-up (exec.c "prewarm", backend_program_created). The game makes all its programs
before its first frame (nvnProgramSetShaders: ~6300 vertex / fragment pairs, 5589 distinct; the shader code is in GPU
memory by then, so nothing is read from AppShaderPackage.mfx). Each one not in cache/programs.bin is translated with
the keys most draws use (float vertex inputs and colour targets, no alpha test, the game's window origin / depth mode)
on the translate thread and its SPIR-V compiled into cache/spirv on the workers, all behind any build a draw asked for
(low-priority job lists; a draw waiting for one promotes it), recorded in programs.bin (the sources in programs.src)
and left dormant (state 3): the first draw that needs one loads its sources back and makes its modules from the
SPIR-V cache in ~1-3 ms. At start-up only the programs earlier runs drew (those with pipelines in pipelines.bin) are
built; the rest stay dormant. Draws never wait past the 6 ms per-frame budget (942d7ee's 2 s wait for a target's first
draw is gone): a program the prewarm has not reached yet is skipped as before. Measured on the R9 (960x540 window, hidden):
- Cold (empty cache/): the prewarm takes 16-18 s from the first program (5581 translated, 730 duplicates, 0 failed),
  done ~29 s after launch, about when the opening movie starts (the health notice is up from ~18 s). Opening movie at
  27.9 s vs 26.4 s without the prewarm; 48-58 fps while it runs (the no-prewarm start ran 36-46 fps over the same
  stretch: this machine's fps is noisy), slowest frames 135 / 124 / 91 ms in the first seconds.
- Warm: programs.bin (the index, 20 MB) read in 42-47 ms at the first frame (with the sources in the same file it took
  650-1140 ms when the OS file cache was cold: hence programs.src), the prewarm's check of all 6311 programs 0.8 s in
  the background, 5-6 skipped draws in the first frames; opening movie 26.0 s vs 26.3 s without the prewarm (this
  machine varies 22-28 s between runs).
- Disk: programs.bin 20 MB, programs.src 164 MB (GLSL), cache/spirv 157 MB (2388 shaders). Memory: +28 MB working
  set (609 vs 581 MB). Building the modules of every program at start-up instead was measured and rejected: 2.0 GB
  working set.
- Photo on a cold cache: the capture program came from the prewarm (dormant), ready 1 ms after the draw asked, 0
  skipped draws at the clear, photo right.
- Skipped draws over the whole test route (boot, Pub, quest in Jurassic Frontier, reward) on a cold cache: 379-385 in
  21 frames (was 476 in 36). What remains: the first frames of the boot while the prewarm runs; area loads, where ~70
  dormant programs and ~100 pipelines are needed in the same frame (1-4 frames of skips, need -> ready 1-4 ms average
  instead of 35-200 ms); variants with other keys (integer vertex inputs: skinned models; alpha-tested materials, ~15%
  of the programs draws use) are translated on first use as before; new pipelines on a cold driver cache.
Not covered: draws of the scene itself skipped in the very frame the photo is taken stay missing from it (a "half
black" photo may be that; not reproduced). Debug aids: MHGU_SKIP_LOG=1 (each skipped draw with its shaders and colour
target), the ini's [Performance] ShaderPrewarm = Off (default On; a restart setting, in the F1 menu's Graphics & Audio
page) or MHGU_NO_PREWARM=1, the prewarm's progress in MHGU_VK_STATS's 2 s builds line ("prewarm N new, N cached, N
queued") and a "[shader] prewarm:" line when it is done. MHGU_QUICKHUNT from the airship (an HR save in the Hub)
crashes (null function in the town's depart path), so the test route walks: scratchpad scripts hstart.ps1 (scratch
MHGU_HOME / save), to_pub_quest.ps1, to_depart.ps1, to_box.ps1.

Follow-up (2026-10-29): a Steam Deck player (0.10.8, warm program cache but only 14 pipelines in pipelines.bin) saw
the photo garbled, often: stripes of other textures (wood, clouds, ground) behind an intact reward UI. Same cause, a
skipped capture draw; RADV hands the never-drawn buffer recycled memory where the R9 gives zeros. The prewarm makes
programs, not every pipeline, and a dormant program still needs 1-3 ms to load: the 6 ms budget can be gone by the
capture in a busy clear frame. Reproduced here with MHGU_PIPE_WAIT_MS=0 and no pipelines.bin / vk_pipelines.bin: the
capture (vs 78515c00 fs 7a2ac600, 960x540) skipped for its program, photo black. Fix: tools/gen.py hooks the request
(0xa6f9ac, the `mov r1, #1` stored to sScreenShot+0x2c) to backend_photo_requested (exec.c); for PHOTO_FRAMES (8)
frames after it every draw waits for its build up to PHOTO_WAIT_NS (2 s) instead of being skipped, so the capture and
the scene it copies are complete (also covers the "half black" case above). Same test with the fix: one "[vk] quest
photo requested" line, 2 waits (2 ms) at the clear, 0 skips, photo right. The reward screen's own draw of the buffer
still skips its first frame (redrawn every frame: unseen).

Shipped pipeline list (2026-10-29). Surfaces black for a moment on entering an area the first time, with the prewarm
done: not programs missing but pipelines (a program with the draw's blend / depth / vertex layout / target formats:
Vulkan 1.2 bakes all of it in; NVN has it as state) and dormant programs loading, past the 6 ms budget. The game
never lists its combinations, so they are recorded: extras/pipelines.list (make copies it beside the executable,
dist ships it) holds the pipelines recorded while playing the common areas (first: the user's town and three hunts,
904). A record names its program by the game's shader code hashes (code_hash) and shader keys, not by a translated
program, so it outlives translator changes (unlike pipelines.bin, tied to SHADER_ID) and other GPUs: DEPTH24_STENCIL8
and vertex formats are made again for the GPU at load (D32S8 on AMD; widened vertex formats). Records whose program is
in programs.bin are attached at start-up like pipelines.bin's; the others when the prewarm meets their shader code
(plist_seen on the translate thread, plist_drain on the render thread makes the variant and its pipelines). The
header checks sizeof ShaderKey / PipeKey / ListRec: a change there needs a new recording. Matches only the settings
it was recorded with for MSAA (Off) and anything else in PipeKey. Measured (R9, empty cache\, quick start into quest
101 with the user's save, skipped draws over the same 40 s): 1398 without, 163 with (the quest area's load 1225 -> 51;
what is left is the town's first frames while the prewarm still runs); the next start 6. Recording more:
MHGU_PIPELIST_EXPORT=<file> writes pipelines.bin's records plus the loaded list's at the first draw, then copy it
over extras/pipelines.list (MHGU_PIPELIST=<file>: read another list; MHGU_NO_PIPELIST=1: none).

## Updater (2026-10-28)

`mhgurecomp-updater(.exe)` (updater/, built by `make` beside the game) is a separate program that is started instead
of the game. It installs GitHub's latest release over its own folder when it is newer, then starts the game. The game
itself has no networking for this and never changes: `mhgurecomp` always just starts the game.
- Flow (updater/updater.c):
  - It reads `[Application] Updates` from mhgurecomp.ini. `Ask` (the default) or `Auto` checks for an update, `Off`
    goes straight to the game. The setting is a g_settings row and is in the settings menu; the game ignores it.
  - It asks for `GET /repos/morerokk/MHGURecomp/releases/latest`, which never returns drafts or pre-releases (the
    `draft` / `prerelease` fields are checked as well), with a 5 s timeout and the last ETag (`update/latest.etag`,
    `latest.json`: a 304 reuses the saved JSON).
  - It compares the tag numerically with its own MHGU_VERSION and only installs a strictly newer one.
  - It picks the asset by name prefix (`Windows-`, `ArchLinux-` / `Linux-`; `.tar.gz` before `.zip`), downloads it to
    `update/<asset>.part` and checks the size and the `sha256:` digest GitHub keeps per asset. An asset without a
    digest is not installed.
  - It extracts to `update/new/`, dropping the archive's single top folder. Unsafe paths (`..`, absolute, `:`) are
    refused, and the player's own files are never written (`data mods cheats bonus cache shots save update
    mhgurecomp.ini log.txt`).
  - It moves each file it replaces to `update/backup/` and renames the new one in. On a failure everything is put
    back. update/backup and update/new are removed at the next start; Windows can't delete the running updater's own
    exe and SDL3.dll, but it can rename them, which is how the updater replaces itself.
  - Before installing it refuses if the game is running from that folder (Windows: the exe or game DLL can't be
    opened for writing; Linux: /proc/*/exe).
  - On Linux it sets 0755 on the programs and `.so` files, taken from the tar's modes or by name, since a zip made on
    Windows has none.
  - It starts the game with the arguments it was given, minus its own (`--update-force`, `--update-no-launch`).
    Windows: CreateProcess with CREATE_NO_WINDOW, so the game logs to log.txt as it does from Explorer, then waits for
    it and returns its exit code, so Steam sees the game run. Linux: execv, the same process.
  - No connection, GitHub's rate limit (403/429; 60 an hour unauthenticated, and a 304 still counts), no asset or
    digest, or "Not now": it starts the game without a word. A failed download or install shows the reason, then
    starts the game after 10 s.
- Version: the Makefile's `VERSION ?= git describe --tags` goes into the updater and the game's log ("MHGU Recomp
  <version>", runtime/main.c). Only an exact tag (`0.10.4`) updates itself: a build between tags or `unknown` is left
  alone unless `--update-force` is given. `make dist VERSION=x.y.z` packs the release (tools/dist.py): a .zip for
  Windows, a .tar.gz for Linux (modes kept). It refuses a VERSION that isn't x.y.z and an updater built with another
  version.
- HTTPS: WinHTTP on Windows; on Linux the system's libcurl loaded with dlopen (libcurl.so.4 /
  libcurl-gnutls.so.4), retried with the usual CA bundle paths when a runtime's libcurl can't find its own. Archives:
  zlib (static on Windows, libz.so.1 on Linux). The updater needs glibc 2.34 at most and is checked like the game
  (GLIBC_CHECK). Window: SDL3 with SDL_RenderDebugText, shown only when there is something to say (after 0.7 s of
  checking, or a question); keyboard, mouse and controller (South: choose, East / Back: not now). Without a display it
  installs in Auto mode and doesn't ask in Ask mode.
- Tested 2026-10-28 on throwaway copies:
  - A 0.9.0 updater installed GitHub's latest release on Windows (zip) and on Linux in WSL (zip on ext4: the
    programs came out 0755). The installed files were byte-identical to the release, and the player's files were
    unchanged.
  - A local fake GitHub served a `make dist` 0.99.0 release (Windows zip, Linux tar.gz). The 0.9.0 updater replaced
    itself, then reported "up to date" on the next run and removed the backup.
  - Correctly refused or skipped: wrong digest, no digest, draft, 403, closed port, game running, newer than GitHub,
    development build. When 0.10.3 was turned into a pre-release mid-test, the updater took 0.10.2.
  - Arguments with spaces, quotes, trailing backslashes and empty strings reach the game unchanged, and its exit
    code comes back. A second check got a 304 from GitHub.
- Not covered: a release that drops a file leaves the old copy behind, and a new `bonus/system` is never installed,
  since bonus/ belongs to the player.

## Fixed (2026-10-28): Linux sleeps were busy loops

Reported (Fedora, Intel N100, 4 cores): all four cores saturated, 40-50% of it kernel time, not smooth at any render
size. plat_sleep_until_ns (runtime/platform/linux.c) gave clock_nanosleep(TIMER_ABSTIME) the time left as if it were
the absolute deadline: long past, so it returned at once and every sleep spun on syscalls until its deadline. Every
sleeping thread did this: the 60 Hz vblank timer (hle/vi.c), svcSleepThread (every game thread that sleeps), Local
Play. Fix: sleep until 200 us before the deadline, then yield as designed. Measured (WSL, 200 sleeps of 5 ms): before
60% of a core (0.5 s sys a second), after 2%, still on time (1 us late on average, 42 us at worst). Windows was not
affected (its own sleep). Since the Linux port began; a 4-core machine shows it the most.

## Next steps
1. Modding tools: decode resident.arc tables (weapon00..14 Base/Level data: record layouts derivable from the game
   code that reads them), monster files per `arc/enemy/emXXX_YY.arc` (`attackdata`, `bodydata`, `dtbase`,
   `dttune`), JSON <-> binary converters, and per-member ARC overrides in the virtual RomFS
   (`mods/<name>/romfs/<arc path>.arc.d/<member>`).
2. Play further (hunts work: Bherna Gal -> quest -> gate left of the balloon) and fix what breaks; shader global
   memory if some effect needs it.
3. A README with build instructions; Linux platform layer (runtime/platform/linux.c). It must also give
   plat_user_dir a home for save data: $XDG_DATA_HOME/MHGURecomp, else ~/.local/share/MHGURecomp (see "Save data").
4. Internet hubs: the hub server and client exist, and the public server runs (see "Internet hubs through a hub
   server"). Left: test across two PCs and real NATs, a hunt, 3-4 players; measure the
   relay's traffic per player; direct UDP by hole punching (relay as fallback); the rest of the security audit
   (vsock.c, emsync.c, hpscale / villagehub aux messages, IPC / fs boundary) before public hubs are advertised:
   docs/research/internet-hubs.md.
5. Android build (arm64, controller only, no touch controls): plan in docs/research/android.md.
6. Draw distance ([Graphics] DrawDistance, 2026-10-28): town NPCs (uLobbyNpc, the 15 people in Bherna) fade in
   within a distance and out beyond it (uLobbyNpc 0x8c2d9c: squared distance to the object at a global +0x54 against
   the table at 0x8c2ef0, 2500^2, or 1800^2 with flag 0x40 of +0x186a; fade value +0x1894, copied to the model's
   alpha +0x158). gen.py PATCHES at the load 0x8c2e34 -> config.c npc_fade_dist2: the factor squared times it, 0 =
   no limit; live (settings menu, Graphics & Audio page). Hunts are untouched (monsters are not uLobbyNpc). Found
   by diffing NPC objects at five player positions (+0x158 / +0x1894 1 near, 0 far). Checked in game (R9, 960x540,
   Bherna arrival view): at 2 and 0 the people far up the path are drawn, at 1 not (as the game). Cost: uncapped
   (VSync Off, FPS 240) ~119 fps at 1, ~113 at 0 (+0.4 ms); VSync Off at 60: a steady 60 at 0. With VSync On it
   fell to 33-45 fps: this driver has no mailbox, so VSync On is FIFO, and the game paces to vi.c's own 60 Hz timer,
   not the display's, so a frame that ends a little late waits a whole vblank (a pacing problem any heavier scene
   hits, not the NPCs' cost). Not looked at: gathering spots and other map objects, Hub NPC classes.
   Speech bubbles (reported 2026-10-28: the "new dialogue" bubble over an NPC still used the game's distance): the
   bubble has its own copy of the check (0x8c11f8, table at 0x8c1328, then 0x938530 puts the bubble above the head);
   its load 0x8c127c gets the same npc_fade_dist2. Checked (the user's save, MHGU_QUICKHUNT=town, Bherna arrival
   view): 1 bubble at 1 (as the game), 5 at 0, each on its NPC.
7. A settings launcher for players who don't edit ini files: every [section] / key of runtime/config.c (it already
   knows the names, defaults, ranges and comments) in a window, saved to mhgurecomp.ini. Cheapest: inside the exe
   with the ImGui the port already ships (a `--settings` switch, or a "Settings" shortcut), so no second program or
   toolkit; the same panel can serve item 8 and Android's controller-only settings (docs/research/android.md).
8. Look into the port's options in the game's own Options menu. The menu is the game's GUI (.gui layouts, gmd text,
   per-item code), so adding entries means extending its item tables, text and handlers: find how the Options list
   is built (fnnew while opening it, the gmd its labels come from) and judge the effort. Fallback: the port's own
   overlay panel (item 7) opened in game with a key / controller combination; settings that need a restart
   (resolution, MSAA) say so.
9. Other aspect ratios. Ultrawide is in (PR #6 by dmitsuki, 2026-10-25, then fixed up): [Display] AspectRatio
   (Auto: the window's), FOV, UIAnchor (Center / Edges / 21:9 / 32:9). Wider than 16:9: the 3D cameras made for the
   screen (aspect 16:9 in sCamera / uCamera +0x38) get the picture's shape (Hor+, gen.py PATCHES at the three
   callers of the perspective matrix 0x9c617c that load it; the other 9 callers are untouched), the game's 2D
   canvas gets the HUD's width (0xdbcd48 / 0xdbcd54), and exec.c (cbuf_info, vertex stage cbuf 3 of 2D draws)
   unstretches the HUD and places its parts left / right / center by their layer depth (94 center banners, 96
   dialogue box left, 97 menus by x, 100 / 105 right, 103 status left / item bar right). From 16:10 to 16:9
   (2026-10-25): the camera keeps 16:9's width and gets more height (Vert+: config.c camera_fovy, from the
   perspective matrix's fovy argument, radians: fovy' = 2 atan(tan(fovy / 2) * (16/9) / aspect)), and exec.c
   flattens the HUD to its 16:9 shape (sy) and moves each layer to the top (status, minimap, menus), the bottom
   (dialogue, item bar, Hunter Arts prompts) or the middle (banners), one anchor per layer so a window drawn in
   pieces stays together. Narrower than 16:10 (4:3) gets 16:10 with bars. Tested 1280x720, 1280x800, 1024x768
   (bars), 1680x720 Center, 1792x504 / 1680x840 Edges: hunt HUD, pause menu, help box, equipment windows,
   nameplates. 16:10 fixes by Oddity-git (PR 21, 2026-10-26; tested on a Steam Deck size, Linux): target cam icons
   top, the loading screen as one picture (layer 110 strips at y 168 / 433 / 570), layer 98 with its 97 menu, the
   Request Log / quest card (layer 100 beside a 97 menu) top, the Quest Complete border and results rule bottom,
   the cutscene's top bar top, and the item bar drawn without the game's scissor (that draw only; the next draw and
   clears get it back). 2026-10-27 (issue 58): a window drawn in pieces on one layer (the chat window, the shoutout list,
   the Guild Card greeting, the Hunting Style viewer) takes one anchor when its pieces' rules disagree (exec.c
   ui_group_end: by the nearer edge, or the centre; patched in transient memory before the pass ends; the game's scissor
   of a piece moves with it, as its group was anchored last frame, so the help windows' body is not cut), the village / Hub
   button guides on layer 96 go top, layers below 95 by thirds (the Guild Card name plate), the quest results screen (marked by its Time Left bar, layer 99 at y 643) centres its
   middle: the stamp, reward table, item lists' parchment and rows, the title's PRESS A scales
   about the centre with its layout (layer 108), and a model preview follows the frame drawn over it, not the last quad
   (the Palico Scout's "Rotate" prompt). MHGU_CONTROL `hudtrace` logs a frame's HUD quads and their anchors. Left: the HUD's layout elsewhere (title, town, quest board, item box) unverified; at Edges
   the Hunter Arts prompt goes right with its layer; whether an off-screen 3D view (equipment box preview, guild
   card) uses one of the three cameras (they are left alone unless 16:9).
10. Keyboard remapping: an ini [Keyboard] section, one line per Switch button (`A = Mouse2`, `B = Space`; SDL key
   names, several keys per button, mouse buttons too), plus the left stick's four directions and the walk key. Today
   the layout is fixed: key_buttons in runtime/host_input.c, WASD / Shift handled beside it. The current layout stays
   the default, so existing ini files keep working; the settings launcher (item 7) can show it later.
11. Button-combo windows at high frame rates: fixed (2026-10-28), see "Two-button inputs and the frame rate" above.
   Hunter Arts on R+A+B (another control type, 2026-10-29): tested by the user at a high frame rate, works (R and A
   held then B, or all three at once). The countdown starts on A, B, X, Y and R / ZR (pad bits A 0x100, B 0x80,
   X 0x20, Y 0x10, R 0x40, ZL 0x200, ZR 0x800 at player +0x13f4 held / +0x13f8 new) and keeps each press of the
   window (+0x1444 bits 13+, 0x47d574), so the buttons need not be held together. Left: a check elsewhere that
   wants them held at the same moment would still miss a quick finger roll at very high frame rates (30 Hz polling
   hid that); a 30 Hz input polling option would cover it at up to 33 ms of input latency - not needed so far.
   Still to check: other weapons' timed inputs (Charge Blade, Insect Glaive, Styles') for other per-update windows.
## Build / run

MSYS2 UCRT64 on PATH (`C:\msys64\ucrt64\bin;C:\msys64\usr\bin`):

    make gen GAME=../mhgu/game     # recompile (needs tools/venv: capstone, unicorn, lz4)
    make -j4                       # ~8 min full, incremental (changed chunks only) is fast
    build/mhgurecomp.exe           # or double-click it; run-game.bat / run-debug.bat start it too

`make` also makes build/ the game folder: the MSYS2 DLLs the executable needs beside it (tools/bundle_dlls.py
follows the import tables; ~100 DLLs, most of them FFmpeg's; never vulkan-1.dll: the driver's loader must be used)
and the game files in build/data/exefs + build/data/romfs (target `data`, once: hard links to GAME's files, so no
second copy on disk; plain copies where hard links don't work). Started without a game folder argument, the
executable works in its own folder (runtime/main.c, platform plat_standalone): data/ for the game, cache/, mods/,
shots/, mhgurecomp.ini and log.txt beside it (save data elsewhere, see below); with a console of its own (Explorer, a shortcut) it drops the
console and logs to log.txt, and a fatal error shows a message box. `make clean` keeps all that (it removes the
compiled files only). With a game folder argument (or MHGU_GAME_DIR) it uses the current folder as before; then the
DLLs must be on PATH (C:\msys64\ucrt64\bin) unless it is the bundled build/mhgurecomp.exe.

Save data: the standalone executable keeps saves in the player's own folder, %APPDATA%\MHGURecomp\save\ (save_0/
system + system_backup, sdcard/), so deleting or rebuilding build/ can't take them along (platform plat_user_dir;
the path is in the log: "[fs] mounted host directory"). With a game folder argument (the test scripts in run/) saves
stay in save/ under the current folder, so automated runs never touch the real save. MHGU_SAVE_DIR overrides both.
Linux (not written yet): plat_user_dir should return $XDG_DATA_HOME/MHGURecomp, else ~/.local/share/MHGURecomp.
First start without a save (checked 2026-10-21 with an empty save_0): the game shows "There is no data to load." and
then the language / bonus notices; these boxes have no button prompt and only A (right click; keyboard L before 2026-10-23) closes them, B and Plus
do nothing, so it looks frozen. After that: title, New Game; a fresh system + system_backup is written by then.

Mods (runtime/mods.c, mods.h; tested 2026-10-21 with GU Rebalanced): drop a mod's folder into mods\ in the game folder (the
standalone build creates the empty folder), remove the folder to uninstall; nothing on disk is changed, all is layered
at start. romfs/ is found up to three levels down (Mod/Mod/romfs, atmosphere/contents/<title id>/romfs) or as a bare
nativeNX/ folder; names match in any case; new files are added (GU Rebalanced adds weapon/w11/163.arc). exefs/:
IPSwitch .pchtxt and IPS / IPS32 (<build id>.ips) patches, matched to main / sdk by build id; offsets are the module's
memory offsets (pchtxt with offset_shift 0x100, as all of theboy181's: 0xBBCD34 = runtime 0xdbcd34). Only data patches
(rodata / data) are applied; code patches can't be (the code is compiled ahead and literal pools are folded in), the
log lists them: "[mods] <mod>: <file> for main: ... code patch(es) NOT applied (at ...)". The three common MHGU exefs
mods are covered by the port itself: 4K-2K = ini resolution (gen.py PATCHES at the same two instructions), 60fps = ini
frame rate, Connecting-Error-Fix = nifm HLE'd as no internet. A code mod someone needs can become a gen.py PATCHES
entry (or a build option that reads .pchtxt at `make gen`). Order: folder names, or mods\load_order.txt; '_' prefix
skips a folder. Log lines: "[mods] <mod>: romfs <path>", "[romfs] mod <path>: <n> files".

Forge plugins (runtime/forge.c, interp.c; 2026-10-21): Forge (github.com/Fexty12573/forge) is a loader for code mods:
its own module (exefs subsdk9) loads .nro plugins from the RomFS folder nativeNX/plugins/ and gives them hooks,
patches, singletons, save data, log and input. The port doesn't use Forge's module (it patches the console's code);
it finds the plugins itself (game RomFS + every mod's romfs, any case), loads them at 0x03200000 (relocated against
main / sdk, imports of forge_* bound to host code) and runs their code in an AArch32 interpreter (runtime/interp.c:
the lifter's semantics instruction for instruction, same Cpu state; tools/test_lift.py --interp checks it against
Unicorn like the lifter: 0 mismatches over 20k sampled game / SDK instructions incl. NEON + VFP). Calls between the
plugin and the recompiled game go through rt_dispatch both ways. Hooks: every generated function's FN_ENTER (cpu.h)
tests a bit in g_fnhook (one bit per code word, dispatch.c fnhook_set) and runs the detour in the function's place;
the "original" is a trampoline in guest memory with Forge's layout (two original words + `ldr pc` to target+8,
plugins verify it) that dispatch.c runs as the unhooked function. forge_hook_create also writes Forge's `ldr pc,
[pc,#-4]; detour` over the entry (plugins verify that too; a hook inside a function: see Damage Numbers below);
patches over code are re-read for that form (or a `b`),
anything else in code is logged as not applicable. Forge's own points: sApp::run (main+0xB8692C: resolve singletons,
forge_onLoad / version check / forge_onInit; every plugin's DT_INIT and DT_INIT_ARRAY run first, as nn::ro runs them
at load: an entry that is 0, -1, a Thumb address or outside the module is skipped and logged, DT_INIT 0 is no
initializer, DT_PREINIT_ARRAY is not run (nn::ro does not for a shared object); while DT_INIT runs the init array holds
no-ops and its constructors run once after it, so a module whose DT_INIT walks __init_array_start__ (Forge's
source/crti.c does) does not run them twice. A plugin with no forge_onLoad still gets update and ImGui calls, as in
Forge), the frame update (main+0x3DA8FC: forge_onUpdate(dt), then the input is read and the overlay drawn, in Forge's
order), cSystem ctor / dtor (singletons: typed at sApp::run, like Forge's; unlike Forge's, ones made later are typed
too, a frame after they are made and once their vtable looks finished, on a lookup or forge_singleton_getAllInstances;
one that never looks finished is given up after 120 frames). forge_socket_*: forwarded to the port's nn::socket (the
virtual local-play network of net/vsock.c; no internet), failures -1; forge_socket_init / deinit are nn::socket's
Initialize / Finalize with the plugin's arguments (no-ops there). Plugins built -mtp=soft -ftls-model=initial-exec get
__aeabi_read_tp (one block per thread: 8-byte TCB, then 4 KiB for each plugin that uses __thread data; a thread's block
goes back to a free list when the thread ends) and R_ARM_TLS_TPOFF32 is resolved against it. An NRO has no PT_TLS and
its link-time symbols for .tdata are not in the dynamic table, so the template cannot be found: __thread variables
start zeroed (a plugin with an initialized one would see zeros; none of the installed plugins uses TLS). A plugin with a
relocation type this port doesn't know, or a damaged NRO, is skipped with a "[loader]" line (the imports nothing
provides are named there too; MOD0, the dynamic section, DT_STRTAB / DT_STRSZ, symbol names and DT_HASH are bounds
checked). Hook creation fails cleanly (no original, a log line) when the trampoline region is full, the tables are
(1024 function hooks, 1024 trampolines, 1024 kept entry words, 512 interpreted functions), the next function starts
under two words after an entry hook, or an inside-hook's function cannot run in the interpreter (the two words are
put back); hooks may target a plugin's own code; Thumb targets are refused (the interpreter is ARM only; Forge hooks
Thumb code too).
forge_input_* is read once a frame after the plugins' forge_onUpdate, as Forge does when it draws: isConnected is true
from the first frame (the game's player 1 pad is always there), the sticks are clamped to -1..1, the buttons include the
stick-direction bits (16-23) of the game's pad. forge_input_getTouch is the free mouse's left button (not while the
mouse is captured or Forge's menu is open), as the first touch point in whole 1280x720 screen pixels: the pointer must be
on the game's picture (fitted in the window by g_render_aspect, centred; outside it: no touch), and the picture's HUD
canvas (g_ui_anchor_aspect wide at most, centred) spans 0..1279 (beyond it: the edge). Forge's keyboard is 256 HID
usages.
Forge's config.ini ([menu] key, font_size; [log] level) is read as iniparser reads it, from one file: the topmost mod's
romfs nativeNX/forge/config.ini (files are not merged; a long line, a stray line or an unreadable file means the defaults:
level info, key Insert, 12 px), key = strtol base 0 (decimal, 0x hex, 0 octal), font_size = atof, a level that is not a
name is error (as in Forge); the default key is Insert (73), not Forge's F9 (66), a choice of the port's author.
Forge's own messages are written at Forge's levels ("[forge] [INFO] ..."); third_party/cimgui's
cimgui.h / cimgui.cpp are byte-identical to cimgui commit d298666 (Forge's pin; checked against GitHub, ImGui itself
being a submodule there is 1.92.8 by its header only).
Extras beyond Fexty's exported.txt, kept as harmless: forge_log_trace(format, ...) (printed at any level, as [DEBUG]), forge_graphics_isInitialized, forge_graphics_isMenuOpen. MHGU_FORGE_LOG=debug|info|warn|error (default info; overrides the file when it names a level). Install = the mod's folder in mods\ (Forge itself is not needed). Log: "[forge] plugin ...",
the plugin's own messages as "[forge] [INFO] ..." (Forge's tags). Plugins are built -mfloat-abi=softfp, as
Forge is: floats in core registers (forge_onUpdate's dt in r0; until 2026-10-23 it went in s0 only).

Forge's ImGui overlay (runtime/overlay.cpp, forge_imgui.c, tools/imgui_thunks.py; 2026-10-23): Forge's "forge"
window (About, Loaded Plugins, Plugin UI = each plugin's forge_onImGuiRender) and forge_onImGuiFreeRender, on Dear
ImGui 1.92.8 + cimgui (third_party/cimgui, Forge's versions; ImGui's Vulkan backend). The menu starts closed; Insert
opens and closes it (or Forge's config.ini [menu] key / font_size from a mod's romfs nativeNX/forge/, with its Roboto
font from there too; else ImGui's own font). Sizes follow the window: x height/720. The frame is built on the game's
thread after forge_onUpdate (plugin code runs there); texture uploads there too, under vk_lock; the render thread
draws a copy of the draw lists at the end of the window image's pass (device.c). Input: while open, keyboard / mouse
/ text go to ImGui (host_input.c asks overlay_event first; F-keys and Alt+Enter still work), the controller navigates
(A activates, B cancels) and the game gets no input. Plugins call cimgui's C API (ig* / Im*, which Forge exports):
tools/imgui_thunks.py generates runtime/forge_imgui_thunks.inc from cimgui's generator output, 1445 of 1465 functions
(OVERRIDE ones are hand-written in forge_imgui.c; the rest, listed at its end: draw callbacks, size_t* helpers...): softfp arguments
converted, guest data pointers as their host address, printf-style ones formatted from the guest's arguments.
ImGui's objects (ImGuiIO, ImGuiStyle, ImDrawList...) reach plugins as mirrors in guest memory with the 32-bit
layout (computed per struct, checked against the compiler's with static asserts), synced both ways around each
call; internal structs are opaque. Functions taking or returning an ImTextureRef by value (ImDrawList_AddImage /
AddImageQuad / AddImageRounded, ImTextureData_GetTexRef, igImage...) are provided: its 16 bytes (pointer, pad, ImU64: 8-aligned,
so r2:r3 + the stack after a first argument, or the hidden result pointer in r0) are rebuilt field by field, _TexData as the
host ImTextureData behind the plugin's mirror. igNewFrame / igRender / igGetDrawData from a plugin are ignored; igMemAlloc is
the guest's malloc. Tested with Key Quest Markers r18 (its only ImGui is its colour settings): the menu over the
title and in a hunt, mouse (headers, tree node, buttons), typing into its hex colour fields, Save & apply (wrote
key_quest_markers.ini to the save folder), controller navigation; the game unaffected with the menu closed. Callback
trampolines (InputText callbacks, size constraints, list getters), mirrors of structs a plugin owns, and several
ImGui plugins at once are written but not exercised by that plugin. MHGU_CONTROL for tests: `sdlkey [ctrl+]<key>`,
`sdltext <text>`, `mouse <x> <y> [click]`; `mousemove <x> <y> <ms>` moves the camera as mouse look does.

Damage Numbers r30 (2026-10-26; it crashed the port at start, fine on emulators). Four gaps:
- A hook inside a function (main+0x16dd7c, the end of f_36dc6c, which works out a hit's weak-spot flag): Forge writes
  its jump wherever it is asked, and the port hooked function entries only. forge.c hook_inside: the jump goes into
  guest memory as on the console, the trampoline (two moved words + `ldr pc` to target+8) into the interpreted stub
  region, and the function holding it runs in the interpreter from then on (dispatch_interpret, as for cheats), which
  reads the jump and runs the plugin's detour and the trampoline as code. Tested with MHGU_FORGE_TESTHOOK=8bd100 (inside
  the town's mode-1 update, f_8bd0fc): its guest-code detour ran and the same function went on to depart the quest.
- igAddContextHook with the plugin's own ImGuiContextHook (no mirror: igAddContextHook(ctx, NULL), a crash at 0x10):
  hand-written, the struct copied to guest memory and a host hook whose callback runs the plugin's on the frame's
  thread.
- File names: the plugin passes console paths ("app:/nativeNX/plugins/...", forge_fs_getRomFsMountPoint) to
  AddFontFromFileTTF / ImFileOpen; every `*filename` argument now maps app:/ and rom:/ to the RomFS's host file
  (forge_imgui.c ig_path). ImFileOpen / Close / GetSize / Read / Write hand-written: the plugin gets a small handle
  number for the host FILE *.
- ImTextureData_GetPixels returned ImGui's host memory (NULL for the plugin): it now gets a copy in guest memory (its
  malloc), copied over ImGui's on RegisterUserTexture and whenever the texture waits for an upload (tex_sync).
- It also hooks f_36dc6c's entry, and that detour calls the original through the trampoline (rt_dispatch: a run of
  its own) into the now interpreted function, which returns into the detour, interpreted code too: the inner run went
  on into the detour and then hit "no function at" the outer caller's return address (crash when aiming the bow at a
  monster, 2026-10-26). interp_run now also ends at its return address in interpreted code when the stack is back
  where it started. MHGU_FORGE_TESTHOOK=8bd0fc,8bd104 (entry + inside, the town update) reproduces it without the fix.
In a quest with the mod: no crash, "8 Forge hooks", its font and effect atlas load. Left: "visible crop unavailable;
using Forge display bounds" (it falls back to the window); the numbers themselves not yet seen on a hit (quickhunt can't
start Arena quests: "no function at 0" when departing, with or without mods).

Cheats (runtime/cheats.c, 2026-10-24): Atmosphere-format cheat codes (as Ryujinx and a modded Switch read them) from
every .txt in cheats\ beside mods\ (MHGU_CHEATS_DIR; '_' prefix skips a file), all on; "[Name]" starts a cheat, "{Name}"
a master code (run first), '#' / "//" lines are comments (Atmosphere itself rejects those). The VM is written from
Atmosphere's docs/features/cheats.md (every code type; Atmosphere is GPL, its dmnt code is not used): 16 registers and
16 saved, both 0 at the start of every cheat (2026-10-25: before, they carried over from the cheat before it in a frame,
so a cheat leaving r0 set shifted the next one's 0TMR writes: a player's log had the positive-only skill cheat 0x14B0
off, its jump into read-only data: FATAL no function), 128 static (shared); blocks / elses / loops matched when a cheat
loads (a cheat that does not decode is logged and left out); key codes test the buttons the game sees (input_get; stick
directions at half tilt); pause / resume (FF0 / FF1) are ignored, debug log (FFF) goes to the log. Runs once in boot()
before the main thread starts, then on the window loop once per presented frame (as dmnt runs beside the game). Regions:
main 0x200000, heap HEAP_BASE, alias ALIAS_BASE, aslr 0x200000 (GetInfo's), 4 = absolute; kernel_mapped() checks every
access (unmapped: reads 0, writes dropped). A write into main / sdk code that changes it: dispatch_interpret() makes the
function holding the word (dispatch_function_at: nearest entry below, up to the next) run in the interpreter from then
on: its range becomes interpreted and its entry gets a hook to an `ldr pc, [pc, #-4]; .word entry` stub in interpreted
memory (an existing hook stays; fnhook_call_original then interprets). Words in the padding after .text
(main+0x13F1DEC..0x13F2000, zeros) are made interpreted ranges themselves: room for a cheat's new code. Limits: a
function the lifter split (a second entry inside it) has its other part still recompiled; a call already running
finishes the old code; an interpreted function is many times slower (fine for f_2df824, called on equipment changes).
interp.c's ranges went 16 -> 256 and merge when they touch. A function a Forge plugin hooks and a cheat changes
(2026-10-25): Forge's hook writes `ldr pc, [pc, #-4]; detour` over the entry in guest memory, so interpreting it jumped
back into the detour, whose original interpreted it again: a stack overflow. hook_install (and a plugin's code patch
that makes a hook) now keeps the two words it replaced (fnhook_keep_entry) and the interpreter runs those at a hooked
entry (code_word: only where g_fnhook has a bit). Tested both orders with MHGU_FORGE_TESTHOOK=2df824 (a plugin-style
hook at start-up, before the cheats) and the control file's `testhook 2df824` (after them, as plugins hook): the detour
runs, the skill table is the cheat's; without the fix, exception c00000fd. Tested: the skill-point cheats of
docs/research/skill-points.md on the test save in a quest (exact), a VM test cheat (loops, registers, master first, C4
once per press, a bad cheat refused), the Linux build. Log: "[cheats] <file>: <n> cheat(s): ...", "[cheats] <cheat>:
changes the code at main+<x>: the function at main+<y> runs in the interpreter now".

Skill points (docs/research/skill-points.md; extras/cheats): f_2df824 totals them, rows at [obj+0x188] (12 bytes:
+4 activation, +6 tree, +8..+14 points per slot weapon/head/body/arms/waist/legs/talisman, +15 total, s8). "Skill
points x2": 0x2dfab4 `uxtbne r0, r0` -> `lsl r0, r0, #1`, plus Double Talisman's two passes adding `r0, lsl #1`
(talisman x4 with it). "positive only": 0x2dfab4 -> `b` to main+0x13F1F00 (`cmp r0, #0; addgt r0, r0, r0; b back`)
and both Double Talisman passes' `ldrsb` -> the same through two more stubs there. Test save hunter (Double Talisman
and Skill +2 active): base tree 35 (Handicraft) = 1,2,2 + talisman 8 (+2) = 15; positive only 2,4,4,16 (+2) =
28, its negatives unchanged (-5 +2 = -3); full x2 the same positives, negatives -10 +2 = -8.

Bonus content (runtime/services/bonus.c, 2026-10-21): the free downloads (event / challenge quests, item packs,
Palicos) live in the shared part of the save file, not in the game files, and Capcom's server (meteor.capcom.co.jp)
is out of reach. So they are copied from a save that has them: put that save's `system` file in bonus\ in the game folder (next
to the exe, like mods\; the current folder with a game folder argument), else %APPDATA%\MHGURecomp\bonus\ is read;
MHGU_BONUS_DIR overrides. Only with ini [Bonus Content] BonusContent = On (default On again since 2026-10-23, was Off
from 2026-10-22; with Off saves are never touched; tested both ways on an empty save). When the game opens a save whose download regions are empty (0xb2a5-0xb310 unlock flags, 0xc000 to the first character slot: item packs,
Palicos, 160 + 40 quest slots of [id][size][ARC]), the donor's are written in; a save with downloads of its own is
left alone. A brand-new save gets them from the next start (the game writes it without reading it back). Tested with
the user's save (125 event + 40 challenge quests): Low and G Rank Event lists filled (URGENT: COAL MINERS NEEDED in
G Rank), Chamberlyne > Downloadable Content > Claim Items lists the packs and claiming works (Pirate's Booty Pack:
Slickaxe x20). Palico downloads not checked. The quest ARCs equal the RomFS files byte for byte, so a donor-free
version (building the regions from the RomFS) is possible once the flag bits and the pack / Palico records are known.

Controls: see `runtime/host_input.c` (keyboard and mouse: WASD, mouse look (relative mode after a click, Esc releases; the mouse speed smoothed
over 25 ms as the right stick tilt, offset past the camera's round ~39% stick dead zone; ini [Mouse] MouseSensitivity), clicks = X / A, Space / R = B / Y, Tab / T = ZL / ZR,
arrows = d-pad, Ctrl / Alt = stick clicks, Enter = Plus; SDL gamepads (all open, the one a button was last pressed on is
read: a Steam Deck with a controller plugged in has two, and taking the first one SDL listed left the player's pad dead;
MHGU_PAD=<name part or vid:pid> keeps only that one), face
buttons by position (Switch layout) or label: ini FaceButtons; the controller also works with the window in the
background, ini BackgroundInput = On (default; SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, set before SDL's gamepad
subsystem starts, runtime/ui.c; MHGU_BACKGROUND_INPUT=0|1), keyboard / mouse only with the focus;
F12 screenshot, F11 8-frame burst -> shots\ in the game folder; Alt+Enter windowed / full screen; F7 cycles MSAA). Display mode,
resolution, anti-aliasing and frame rate: mhgurecomp.ini (delete it to get the defaults back).
Game folder: where the settings, caches, mods\, bonus\, shots\ and log.txt are: the exe's folder when started on its
own (build\ itself holds only what the build makes; compiler files go to obj\). MHGU_HOME=<folder> moves them there
(run-game.bat: run\, test scripts: testsave\home); the game files are still read from data\ beside the exe.

When the game quits by itself (2026-10-28, a Fedora report: "ExitProcess" right after GetService acc:u0, no
reason): the SDK's failed checks (every AbortImpl variant ends in nn::diag::detail::VAbortImpl, 0x244a0d0), abort()
and exit() are HLE_SDK in runtime/hle/abort.c and log the condition, function, file and line, the Result (as
2xxx-yyyy) and the message, then the game's lr and callers (kernel_log_guest_stack, the same heuristic as
MHGU_DUMP_THREADS); svcExitProcess logs its caller too. Look the callers up with tools/disasm.py.

Debug env: `MHGU_TRACE_SVC`, `MHGU_TRACE_IPC`, `MHGU_LOG_FILES`, `MHGU_FNPROF` (hot functions every 5 s),
`MHGU_FNTRACE=lo-hi[:n]` (calls into a guest address range, with caller and thread), `MHGU_DUMP_THREADS=<s>`
(every guest thread's wait state after s seconds), `MHGU_HLE_SKIP`, `MHGU_AUDIO_LOG`, `MHGU_MOVIE_TRACE` (calls
through the movie vtables), `MHGU_HIDDEN=1` (no visible window), `MHGU_REGION=<n>` (GetRegionCode; default 2 Europe), `MHGU_NO_WRITE_WATCH=1` (Windows: no page write tracking, textures hashed as on Linux), `MHGU_INPUT="frame:BUTTON:frames,..."` (scripted
controller, frames = presented frames; e.g. `300:A:10`), `MHGU_NAME` (answers every text entry with that text at once),
`MHGU_CONTROL=ctl.txt` (drive a running game: append lines `A`, `DOWN 8`, `STICK_UP 60`, `shot name` -> shots/name.png;
`X 6 25` = X for 6 frames from 25 frames on, lines written together form a timed sequence; `fntrace lo hi n`;
`fnnew lo hi [learn ms]` = log only the first call of each function in [lo, hi) not seen while learning, to find what an
event runs; `peek` / `poke` / `memsave` / `find` for guest memory), `MHGU_QUICKHUNT=<quest number>` (from start-up
straight into that quest: A through the menus to the save's town, then the quest is ordered and departed on;
runtime/hle/quickhunt.c has how the game departs; `MHGU_QUICKHUNT=town` stops in the town), `vpad` (control file:
attach an SDL virtual controller; `vpad <n> <button> <0|1>` presses its button), `MHGU_LOG_MOVES=1` (the player's actions and command table
changes, runtime/hle/movelog.c), `MHGU_EMTRACE=1/2/3` (large monster actions with their real-time length, height and
distance; 2: the position every update; 3: the monster object saved on 4 updates once a forced action is 200 up;
MHGU_CONTROL's `emact <group> <index>` starts an action, `emsweep <group> <first> <last>` each in turn:
runtime/hle/emtrace.c), MHGU_CONTROL's `fntrace lo hi n r0lo r0hi` (only calls with r0 in that range: one object's
methods) and `interpwatch <addr> <file of function entries>` (runs those functions and all game code they call in the
interpreter, logging every instruction that changes the word at addr),
`MHGU_SHOT_AT=<frames>`, `MHGU_FPS=1`, `MHGU_VK_STATS=1`, `MHGU_VK_VALIDATION=1`, `MHGU_SYNC_SHADERS=1`, `MHGU_PIPE_WAIT_MS=n`, `MHGU_SKIP_LOG=1`, `MHGU_NO_PREWARM=1`,
`MHGU_NO_PROGRAM_CACHE=1`, `MHGU_SAVE_GLSL=1`.
`python tools/disasm.py <game dir> <addr | module:symbol> [count] [before]` disassembles guest code.

Plugin bug warnings (runtime/forge.c forge_guest_fault, interp.c gchk / bad_access, platform.h plat_touch_hook): the
port backs any untouched guest page with zeros, so a plugin's null / garbage pointer that crashes on the console
(Ryujinx) runs on here. Quiet `[forge] [WARN]` lines name it (MHGU_FORGE_LOG=error silences them):
`[forge] [WARN] foo.nro: 4-byte read at 0x00000008 (null page) by code at +0x1A4C (pc 03212A4C) r0=0 r1=.. lr=..`.
Detected, for code in a plugin's image (interpreted): any load / store below 0x10000 (the null page); one at
0x3C000000-0x43FFFFFF or 0xBC000000-0xC3FFFFFF (a float bit pattern: 0x3F800000 = 1.0f) that no region maps (the game's
heap counts as mapped); the first touch of any unmapped page, i.e. the fault that makes the port back it (size not
known, "unmapped page"). Not detected: later accesses in an unmapped 64 KB chunk after its first touch, other
unmapped addresses that the interpreter checks cannot see (outside the two float ranges) once their chunk is
backed, atomics (ldrex/strex), bad accesses by the game's own code or by host (HLE) functions, branches to bad
addresses, and a mapped but wrong pointer. Cost: one compare per interpreted load / store plus three thread-local
stores per interpreted instruction; recompiled code and mapped accesses are untouched (the fault hook runs only
when a page is first backed). Limits: once per (plugin, pc); at most 20 lines per plugin, then one "further memory
warnings ... suppressed" line; 128 sites tracked per plugin. Also WARN, once per site: a code patch that lands in
recompiled code is ignored (`foo.nro: code patch at main+0x... cannot apply to recompiled code: ignored`), and at
load `[forge] [WARN] foo.nro: import X is not provided (calls return 0)`. Failed hooks stay ERROR lines
(`Failed to hook ...`), as Forge's own.
