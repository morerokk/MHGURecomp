// BC1-BC7 decoder (bcdec, third_party/bcdec) for GPUs without BC textures (Raspberry Pi's V3DV, some Mali GPUs):
// the blocks are decoded on the CPU on upload, as runtime/nvn/astc.c does for ASTC on desktop GPUs.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// kind: 1-7 = BC1-BC7. Output per texel: BC1/2/3/7 RGBA8, BC4 R8, BC5 RG8 (signed bytes when snorm), BC6H RGBA16F
// (alpha 1.0).
uint32_t bcn_texel_size(int kind);

// blocks: row-major blocks (8 bytes for BC1 / BC4, else 16) covering ceil(width/4) x ceil(height/4).
// out: width*height texels of bcn_texel_size(kind) bytes, tightly packed rows.
void bcn_decode(int kind, bool snorm, const uint8_t *blocks, uint32_t width, uint32_t height, uint8_t *out);
