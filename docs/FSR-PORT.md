# FSR 3.1 upscaler (2026-10-26)

AMD FidelityFX Super Resolution 3.1 (the FSR 3.1.4 temporal upscaler, Vulkan, from the FidelityFX SDK 1.1.4) in the
Vulkan renderer. With `[Upscaler] FSR = On` the game renders its 3D picture at a lower resolution, FSR rebuilds it at
full resolution from several jittered frames (and anti-aliases it), and the HUD is drawn at full resolution on top.
Default: Off. With FSR off nothing of it runs (every hook point is behind `g_fsr_on` / `g_config.fsr`).

## What works (checked in game, RX 9070 XT, Windows, 1920x1080 window unless noted)

- FSR 3.1 in a quest (Jurassic Frontier, quest 101) and in the village: 60 FSR frames a second, HUD, menus and
  text at full resolution (`fsr1.png`, `menu_fsr.png`, `vfsr_turn.png` in the test shots, see below).
- Quality (1280x720 -> 1920x1080), Performance (960x540 -> 1920x1080), NativeAA (1920x1080 -> 1920x1080), 21:9
  (2560x1080 window: 1706x720 -> 2560x1080, HUD placement as without FSR), RenderResolution 3840x2160 (2560x1440 ->
  3840x2160), Balanced set in an ini file (`FSR = On`, `FSRQuality = Balanced`, `Sharpness = 0.5`, `MSAA = 4x`:
  1128x634 -> 1920x1080, MSAA forced off). UltraPerformance only through the settings code, not run.
- Title, logos, loading and other frames without a 3D scene: no FSR, the picture scaled to the output size, their
  2D drawn at full resolution (as sharp as without FSR); the jitter stops after 3 such frames.
- Camera jitter and motion vectors, measured (not only looked at):
  - jitter sign: `MHGU_JITTER_TEST=40,40` moves the 3D picture exactly 40 px right and 40 px down (image correlation
    of two screenshots), HUD unmoved;
  - motion vectors (`MHGU_FSR_MVCHECK=1`, camera turning and pitching in a quest): mean luma difference to the
    previous frame 0.09-0.12 at the same texel, 0.023-0.036 along the vectors, 0.066-0.139 along the vectors with y
    negated, so direction, scale, y convention and the camera/frame pairing are right;
  - the frame's camera is found in its own uniform buffers every FSR frame (`camera matched 60` a second).
