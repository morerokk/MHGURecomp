# FSR 4 upscaler (2026-10-26, moved to AMD's FSR SDK 2.4.0 on 2026-11-01)

AMD's machine-learning upscaler (FSR 4, provider "FSR 4.1.2") as an alternative to FSR 3.1 in the Vulkan renderer
(`docs/FSR-PORT.md` describes FSR 3.1 and everything around it, which this builds on). It runs **natively on Vulkan**,
on the game's own VkDevice and command buffer: no Direct3D 12 bridge, no shared images, no split submits. It takes
exactly the inputs fsr.c prepares for FSR 3.1 (colour, depth, camera + object motion vectors, reactive mask, jitter,
camera near / far / FOV) and is dispatched where the FSR 3.1 dispatch is. Default stays FSR 3.1, FSR stays Off by default.

Tested on: RX 9070 XT, Adrenalin 26.10.1 (Vulkan driver 1.4.359), Windows 11. Measurements below were taken on the driver route
of the first version; the SDK 2.4.0 route is re-tested in the notes at the end.

## Settings

    [Upscaler]
    FSR = On
    Upscaler = FSR4        ; FSR3 (default) or FSR4
    FSRQuality = Quality   ; same ratios as FSR 3.1: NativeAA 1x, Quality 1.5x, Balanced 1.7x, Performance 2x, UltraPerformance 3x
    Sharpness = 0.8        ; the provider's RCAS sharpening (same key as FSR 3.1)

An existing mhgurecomp.ini has no `Upscaler` key (only whole missing sections are added): add the line by hand.
Test overrides: `MHGU_FSR_UPSCALER=3|4`, `MHGU_FSR4_LOADER=<path>` (another loader DLL; a missing path shows the fallback),
`MHGU_FSR4_LINEAR=1` (colour not flagged as gamma-encoded, A/B), `MHGU_FSR4_FAILTEST=n` (the n-th FSR 4 dispatch is
reported as failed, to see the fallback), `MHGU_FSR_DEBUG=1` (FFX input checks), `MHGU_FSR_LOG=1` (per second:
`upscaler FSR 4.1.2, FSR 4 frames N`).

To try it: set `FSR = On`, `Upscaler = FSR4` (any FSRQuality) and start the game; log.txt has the `[fsr4]` lines.

## What is included

> **2026-11-01: moved to AMD's FSR SDK 2.4.0.** The first version of this port (2026-10-26) predates the SDK's Vulkan DLLs and
> reached the FSR 4 provider inside the AMD driver (`amdvkffx64.dll`) by patching one import of that DLL in memory. SDK 2.4.0
> ships the Vulkan loader and upscaler DLLs, so that route and the patch (`FSR4_DRIVER_PATCH`) are gone: FSR 4 now goes through
> AMD's own loader, the way AMD intends. Sections below that still describe the driver route are history (marked as such).

FSR 4 on Vulkan is **opt-in**: it needs `FSR = On` and `Upscaler = FSR4` (default `FSR3`; FSR itself is Off by default), and if it
cannot start the game uses FSR 3.1 instead (every reason is logged with a `[fsr4]` line).

Route (the only one): `amd_fidelityfx_loader_vk.dll` beside `mhgurecomp.exe` (or `MHGU_FSR4_LOADER=<path>`) is loaded at run time. It
finds `amd_fidelityfx_upscaler_vk.dll` beside the exe (or beside itself). The game asks the loader for the upscaler versions (`ffxQuery`),
takes the newest 4.x one and creates and dispatches it through the public ffx-api (`ffxCreateContext` / `ffxDispatch`). Log:
`loader ..., file version`, `loader offers upscaler 4.1.2 ...`, `route: AMD's FSR SDK loader`.

## The AMD DLLs

They are **AMD's signed binaries from FSR SDK 2.4.0, unmodified**: `amd_fidelityfx_loader_vk.dll` (39 KB) and
`amd_fidelityfx_upscaler_vk.dll` (36 MB). They are not in this repository (`*.dll` and `third_party/fsr-sdk/` are git-ignored). On
Windows `make` downloads them once from AMD's tagged release (`github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK`, tag `v2.4.0`),
checks their SHA-256, copies them into `build/` (the game folder) and writes AMD's licence into `build/licenses/AMD-FSR-SDK.txt`, so a
release carries them with the notice AMD's licence asks for. `make FSR_SDK=none` builds without them (FSR 4 then needs the player to
drop the two DLLs beside the exe, else FSR 3.1).

