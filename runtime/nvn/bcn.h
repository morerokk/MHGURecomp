// BC1-BC5 (S3TC / RGTC) decoder, for GPUs without textureCompressionBC: Android's Adreno and Mali drivers (see
// docs/research/android.md). MHGU_NO_BC=1 makes a desktop GPU act the same, to test it.
#pragma once
#include <stdint.h>

// BC1 / BC2 / BC3 -> RGBA8, BC4 -> R8, BC5 -> RG8; U: UNORM, S: SNORM (two's complement bytes).
enum { BCN_NONE, BCN_BC1, BCN_BC2, BCN_BC3, BCN_BC4U, BCN_BC4S, BCN_BC5U, BCN_BC5S };

uint32_t bcn_texel_size(int kind);  // bytes per decoded texel: 4, 1 (BC4) or 2 (BC5)

// blocks: row-major 4x4 blocks (8 bytes for BC1 / BC4, else 16) covering ceil(width/4) x ceil(height/4).
// out: width*height decoded texels, tightly packed rows.
void bcn_decode(int kind, const uint8_t *blocks, uint32_t width, uint32_t height, uint8_t *out);
