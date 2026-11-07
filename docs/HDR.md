# Native HDR output (2026-11-06)

`[HDR] HDR = On` shows the game on an HDR display as HDR10 (BT.2020, PQ) or scRGB (linear BT.709, FP16), whichever
the display offers (`HDRFormat = Auto` prefers HDR10). Default: Off. With it off nothing of it runs: render targets,
shaders and the window image are exactly as before.

It is a port of the approach of the RenoDX HDR mod for this game (by souperman9, on the RenoDX framework by ShortFuse;
MIT licence, https://github.com/clshortfuse/renodx), which works on Ryujinx through ReShade. Here it is done inside the
renderer: no ReShade, no shader ids from an emulator.

**Status: built, not yet tested in game.** Needs checking on an HDR display, with FSR off.

## How it works

The game renders into RGBA8 targets: everything above SDR white clips at each step. With HDR on:

1. **16-bit float targets** (`textures.c vk_rt_widen`, `WIDE_FLOAT16`): the post chain from the colour grading on -
   B (the grading's target), H (the full-screen image FXAA, the bloom add and the HUD are drawn into) and P (the window
   texture the brightness pass writes) - are rendered as RGBA16F. With `UnclampedScene = On` (default) every other RGBA8
   target the game renders into is too (the scene, depth of field, the bloom chain at half size and below), so the scene's own lighting above white (sun, fire, sparks, specular) reaches the grading.
   Copies between these targets and guest memory convert (exec.c).
2. **The game's colour grading and bloom add as HDR variants** (`ShaderKey.hdr`, `hdr.c vk_hdr_wrap_fs`): the game's
   own translated shader runs (`main()` renamed), then its output is decoded to linear light (sRGB), graded with the
   user's settings, tone mapped and written in an intermediate encoding where 1.0 is UI white:
   - grading: RenoDX's `ApplyUserColorGrading` (exposure; contrast with flare; highlights; shadows; saturation and
     highlight saturation - "blowout" - in OkLab; AP1 clamp), the same slider scaling as RenoDX (50 = neutral);
   - tone mapper: `RenoDRT` = Neutwo per channel (`p x / sqrt(x^2 + p^2)`, peak p = PeakBrightness / GameBrightness,
     gamma-corrected), what RenoDX's MHGU mod's DisplayMap uses; `None` = no curve (clipped at the peak at the end);
     `Vanilla` = clipped to SDR (the game's look, at GameBrightness);
   - intermediate (RenoDX `RenderIntermediatePass`): gamma correction (2.2 / 2.4), scaled by GameBrightness /
     UIBrightness, encoded back.
   The bloom add is scaled by `Bloom` first and is no longer clamped.
3. FXAA, the HUD (drawn on top in [0, 1], so at UI brightness) and the game's brightness pass (`pow(c, cb3[0][0])`)
   run unchanged on the wider values.
4. **Output** (`device.c` + `hdr.c vk_hdr_encode`): the game's picture and the settings overlay are drawn into a
   window-size RGBA16F composition image; the final pass decodes it (gamma correction), scales to UIBrightness, clamps
   to PeakBrightness in BT.2020 and writes PQ or scRGB. HDR metadata (`VK_EXT_hdr_metadata`): MaxCLL = peak,
   MaxFALL = game brightness.

### Finding the passes

From the frame structure (`docs/FSR-PORT.md`, "Frame structure"), every frame:
- FXAA is recognised by its fragment shader's code hash (as `exec.c` does); the texture it samples is B.
- The colour grading is the last full-screen draw into B before FXAA; its fragment program's code hash is learnt from
  one frame and used from the next.
- H is what the frame's last draw sampled when it wrote the presented texture (as fsr.c learns it).
- The bloom add: a full-screen draw into H after FXAA, sampling a smaller render target (the bloom chain), before the first other draw
  into H (the HUD).

Log lines: `[hdr] colour grading target`, `[hdr] full-screen image`, `[hdr] colour grading: fragment program ...`,
`[vk] render target ... RGBA16F (HDR)`. The first frame after starting (or after B / H change) is drawn as SDR.

## Settings (all live)

ToneMapper, Flare, Bloom and UnclampedScene are ini only (not in the settings menu).

| Key | Range | Default | |
|---|---|---|---|
| `HDR` | Off / On | Off | |
| `PeakBrightness` | 80 - 10000 nits | 1000 | the display's peak |
| `GameBrightness` | 48 - 500 nits | 203 | the game's white |
| `UIBrightness` | 48 - 500 nits | 203 | HUD, menus, overlay |
| `ToneMapper` | Vanilla / None / RenoDRT | RenoDRT | |
| `GammaCorrection` | Off / 2.2 / BT.1886 | 2.2 | |
| `Exposure` | 0 - 2 | 0.65 | |
| `Highlights`, `Shadows`, `Contrast`, `Saturation`, `HighlightSaturation` | 0 - 100 | 60, 50, 50, 50, 82 | 50 = the game's |
| `Flare` | 0 - 100 | 0 | |
| `Bloom` | 0 - 100 % | 100 | the game's bloom |
| `UnclampedScene` | Off / On | On | everything the game renders in 16-bit float (scene, depth of field, bloom) |
| `HDRFormat` (ini only) | Auto / HDR10 / scRGB | Auto | |

`MHGU_HDR=0|1` overrides `HDR`.

Every other fragment shader is compiled with its colour outputs bounded as an RGBA8 target bounded them - NaN and
infinity to 0, nothing below 0, alpha at most 1 - but colour above 1 kept (to 256): `hdr.c vk_hdr_guard_fs`. Without
it a particle whose shader made NaN (0 / 0, which RGBA8 stored as 0) blackened its whole square, even where its alpha
was 0. It is in every program, HDR on or off (programs are built once, and HDR can be turned on while playing);
rendering into RGBA8 (HDR off) it changes nothing.

FSR is off while HDR is on (config.c; the settings menu shows "Off (HDR)"): FSR's input stays RGBA8, so the picture
would be SDR-bright. Turning HDR on while FSR runs applies fully at the next start (HDR's row is marked " *").

## Wide colour gamut

From the colour grading on, a graded frame's picture is in BT.2020 (linear BT.709 -> BT.2020 before the tone mapper,
in the intermediate encoding), so colours outside BT.709 that the grading makes (Saturation, HighlightSaturation)
stay positive through the game's later passes (FXAA, its brightness pass's `pow`, the NaN guard's clamp), which lost
them as negative BT.709 values. The HUD drawn into H is converted as it is drawn (`HDR_PASS_HUD`, its BT.709 colours
-> BT.2020), and the picture goes into the composition image as BT.709 again, signed (`present.frag.hdr`), where the
overlay is drawn in BT.709 and the window image's pass turns it into BT.2020 / scRGB. A frame without the grading's
HDR variant (loading screens, movies, the first frame) stays BT.709 throughout.

## HDR analysis

`[HDR] Analysis = On` (the settings menu: HDR analysis) draws, over the game, what the display gets - roughly what
Lilium's HDR analysis shows with ReShade, on every platform (also Android, where there is no ReShade): in the top left
the picture's luminance in nits (max / avg / min, and the middle of the screen, marked by a small cross) and its
brightest channel (CLL), the share of pixels within BT.709, DCI-P3 and BT.2020, and the settings; in the bottom right
a waveform (brightness by screen column, 0.01 - 10000 nits on a log scale; red: PeakBrightness, yellow:
GameBrightness). A compute pass measures the presented picture (every other pixel each way) with the window image's
own maths, before the overlay is drawn over it (`hdr.c vk_hdr_analyze`, read back without waiting); the overlay draws
it (`overlay.cpp draw_hdr_analysis`). Off: nothing of it runs.

## Limits

- FSR: off while HDR is on (FSR's input, H, stays RGBA8: FSR 3/4 take no other format here).
- `UnclampedScene`: values above 1 reach shaders that never saw them on the Switch. NaN / infinity are cleaned in the
  grading, but a scene effect could still look different; turn it off if something looks wrong (the post chain stays
  16-bit float either way).
- Screenshots (`MHGU_SHOT_AT`, the control file's `shot`) are of the composition image: the intermediate encoding,
  clipped to 8 bits.