AMD's licence for these binaries (SDK `docs/license.md`): redistribution in binary form only, free of charge, with the licence text
reproduced; no reverse engineering, decompilation or disassembly. That is why they are bundled, never modified or linked into the
executable, and why they stay under AMD's licence, separate from this repository's (`THIRD_PARTY_NOTICES` says so). The `ffx-api`
headers in `third_party/ffx-api` are the SDK's MIT-licensed ones. FSR 4 is verified on an RDNA 4 GPU; the SDK 2.4 also
supports RDNA 2 / 3 (INT8 models), which the game now attempts but is untested (see "Not done").

## How it works (runtime/nvn/vulkan/fsr4.c, fsr4.h)

- Loader: `LoadLibraryEx` of `amd_fidelityfx_loader_vk.dll` (beside the exe), then `GetProcAddress` for `ffxQuery`, `ffxCreateContext`,
  `ffxDestroyContext`, `ffxDispatch`. `ffxQuery` with `FFX_API_QUERY_DESC_TYPE_GET_VERSIONS` and a `VkDeviceContext` (device, physical
  device, vkGetDeviceProcAddr) lists the upscaler versions this GPU gets; the newest "4.x" is used via `ffxOverrideVersion`.
- Context: `ffxCreateContext` with `ffxCreateContextDescUpscale` (flag `FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE`) -> the Vulkan backend
  descriptor (`ffxCreateBackendVKDesc`, id `0x01000003`) -> `ffxCreateContextDescUpscaleVersion` (`FFX_UPSCALER_VERSION`, 4.1.2) ->
  the version override. Created once, at the first FSR frame, because the ratio does not change during a run.
