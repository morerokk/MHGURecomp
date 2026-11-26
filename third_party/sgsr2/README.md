# Snapdragon Game Super Resolution 2 (SGSR 2)

This directory holds the two fragment shaders of Qualcomm's SGSR 2 (the 2-pass-fs
variant, Qualcomm's recommendation for mobile / Adreno GPUs):

* `glsl/sgsr2_convert.fs`
* `glsl/sgsr2_upscale.fs`

Source: https://github.com/SnapdragonGameStudios/snapdragon-gsr, commit
`d926f074bcb9d714e179f1ce0fcb9ee2eeb5074e`, path `sgsr/v2/include/glsl_2_pass_fs/`.
License: BSD-3-Clause, Copyright (c) 2023 Qualcomm Innovation Center, Inc. The
license is in `LICENSE` and reproduced in the repository's THIRD_PARTY_NOTICES.
Qualcomm's own integration guide is kept as `README.upstream.md`.

The GLSL files are verbatim. The integration in `runtime/nvn/vulkan/sgsr2.c`:

* compiles them with `-DREQUEST_NDC_Y_UP=1` (the game's clip space and texture
  coordinates have +y at the top; see the "Pass NDC Considerations" section of
  the upstream README), and uses its own fullscreen-triangle vertex shader with
  explicit `location` qualifiers;
* supplies the `Params` UBO from the game's camera / jitter;
* feeds `InputVelocity` from the port's existing prep + motion-vector passes,
  encoded as clip-space motion * 0.2495 + 0.5, with zero for static pixels so
  SGSR 2 reprojects them from depth;
* renders Convert into an RGBA16F motion/depth/clip target and Upscale into one
  of two ping-ponged RGBA16F history images (as Qualcomm's Vulkan sample does),
  then writes the clamped result into the game's full-resolution RGBA8 picture.

`src/sgsr2_glsl.c` embeds the two files into the executable with `.incbin`, so
no shader file has to be shipped next to the program (the files are compiled
with shaderc the first time SGSR 2 is used, like the renderer's other shaders).
