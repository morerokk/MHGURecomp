// BC1-BC7 decoder, for GPUs without BC texture support (see textures.c bc_cpu_format).
#pragma once
#include <stdbool.h>
#include <stdint.h>

// kind 1-7 = BC1-BC7. Texels: RGBA8 (BC1/2/3/7), R8 (BC4), RG8 (BC5), RGBA16F (BC6H).
uint32_t bcn_texel_size(int kind);

// blocks: ceil(width/4) x ceil(height/4) blocks, row-major. out: width x height texels, tightly packed.
void bcn_decode(int kind, bool snorm, const uint8_t *blocks, uint32_t width, uint32_t height, uint8_t *out);