- Dispatch: `ffxDispatchDescUpscale` with VkImage resources and the command buffer; all images are in GENERAL (the renderer's rule) =
  state "unordered access" for every resource. Colour is the game's gamma-encoded LDR RGBA8, so `FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB`
  is set (depth: standard Z, no flags; motion vectors in render pixels, jitter removed; motionVectorScale 1; preExposure 1).
- Jitter: the same picture shift FSR 3.1 gets (the camera hook applies AMD's Halton jitter, 8 (output/render)^2 phases).
- fsr.c: `run_fsr` is shared; after the prep / object-velocity / finish passes it records either the FSR 3.1 dispatch
  or `vk_fsr4_dispatch`. On any FSR 4 failure it logs why, switches FSR 4 off for the run and records the FSR 3.1
  dispatch for that same frame with a history reset (FSR 3.1's context is created only then).
- Device (device.c, only with FSR on and `Upscaler = FSR4`): extensions `VK_KHR_cooperative_matrix`,
  `VK_EXT_shader_float8`, `VK_KHR_synchronization2`, `VK_EXT_subgroup_size_control` and the features the provider's
  shaders declare (cooperative matrix, float8 + float8 cooperative matrix, synchronization2, subgroup size control /
  full subgroups, shaderFloat16 / Int8 / Int16, buffer device address, Vulkan memory model, 8 / 16-bit storage,
  variable pointers, descriptor indexing, storage image read / write without format); instance apiVersion 1.3 instead
  of 1.2 (its SPIR-V is 1.6). If the GPU lacks any of it: logged, FSR 3.1 runs. With FSR off or `FSR3` nothing changes.
  (These were worked out against the driver's provider; whether SDK 2.4's upscaler DLL asks for the same set is checked by the
  first run, see the test notes.)

Log lines (log.txt / stdout): `[fsr4] loader <path>, file version`, `loader offers upscaler 4.1.2 (id 0x...)`,
`route: AMD's FSR SDK loader`, `FSR 4.1.2 context WxH -> WxH created in N ms (...formats...)`, `first FSR 4.1.2 dispatch recorded`,
`[fsr] on (FSR 4.1.2): ...`, failures: `[fsr4] ... FSR 3.1 is used` / `... FSR 4 is off from here on, FSR 3.1 takes over`.

## Files

New: `runtime/nvn/vulkan/fsr4.c`, `fsr4.h`; `third_party/ffx-api/` (ffx_api.h, ffx_api_types.h, ffx_upscale.h from the
FSR SDK v2.4.0, MIT, licence file kept; `vk/ffx_api_vk.h` added); this file.
Changed: `runtime/config.c/.h` (`Upscaler` key, `MHGU_FSR_UPSCALER`), `runtime/nvn/vulkan/device.c` (the FSR 4 extensions,
features, apiVersion), `runtime/nvn/vulkan/fsr.c` (the branch in `run_fsr`, init, a log line), `Makefile`
(`-Ithird_party/ffx-api`, `-lversion`, `FSR_SDK`: downloads and bundles AMD's two DLLs and their licence text), `.gitignore`, `THIRD_PARTY_NOTICES`. Not changed: shaders, textures, exec.c, the camera / jitter code.

Note: the conventions used here (4.x provider chosen by version id, gamma-encoded LDR colour with the non-linear flags,
depth / motion-vector conventions, one jitter convention for all providers) were worked out with a Direct3D 12 route
first; the Vulkan provider made that route unnecessary, so it is not part of this port.

## Measured (RX 9070 XT, 1920x1080 window, quest 101, hunter standing, 4 frames per shot; MHGU_CONTROL gpuprof)

FSR 4 ran every frame: ~60 `FSR 4 frames` a second in every one-second log line while the quest scene was shown
(2170 frames in the Quality run, 2211 in the NativeAA run, the rest being the town / loading frames, scaled as before).

GPU time (prep + FSR stage = the gap before the HUD pass; whole frame), one sample each:

| setting | FSR 3.1 stage / frame | FSR 4 stage / frame |
|---|---|---|
| Quality (1280x720 -> 1080p) | 0.886 / 1.499 ms | 1.301 / 1.915 ms (colour unflagged: 1.102 / 1.714; Sharpness 0: 1.215 / 1.831) |
| NativeAA (1080p) | 1.488 / 2.414 ms | 1.640 / 2.492 ms |

So FSR 4 costs ~0.4 ms more than FSR 3.1 at Quality and ~0.15 ms more at NativeAA here; 60 fps (vsync) in all runs.

Image metrics (`metrics.py`: PSNR dB against the 4K supersampled picture / sharpness relative to it / flicker x1000 over
4 static frames; regions chest+structure | ground | distant terrain | mountain):

    native + FXAA   31.5/0.96/1.9 | 35.6/0.94/0.1 | 36.5/0.98/3.2 | 35.2/0.93/1.6
    FSR 3.1 Quality 31.2/0.94/1.9 | 29.8/0.99/1.2 | 32.6/1.01/1.2 | 33.6/0.87/0.7
    FSR 4   Quality 28.4/0.87/1.0 | 30.8/0.89/2.0 | 35.4/0.94/1.9 | 35.3/0.92/1.3
    FSR 4   Quality, colour not flagged gamma 28.3/0.93/1.3 | 30.7/0.91/3.0 | 32.6/1.01/2.7 | 33.9/0.93/1.9
    FSR 3.1 NativeAA 28.1/1.18/0.7 | 28.8/1.20/0.8 | 33.9/1.37/1.2 | 35.1/1.32/0.8
    FSR 4   NativeAA 28.4/0.95/0.8 | 33.0/0.99/1.5 | 30.5/1.08/2.0 | 33.6/1.00/1.0

Reading: it is not a clear win on this metric in a static, already clean scene: FSR 4 Quality is closer to the
supersampled picture than FSR 3.1 Quality on the distant terrain and the mountain (+2.8 / +1.7 dB) and on the ground
(+1.0 dB), worse on the chest / structure (-2.8 dB) and a little softer (sharpness 0.87-0.94 vs 0.87-1.01) at Sharpness
0.8. FSR 4 NativeAA keeps the picture as sharp as the reference (0.95-1.08) where FSR 3.1 NativeAA over-sharpens (1.18-1.37 at the
same Sharpness); its PSNR is higher on the ground (+4.2) and mountain, lower on the distant terrain (-3.4). With
the gamma flags the colour is closer on the ground / terrain / mountain; flicker is the same order for both. One sample
per setting, one scene: treat the numbers as indications. The crops are in the pull request (they are not kept in the repository); `FSR-PORT.md`, section "Comparison images", describes them.

Seen in the pan frames (camera turning right, hunter walking in place): no ghost trails behind the hunter or the
shield, thin features (chest straps, rope, hunter's outline) stable; no jitter-sign problem (a wrong sign shows as
shimmer on static detail; the standing frames flicker like FSR 3.1's).

## Verified

- Loaded the real driver provider in game (`FSR 4.1.2`, `isSupported` = true, context created in 66-73 ms), 60 FSR 4
  frames a second in the quest scene; menus / title / loading are scaled as with FSR 3.1.
- Fallback: `MHGU_FSR4_DLL=C:/nonexistent/...` -> `FSR 3.1 is used`, FSR 3.1 runs (60 FSR frames a second);
  `MHGU_FSR4_FAILTEST=100` -> `the FSR 4 dispatch failed ... FSR 3.1 takes over` at the 100th dispatch, FSR continues with
  FSR 3.1 the next frame (one history reset, 59-60 FSR frames a second after).
- Vulkan validation (`MHGU_VK_VALIDATION=1`, SDK 1.4.341.1 layer) with FSR 4 in the quest: no message from our own
  commands (barriers, layouts, copies). What remains comes from AMD's own shader modules and the game's old messages:
  the provider's SPIR-V declaring NV cooperative-matrix-2 capabilities (11 `spirv-val` errors, 9 of them the same one) and
  storage-image sampled-type width mismatches in its RCAS / reprojection shaders (7 messages), plus the two old renderer
  messages (a 1D image view, a fragment output without attachment). The driver compiles and runs them (frames are right).

## Verified for the SDK 2.4.0 route

- Builds: `make` downloads the two DLLs, checks their SHA-256, copies them into `build/` with `licenses/AMD-FSR-SDK.txt`; `fsr4.c` compiles.
- Run tests on the RX 9070 XT (2026-11-01, unattended quest 101 on a copy of the save, 1920x1080 window, picture 1918x822 -> 1920x822,
  Quality):
  - `make test-lift` (recompiler against Unicorn, 6000 instructions each for main and sdk): 0 failing instructions
    (7 NaN-payload differences in main, none in sdk).
  - FSR 4 (`MHGU_FSR=1 MHGU_FSR_UPSCALER=4`): the loader (file version 1.0.2.4555) offers upscalers 4.1.2, 3.1.6 and 2.3.4; 4.1.2 is
    picked, context created in 58 ms, first dispatch recorded, 60 FSR 4 frames per second, no fallback, no errors. The screenshot
    (still scene and after a camera turn) is clean: no corruption, no ghosting, HUD unmoved. The GENERAL-layout / "unordered access"
    state the renderer uses for every resource is accepted by the SDK Vulkan backend. Whole frame 2.02 ms on the GPU profile.
  - FSR 3.1 (`MHGU_FSR_UPSCALER=3`): unchanged, "FSR 4 frames 0", 2.26 ms (a different moment of the scene, not a like-for-like timing).
  - Fallback (`MHGU_FSR4_FAILTEST=100`): the 100th dispatch is reported failed, log says "FSR 4 is off from here on, FSR 3.1 takes
    over", FSR 3.1 runs from then on and the picture is fine.
  - After the relaxed device gate and the FSR 3.1.6 reproject fix (same scene): log `GPU features: cooperative matrix 1, float8 1/1,
    integer dot product 1`; FSR 4 still 60 FSR 4 frames a second, 2.04 ms; FSR 3.1 1.96 ms, both running with no errors.
  - Not run: Vulkan validation layer, the other quality presets, a large monster fight, other GPUs (RDNA 2 / 3).

## Not done / known issues

- Only tested with provider 4.1.2, only the Vulkan renderer on Windows, an RX 9070 XT. SDK 2.4.0 adds FSR 4.1 for RDNA 2 / 3 (INT8,
  a lighter model on some APUs). The device gate no longer demands RDNA 4's cooperative matrix + float8: it requires only the
  baseline (synchronization2, subgroup size control, fp16 / int8 / int16, buffer device address, memory model), switches on
  cooperative matrix, float8 and integer dot products where the GPU has them, and leaves the decision to AMD's loader (no 4.x
  version offered, or the context fails: FSR 3.1, logged). **Not tested on any RDNA 2 / 3 card** (none here); whether the INT8
  models need a Vulkan extension this list lacks is unknown (AMD's licence forbids looking inside the DLL, and the SDK docs do
  not list one). The log line `[fsr4] GPU features: ...` shows what the GPU offered.
- Linux: unconfirmed whether FSR 4 on Vulkan runs there. FSR 4 is expected to come to Linux, but whether AMD's DLL route or
  a similar trick works there is not confirmed yet. In this build the FSR 4 code is compiled out on non-Windows and
  FSR 3.1 is used (the FSR 3.1 code was syntax-checked for Linux with gcc, not linked or run).
- Tested by the author in game since: a large monster fight (no ghosting or shimmer seen), the village. Not tested:
  4K, multiplayer.
- The loader and upscaler DLLs are trusted code loaded from beside the exe (the build puts AMD's, checked by SHA-256, there):
  do not put a copy there you do not trust.
- FSR 4 on the AMD driver recreates its network (pipelines) when the ratio changes; the context is created once so
  this port never does. Changing Upscaler / FSRQuality needs a restart (as FSR 3.1's quality already did).
- The colour input is gamma-encoded LDR: FSR 4 was designed for that as well as HDR; `MHGU_FSR4_LINEAR=1` compares.
- The FSR 4 GPU cost in the table includes the shared prep / finish passes (~0.5 ms at 720p), measured with the
  renderer's gpuprof, not with the provider's own timestamps.
- `DX12 bridge`: not built (not needed: the Vulkan provider works).
