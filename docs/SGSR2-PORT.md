# SGSR 2 upscaler (2026-11-26)

[Snapdragon Game Super Resolution 2](https://github.com/SnapdragonGameStudios/snapdragon-gsr/tree/main/sgsr/v2)
(SGSR 2) is Qualcomm's temporal upscaler for Adreno GPUs. The 2-pass fragment variant is available as an alternative
to FSR 3.1, aimed at the mobile GPUs in issue #465 (for example the Adreno 725 in the POCO F5). It is in-tree, not an
engine module: engine modules are not available on Android, and the upscaler needs inputs from the port's prep passes.

## Using it

    [Upscaler]
    FSR = On
    Method = SGSR2        ; FSR3 (default) or SGSR2
    FSRQuality = Quality  ; the same render scale as FSR 3.1

`MHGU_UPSCALER=SGSR2` overrides the setting (for A/B runs). The settings menu has the same choice under
Graphics → Upscaler method; changing it needs a restart. With `FSR = Off` neither upscaler runs. HDR is off while
upscaling is on, as before.

Everything else is shared with FSR 3.1: the jittered camera, the scene-frame detection, the output-size stand-ins for
the HUD, the texture LOD bias, object motion capture, animated-surface detection and history resets. `docs/FSR-PORT.md`
describes that preparation; only the final dispatch differs. SGSR 2 uses no reactive mask (`Reactive` has no effect
with it). It has no sharpening of its own, so its final copy into H' runs AMD's RCAS (the filter FSR 3.1 runs after
its upscale), with `Sharpness` mapped as FSR maps it and read each frame; the history stays unsharpened. In town at
Performance, 0.8 took the mean neighbour difference (a sharpness measure) from 6.4 to 7.6, against 8.9 native and 9.4
for FSR 3.1 at 0.8.

## What was added

* `third_party/sgsr2/` - Qualcomm's verbatim 2-pass-fs GLSL (`sgsr2_convert.fs`, `sgsr2_upscale.fs`), commit
  `d926f074bcb9d714e179f1ce0fcb9ee2eeb5074e`, BSD-3-Clause. `src/sgsr2_glsl.c` embeds them into the executable with
  `.incbin`; shaderc compiles them at run time, as it does the renderer's other shaders.
* `runtime/nvn/vulkan/sgsr2.c` - the three passes around those shaders:
  1. a compute pass encodes the port's motion vectors into SGSR 2's format. The port's vectors are render pixels
     (previous - current, +y down); SGSR 2 decodes `(ev - 0.5) / 0.2495` into clip-space motion (current - previous,
     +y up). Only moving objects get a vector; static pixels get zero so SGSR 2 reprojects them from depth with
     `clipToPrevClip`. This avoids the camera vectors that FSR needs everywhere. The encoded image is RGBA32F: the
     decode assumes 16-bit UNORM precision, and RGBA16F's steps near 0.5 (~1 render pixel of motion) made static
     objects waver.
  2. Qualcomm's Convert pass at render size: colour + D24S8 depth + encoded velocity -> RGBA16F motion/depth/clip.
  3. Qualcomm's Upscale pass at output size, into one of two ping-ponged RGBA16F history images, then a small copy to
     the game's RGBA8 H'. The UBO is the std140 `Params` block from the two shaders (128 bytes), filled from the
     camera, jitter, matrices and render/output sizes that `fsr.c` already computes.
* `runtime/nvn/vulkan/fsr.c` - `run_fsr` prepares the inputs as before and then calls either the FFX dispatch or
  `vk_sgsr2_dispatch`. `vk_fsr_init` applies FSR 3.1's device-capability gate only to FSR 3.1: SGSR 2 needs Vulkan 1.1,
  storage images with declared formats, and no FFX features.
* `runtime/config.c/.h`, `runtime/settings_panel.cpp`, `runtime/settings_locale.h` - the `Method` setting.
* `runtime/nvn/vulkan/shaders.c/.h` - `vk_glsl_to_spirv_stage_ex`, the existing shader compiler with an explicit
  stage (the previous `_ex` entry point was compute-only).

The shaders are compiled with `REQUEST_NDC_Y_UP=1`: the game's clip space and the port's texture coordinates both
have +y at the top. Its depth is forward-Z (0 near, 1 far), so the reverse-Z edits in Qualcomm's README are **not**
applied.

## Verified and not verified

Verified here: the two vendored fragment shaders compile to SPIR-V for Vulkan 1.1 and 1.2 with `REQUEST_NDC_Y_UP`
(glslang `main-tot`); the UBO layout matches the shaders' declarations (128 bytes); the interface points and build
wiring follow the FSR 3.1 port.

Not verified here: anything that needs the game or an Adreno GPU. In particular the image formats on Turnip, driver
quirks with `textureGather` on a depth image, the visual result during camera pans and animated surfaces, and the
frame-time win claimed in issue #465. These need A/B runs on the reporter's POCO F5:

    MHGU_FSR=1 MHGU_UPSCALER=fsr3  MHGU_FSR_LOG=1 gpuprof
    MHGU_FSR=1 MHGU_UPSCALER=sgsr2 MHGU_FSR_LOG=1 gpuprof

with the same quest, render scale and scene; check for ghosting, flicker, and that HUD/menu frames are still drawn
at output resolution. `MHGU_VK_VALIDATION=1` should show no new validation messages.