- Vulkan validation (`MHGU_VK_VALIDATION=1` with the SDK's layer): FSR on gives exactly the messages FSR off gives
  (two old ones of the game's renderer), none from FSR.
- MSAA and FXAA are forced off with FSR on (logged; F7 / `msaa` say "MSAA stays off while FSR is on").
- GPU time (MHGU_CONTROL `gpuprof`, quest): 1080p output: prep + FSR ~0.5 ms, frame 1.02 ms (native 1080p frame
  0.85 ms); 4K output: prep + FSR 1.8-2.0 ms, frame 3.04 ms (native 4K 2.55 ms). On this GPU the game is so light
  that FSR Quality costs more than it saves; the gain is anti-aliasing and, on slower GPUs, speed. Frame rate 60
  (vsync) in every test.

## Settings (mhgurecomp.ini, new section; older files get it added with the defaults at the next start)

    [Upscaler]
    FSR = Off              ; On / Off
    FSRQuality = NativeAA  ; NativeAA (1x, default), Quality (1.5x), Balanced (1.7x), Performance (2x), UltraPerformance (3x)
    Sharpness = 0.8        ; RCAS 0 (off) .. 1
    ObjectMotion = On      ; motion vectors of moving characters / monsters from their geometry (Off: camera only)
    Reactive = 0.9         ; reactive mask strength on the game's moving-object mask, 0 (off) .. 1
    AnimSurf = On          ; surfaces animated through their texture: reactive + the current frame mixed in there

With FSR on and Borderless, FSR upscales to the screen (in the picture's shape), not to Resolution. Texture LOD bias
log2(render / output) on mipmapped samplers (-0.58 at Quality). More test overrides: `MHGU_FSR_ANIMSURF`,
`MHGU_FSR_SHARPNESS`, `MHGU_FSR_LODBIAS`, `MHGU_FSR_JITTER_SIGN=sx,sy`, `MHGU_FSR_ANIMVIEW=1` (animated-surface tiles
tinted magenta).

Output size = what the 3D resolution would be without FSR (RenderResolution, or the window's with Auto); the game's
3D resolution (`g_render_size`, patched into the game) = output / ratio, in the picture's shape. Test overrides:
`MHGU_FSR=0|1`, `MHGU_FSR_QUALITY=<name>`, `MHGU_FSR_OBJMOTION=0|1`, `MHGU_FSR_REACTIVE=<0..1>`. Debug: `MHGU_FSR_LOG=1` (a line a second: FSR / scaled frames, history
resets, camera match, where the HUD started), `MHGU_FSR_MVCHECK=1` (with the log: the motion vector check above),
`MHGU_FSR_DEBUG=1` (FFX's own input checks), `MHGU_JITTER_TEST=x,y` (a constant picture shift, FSR on or off),
`MHGU_JITTER_LOG=1` (every camera capture).

## Files

New:
- `third_party/fsr-vulkan/` - FSR 3.1.4 upscaler sources: FidelityFX SDK 1.1.4 host + Vulkan backend + its GLSL (compiled at run time with shaderc, `src/ffx_fsr3_shaderblobs.cpp`),
  and the portable C wrapper `ffxVkPortable*` (FireBurn/FSR-Vulkan subset, minus its loader shim). All MIT, licence files kept (`LICENSE.txt`, `upstream/ffx-1.1.4/sdk/LICENSE.txt`,
  `NOTICE.md`, `UPSTREAM.md`). `src/ffx_mingw_portability.hpp` (new, ours): what MinGW-w64 lacks for the
  MSVC-written SDK sources (`swprintf_s`, `<new>`, `<cmath>`). No binaries at build time.
- `runtime/nvn/vulkan/fsr.c`, `fsr.h` - the frame analysis, stand-ins, prep / motion-vector shader (GLSL compiled at
  run time with shaderc, like the renderer's other built-in shaders), FSR context and dispatch, present.
- `runtime/hle/camera_jitter.c`, `runtime/camera_jitter.h` - main camera capture and the jitter.
- `docs/FSR-PORT.md` - this file.

Changed (small, marked "FSR"):
- `Makefile` - builds the FSR sources into obj/fsr/ and links them into the executable; `-I` for the wrapper header.
- `runtime/config.c/.h` - `[Upscaler]` keys, render size = output / ratio, MSAA / FXAA off, `g_fsr_output`.
- `runtime/main.c` - `camjit_init()`.
- `runtime/nvn/vulkan/exec.c` - `vk_fsr_draw` before a draw's pass; stand-ins in `begin_pass` (render targets),
  `texture_info` / `resolve_feedback` (sampled); `pass_sx/sy` scale viewport (`draw`), scissor (`scissor_rect`) and
  `pass_size` onto them; the ultrawide HUD rewrite also applies to them. Object motion: `Prog.fsrcap` (the vertex
  shader's source kept for its capture variant, `compile_program`), the capture draw after a depth-writing draw
  (`draw()`), its two bindings in `stage_set`, `vk_run_job` (fsr.c's builds on the shader threads).
- `runtime/nvn/vulkan/device.c` - with FSR on: `shaderStorageImageWriteWithoutFormat` (FSR needs it),
  `shaderFloat16` + `shaderInt16` (its 16-bit variants); `vk_fsr_init`; the present shows the stand-in; MSAA keys.
- `runtime/nvn/vulkan/vk.h` - two feature flags in `g_vk`.

Not changed: `translate.c` (the jitter goes into the camera's projection matrix, not the shaders), `textures.c`,
`shaders.c`, `tools/`.

## How it works

Camera (`hle/camera_jitter.c`): main+0x1C33C (look-at, r0 = out, r1 = camera) and main+0x1C358 (projection, r0 = out,
r1 = camera; 0x9c617c builds a row-vector, right-handed, depth 0..1 perspective: m[10] = f/(n-f), m[11] = -1) are
hooked (fnhook). For a camera with the screen's aspect (camera +0x38: 16:9, or the picture's shape after the
AspectRatio patch) each projection call is recorded with that camera's last view (ring of 64 captures: view,
unjittered projection, near +0x34 / far +0x30, fovy) and then gets the jitter: AMD's Halton(2,3) - 0.5 sequence,
8 (output/render)^2 phases, one phase per presented frame, written into m[8] / m[9] of the matrix the GPU gets
(`m[8] -= 2 jx / w`, `m[9] += 2 jy / h`; the translator's clip space has +y up). main+0x1C380 / 0x1C3A8 (the CPU
world-to-screen for HUD markers) are not touched, so markers stay steady.

Frame structure (from MHGU_CONTROL `dump`, quest, FSR off): pass 0 shadow map; 1 colour-grading 256x1; 2 depth
prepass into D (render size, D32S8 on AMD); 3 dynamic-object mask (characters and their shadows, blue; with D);
4-5 the scene (with D); 6-28 post (depth of field, bloom chain, half / quarter sizes); 29 into B; 30 into H:
FXAA (B -> H) + bloom add; 31 H -> B copy; 32 the HUD into H (sprites from a 512x512 atlas, depth / stencil off);
33 H -> the window texture P (the brightness / gamma setting: `pow(c, cb3[0][0])`), presented.

Identification (`fsr.c`, per frame, no texture ids):
- H = the first texture the last draw before a present sampled, when that draw wrote the presented texture
  (learnt at each present, used from the next frame; logged when it changes).
- The scene depth = the render-size depth target with the most depth-writing draws this frame.
- The start of the HUD = the first draw into H that is not a post pass (post pass: <= 6 vertices and samples a
  render target other than H, e.g. the bloom add), or else the draw into a window texture that samples H.
- Scene frame = >= 16 depth-writing draws into the scene depth and a camera capture from this frame or the last.
- The frame's camera capture: the one whose jittered m[8] / m[9] appear in a scene draw's vertex uniform buffers
  (adjacent or 16 bytes apart). In game it is always the capture before the newest one at present time; without a
  match (no jitter yet) the newest one is used.
- Menu skip: frames that are not scene frames get no FSR (scaled); after 3 in a row the jitter stops, the next scene
  frame starts it again (FSR history reset at gaps, camera changes, cuts > 3 m).

At the start of the HUD (outside a render pass): scene frame: prep (compute, render size: D's depth -> R32F; motion
vectors = previous position - current position in render pixels from inverse(view*proj) * (view_prev*proj_prev),
unjittered), then FSR (colour = H as the game finished it: post-processed LDR RGBA8; depth R32F; motion vectors
RG32F; no reactive mask; jitterOffset = the capture's jitter = the picture shift in render pixels, x right / y down;
near / far / fovy from the capture; 0.01 m per unit) into H' = an output-size stand-in of H. Other frames: H scaled
into H' (linear blit). From then until the present, H, the window texture and the render-size depth target of their
passes are replaced by output-size stand-ins (H', P', D' - D' cleared to far): the HUD and the gamma pass render at
the output size, and the present shows P'.

### Temporal quality: object motion and the reactive mask (2026-10-26)

Order at the HUD start (scene frames): prep (depth, camera vectors) -> velocity pass -> finish (compute) -> FSR.
- Capture (`exec.c draw()` -> `vk_fsr_capture`): every depth-writing draw into a render-size colour + depth target
  before the HUD start (not into H) is drawn once more through a variant of its own vertex shader: the translated GLSL
  plus `positions[base + gl_VertexIndex] = gl_Position` (set 0 bindings 30 / 31; rasterizer discard). The variant is
  built on the shader threads the first time the program is captured; until then the draw just isn't captured. The
  positions are the game's own (camera, jitter and skinning in them). Indices are copied to host memory (read from
  the guest's own index data: reading the renderer's write-combined transient copy cost 30 ms a frame).
  Equivalent of the reference's transform feedback; needs `vertexPipelineStoresAndAtomics` and
  `fragmentStoresAndAtomics` (else ObjectMotion is off with a log line, the reactive mask still works). No CPU
  expansion of primitive-restart strips is needed: the velocity pass draws the same indices, topology and restart.
- Pairing with the previous frame: vertex program + index buffer + counts + first + base vertex + topology + vertex
  buffers (repeats in order); without the vertex buffers when unique in both frames; same gl_VertexIndex range.
- Velocity pass: each pair rasterized at the current positions (no culling, no depth attachment) into RGBA16F; a
  fragment keeps (previous px - previous jitter) - (current px - current jitter) where its linear depth is within
  max(2 units, 1%) of the scene's and |mv| <= 64 px.
- Static draws: on every 4th texel the pass notes per draw "seen" / "differs from the camera by > 0.25 px" (read back
  frames later); 240 checked frames without differing -> left out for 600 frames (camera vectors are right for them).
  The notes are atomics into device memory, copied to host memory for the CPU after the pass. (Atomics straight
  into host memory each crossed the bus: in town, 225 draws a frame, the pass took 12 ms of a 15 ms frame on an RTX
  3090 at 1080p, FSR ran at ~28 fps; with the copy 0.2 ms, ~50 fps in the same hidden test run.)
- Finish: object vector where valid, else camera. Reactive from the game's moving-object mask (the first render-size
  colour target drawn with the scene depth, when it is not the main scene target; blue/green channel; characters,
  monsters, their shadows): only where the silhouette changed - outside the 3x3 min/max of the previous frame's
  silhouette at texel + vector (ghost trail behind a moving object, its new leading edge). A standing hunter is in his
  own box: no reactive, so he does not alias more. x Reactive (0.9), capped to 0.1 on texels with valid object vectors.
- HUD / 2D never get vectors or reactive: the capture needs depth writes into the scene targets before the HUD start
  (HUD draws here have depth / stencil off and come after it); the mask is the game's own 3D pass. Checked: 16:9 and
  21:9 quest with pause menu, title, character select, quest loading.

Implementation notes:
- The HUD is drawn after FSR at the output size (stand-ins), so no HUD layer, composite or HUD blend handling
  (alphaToOne / multiply transmittance) is needed.
- Menu 3D (character select, previews): the models are drawn after the HUD start, i.e. straight into H' at the output
  size, unjittered (jitter is off in non-scene frames) - no FSR, no history, so no smear
  (checked: character select, FSR on vs off, same sharpness).
- Object positions are captured by a storage-buffer store in the vertex shader (no transform feedback, no strip
  expansion).
- Reactive mask: the silhouette change of moving objects, written in blue (and green).
- Animated surfaces: a second pass (see the audit below), with characters left out and a half-pixel search.
- Not implemented: block matching for object vectors, a low-rate cloth "jump" reactive mask, pose signals. Every
  depth-writing draw is captured (object vectors measured right on moving geometry), and no cloth that re-poses at a
  low rate was seen (the village cloth moves every frame), so none of them was needed for the artifacts measured.
- The ultrawide UI matrix rewrite (exec.c cbuf_info) only touches depth-test-off draws (HUD), which are never
  captured; runtime/hle/hpscale.c changes monster HP only, nothing in rendering.

Measured (quest 101 area 2, herbivores moving, MHGU_FSR_MVCHECK on texels of the game's moving-object mask, mean luma
difference to the previous frame; same script run with ObjectMotion = Off + Reactive = 0, i.e. the previous build's
behaviour, and with the defaults): hunter walking with the camera following: camera vectors 0.035 / 0.042 -> FSR's
vectors 0.026 (camera-only run: 0.042); standing hunter, moving herbivores: 0.021-0.023 -> 0.019-0.020. Earlier walk
in area 1: 0.113-0.131 -> 0.073-0.093. Screenshots: walking hunter limbs and a herbivore sharper than with camera
vectors only (`cmp_walk.png`: Off / old / new), standing hunter not more aliased (`cmp_stand.png`). 60 fps, capture
CPU 0.03-0.14 ms a frame, ~50-155 draws captured a frame. Validation: no new messages (one, dynamic depth bias after
a capture draw, was found and fixed).

### Audit (2026-10-26, second pass): what was wrong, what was measured

Images (build\shots): `cmp_display_chest.png` / `_ground` / `_distant` (at the 3440x1440 screen: old setup vs FSR
Quality vs FSR NativeAA), `cmp_detail_structure.png` / `_ground` / `_distant` (1920x1080 window, 2x crops: native+FXAA,
FSR Quality 0.3 / 0.8, NativeAA, Quality at 2880x1620, 4K supersampled), `cmp_pan_structure.png` (camera turning).

Found and fixed:
- No texture LOD bias: at a lower render size textures were sampled one mip too blurry. Now log2(render / output).
- Sharpness 0.3 too soft: sharpness relative to the 4K supersampled picture (mean gradient, 4 static regions): native
  1080p 0.93-0.98, FSR Quality 0.3 0.70-0.86 (0.58-0.78 without the LOD bias), FSR Quality 0.8 0.89-1.00. Default 0.8.
- The user's setup (Borderless, Resolution 1920x1080 on a 3440x1440 screen) upscaled 720p to 1080p, which the present
  then stretched to 1440p with a bilinear filter: most of what FSR rebuilt was blurred away again. With FSR on and
  Borderless, FSR now upscales to the screen size in the picture's shape (here 2560x1440: Quality renders 1706x960).
- A draw left out as "static" stayed on camera vectors for up to 600 frames after it started moving. Now woken the
  same frame its own uniform data changes; in the tests most "static" candidates were woken at once (their uniform
  data changes every frame), so in practice almost everything is captured (~150 draws a frame, 0.03-0.14 ms CPU).
- Animated-surface detection had many false positives (47 tiles a frame in the village, among them the hunter's
  head): characters are now left out, the search uses half-pixel steps: ~2 tiles a frame in the village, 1.4 in
  quest 101 area 2. In the tested scenes nothing was found that needed it (the village banners move as geometry and
  get object vectors; the river's texture moves too slowly to cross the threshold), so its visible effect there is
  nil; it costs ~0.1-0.3 ms at 720p.

Verified (numbers from MHGU_FSR_MVCHECK / MHGU_FSR_LOG, quest 101 area 2, herbivores, hunter walking):
- Every candidate draw (~150 a frame) captured; skipped for memory / range / failed variants: 0.00 a frame;
  "not built yet" only while an area loads. Unpaired 0-0.74 a frame (<0.5 %), all "no previous copy": draws that
  appear that frame (level-of-detail switches, objects coming into view). Blended skinned draws: none in these scenes.
- Moving geometry (valid object vector > 0.5 px from the camera's), mean luma difference to the previous frame along:
  object vector 0.013-0.029, camera vector 0.028-0.084, object vector without removing the jitter change 0.025-0.050,
  object vector with y negated 0.029-0.175, same texel 0.041-0.202. The object vectors win in every second measured:
  sign, y convention and jitter removal are right.
- Skinned meshes: the capture draw is recorded right after the game's draw with the same descriptor set contents
  (exec.c stage_set over the same st.ubuf ranges, copied at record time), so each frame's positions use that frame's
  bones; the previous frame's positions come from the previous frame's capture (its own slot of the buffer).
- FSR's jitter sign: the three other sign combinations (MHGU_FSR_JITTER_SIGN) all measured worse (PSNR against the
  supersampled picture down 1-8 dB, sharpness down).
- FSR input: the game's picture after its post effects (depth of field, bloom, tone mapping, FXAA forced off) and
  before its brightness / gamma pass (that runs after FSR, at the output size). The post effects run at the render
  size; moving FSR before them would mean re-doing the game's post passes at the output size (not done).
- Shipped-config run (build\mhgurecomp.ini as the user has it: FSR = On, Quality, Sharpness 0.3): log "FSR upscaling",
  60 FSR frames a second, no fallback; "plain scaling: no 3D scene" only on the title / loading frames.

Compared (1920x1080 window; PSNR dB against the 4K supersampled picture / sharpness / static flicker x1000, four
static regions; full table in the session log):
- native + FXAA (old default): 31.5-36.5 dB, 0.93-0.98, flicker 0.14-3.16; jagged edges, crawl in motion.
- FSR Quality (sharpness 0.8): 28.2-34.1 dB, 0.89-1.00, flicker 0.81-1.19; smooth edges, a little less fine texture
  detail than native (720p input), softer while the camera turns fast. GPU 1.5 ms vs 0.73 ms native here.
- FSR NativeAA: 31.5-36.8 dB, 1.00-1.07, flicker 0.57-1.03; as sharp as native with smooth, stable edges: better than
  native + FXAA. GPU 2.3 ms at 1080p, 3.9 ms at 2560x1440 (RX 9070 XT).
- FSR Quality / NativeAA with RenderResolution 2880x1620 / 2560x1440 on a 1080p window: the closest to the
  supersampled picture (up to 39.5 dB), 2.8 ms.
On this GPU the game is light enough that NativeAA (now the default quality) is the useful setting: it is clearly
better than native + FXAA. Quality and below pay off on slower GPUs or very high resolutions.

## Known issues / not done

- Object motion covers geometry drawn with depth writes. Not covered: transparent / blended effects (no depth write:
  particles, water surface, some hair cards) - they keep camera vectors; texture or UV animation (flipbooks); objects
  whose vertex data moved to another buffer and whose rest is not unique (unpaired, ~1 % of draws). Fast objects
  (> 64 render px a frame) fall back to camera vectors.
- A draw left out as static that starts moving keeps camera vectors until its 600 frames are over (10 s at 60 fps);
  which draws were left out was not checked one by one (the log only counts them: 6-56 at a time in the tests).
- Area-intro cutscenes and other event cameras are shown scaled (no FSR) while the main camera hook does not run.
- Output size is fixed at start-up; a window resized later shows the output scaled (as the 3D picture was).
- After the start of the HUD: copies into H / P go to the render-size originals, and HUD draws that would depth-test
  against the scene's depth see an empty D'; neither was seen in the tested frames (HUD draws have depth / stencil off).
- GPU requirements: subgroup operations, `shaderStorageImageWriteWithoutFormat`, FSR's storage formats; without them
  FSR is off with a log line (the picture is scaled). Not tried on the R9 270X / GCN1 baseline, NVIDIA, Intel, Linux.
- Not tested in game: UltraPerformance, the hunter creation and equipment preview screens (the character select
  was), a large monster in a fight (only herbivores in quest 101's area 2), multiplayer, Alt+Enter / borderless on
  another monitor size.
- FSR 4: see docs/FSR4-PORT.md (`Upscaler = FSR4`, native Vulkan through the driver); block matching / animated surfaces / HUD layer: see above.

## Test commands (Git Bash; the game must not be running while building)

    cd /c/msys64 && MSYSTEM=UCRT64 CHERE_INVOKING=1 ./usr/bin/bash.exe -lc \
        "cd /e/Gaming/GU/MHGURecomp-src && make -j8 GAME=../Windows-MHGURecomp/data"

Unattended run into a quest on a copy of the save (the real save in %APPDATA% is never touched), FSR on, screenshot,
GPU profile, then quit:

    cp -r "$APPDATA/MHGURecomp/save/save_0" <test>/save_0
    cd /e/Gaming/GU/MHGURecomp-src/build && : > ctl.txt
    MHGU_SAVE_DIR=<test> MHGU_WINDOW=1920x1080 MHGU_QUICKHUNT=101 MHGU_CONTROL=ctl.txt MHGU_FPS=1 \
    MHGU_FSR=1 MHGU_FSR_LOG=1 DISABLE_RTSS_LAYER=1 DISABLE_VULKAN_OW_OVERLAY_LAYER=1 ./mhgurecomp.exe > out.txt 2>&1 &
    # ~20 s later "[quickhunt] in town: ordered quest 101" is in out.txt; ~30 s after that the hunt has started:
    echo "shot fsr_on" >> ctl.txt; echo gpuprof >> ctl.txt; echo "RSTICK_RIGHT 180" >> ctl.txt
    taskkill //F //IM mhgurecomp.exe

(Started from a terminal the log goes to stdout, not log.txt. The RTSS / Overwolf Vulkan layers on this machine
crashed one start inside vkCreateDevice; their DISABLE_* variables keep them out of test runs.) Same with
`MHGU_FSR=0` for the reference picture, `MHGU_FSR_QUALITY=performance`, `MHGU_RENDER=3840x2160` (4K output),
`MHGU_FSR_MVCHECK=1`, `MHGU_JITTER_TEST=40,40`, `MHGU_VK_VALIDATION=1 VK_ADD_LAYER_PATH=C:\VulkanSDK\1.4.341.1\Bin`.
Without MHGU_QUICKHUNT, pressing A every 1.5 s for ~20 s reaches the village.

## Quality presets side by side

The same still scene (quest 101, RX 9070 XT, 1920x1080 window with a 21:9 picture, so 1920x822) at every `FSRQuality` preset, FSR 3.1 on the left and FSR 4 on the right (the picture is in the pull request, not in the repository). Each tile is labelled with the resolution the 3D picture is rendered at; the HUD is always drawn at the output resolution. Real captures, nothing edited apart from cropping the black bars and scaling to fit. Frame time on that card (whole-frame GPU ms, FSR 3.1 / FSR 4): NativeAA 1.98 / 2.06, Quality 1.27 / 1.56, Balanced 1.16 / 1.47, Performance 1.04 / 1.37, UltraPerformance 0.87 / 1.23.

## Comparison images

Captures from the test runs, shown in the pull request and not stored in the repository (quest 101, RX 9070 XT, 1920x1080 window, crops of 4-frame captures; nothing edited).
Panel labels in images 1-5: `nat_fxaa` = native 1080p with FXAA (no FSR), `f3q` = FSR 3.1 Quality (1280x720 -> 1080p),
`f4q` = FSR 4 Quality, `f4naa` = FSR 4 NativeAA, `ss4k` = 4K supersampled reference. Within one image the hunter or the
animated parts can be in a slightly different pose per panel (the panels are different runs); compare edges and texture, not poses.

What can be seen across the set (my reading; one scene, one GPU; the numbers are in the measurements above and in `FSR4-PORT.md`):
- Native + FXAA has stair-stepped geometry edges (chest strap and rim, cloth edge, sword); every FSR variant has smooth edges.
- FSR 3.1 Quality and FSR 4 Quality rebuild texture detail from 720p input to about native level, a little softer than native + FXAA on some
  textures; FSR 4 Quality is a little softer than FSR 3.1 Quality (sharpness metrics 0.87-0.94 vs 0.87-1.01).
- FSR 4 NativeAA is the closest to the 4K reference in sharpness; FSR 3.1 NativeAA (images 6-8) is visibly sharper than its Quality mode (sharpness 0.8 vs 0.3 there).
- In the pan crops (4, 5), no ghost trails are visible behind the hunter or on the ground in any FSR panel.
- A thin green outline on the headband is present in native and all FSR panels (a game texture/outline feature, not FSR).

1. Chest and structure, static: native+FXAA, FSR 3.1 Quality, FSR 4 Quality / FSR 4 NativeAA, 4K reference.

2. Ground texture, static.

3. Hunter cloth, sword and armour, static.

4. Camera turning, hunter walking in place: motion / ghosting check on the character.

5. Camera turning, ground.

The next three are FSR 3.1 only, on a 2560x1440 screen: the old setup (native 1920x1080 stretched to the screen), FSR 3.1 Quality
(1706x960 -> 2560x1440, sharpness 0.3) and FSR 3.1 NativeAA (2560x1440, sharpness 0.8).

6. Chest.

7. Hunter and ground.

8. Distant terrain.

## Other upscalers

Nothing in the preparation is specific to FSR: the jittered main camera (`runtime/hle/camera_jitter.c`), per-pixel motion vectors from the
camera and from moving objects, the scene depth, the reactive and animated-surface masks, the HUD split (the HUD is drawn after the upscale at
output size), the texture LOD bias and the output-size handling (`runtime/nvn/vulkan/fsr.c`, `textures.c`) are what any temporal upscaler
needs. The upscaler itself is called from one place: `run_fsr` in `fsr.c` prepares the inputs and then records either the FSR 3.1 dispatch
or `vk_fsr4_dispatch` (`fsr4.c`, which takes all of them in one `Fsr4Dispatch` struct). A DLSS backend (NVIDIA, via NGX or Streamline on
Vulkan) could be added the same way by someone with an NVIDIA RTX card. The author has no such card, so it is neither implemented nor tested.

SMAA T2x (`[Anti-aliasing] AntiAliasing = SMAAT2x`, docs/STATUS.md) uses that preparation at the render size: `frame_vectors` (the prep,
object motion and finish steps `run_fsr` runs first) and then `run_t2x` instead of an upscaler, in place, without stand-ins. For it the
main camera's hooks are always in (`camjit_init`; without jitter they only record the camera), and `vk_fsr_present` is called every frame
(it returns at once while neither FSR nor T2x runs). SMAA 4x (`SMAA4x`) is the same T2x with a smaller jitter, plus `vk_s2x_target`:
which resolve of a multisampled image is the scene's, for `exec.c` to resolve it by SMAA S2x. SMAA S2x alone (`SMAAS2x`) uses only
that: `g_s2x_on` has `exec.c` pass the draws to `vk_fsr_draw`, which counts the scene's draws and returns, and `vk_fsr_present` picks
the scene for the next frame (no jitter, vectors or capture). Object motion is captured in MSAA passes
too (the capture variant discards rasterization, so its pipeline has no sample count to match).
