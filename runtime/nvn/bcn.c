// BC1-BC7 decoding with bcdec (third_party/bcdec). BCDEC_BC4BC5_PRECISE gives the signed BC4 / BC5 variants.
#include "bcn.h"

#include <string.h>

#define BCDEC_IMPLEMENTATION
#define BCDEC_STATIC
#define BCDEC_BC4BC5_PRECISE
#include "../../third_party/bcdec/bcdec.h"

uint32_t bcn_texel_size(int kind) {
    switch (kind) {
    case 4: return 1;
    case 5: return 2;
    case 6: return 8;
    default: return 4;
    }
}

void bcn_decode(int kind, bool snorm, const uint8_t *blocks, uint32_t width, uint32_t height, uint8_t *out) {
    uint32_t block_size = kind == 1 || kind == 4 ? 8 : 16;
    uint32_t texel_size = bcn_texel_size(kind);
    int pitch = 4 * (int)texel_size;  // one row of the decoded 4x4 block
    uint16_t block16[4 * 4 * 4];      // a decoded block at the largest texel size (RGBA16F)
    uint8_t *block = (uint8_t *)block16;
    uint16_t rgb[4 * 4 * 3];
    for (uint32_t by = 0; by < (height + 3) / 4; by++) {
        for (uint32_t bx = 0; bx < (width + 3) / 4; bx++) {
            const uint8_t *src = blocks + ((uint64_t)by * ((width + 3) / 4) + bx) * block_size;
            switch (kind) {
            case 1: bcdec_bc1(src, block, pitch); break;
            case 2: bcdec_bc2(src, block, pitch); break;
            case 3: bcdec_bc3(src, block, pitch); break;
            case 4: bcdec_bc4(src, block, pitch, snorm); break;
            case 5: bcdec_bc5(src, block, pitch, snorm); break;
            case 6:  // RGB halves -> RGBA16F, alpha 1.0
                bcdec_bc6h_half(src, rgb, 4 * 3, snorm);
                for (int i = 0; i < 16; i++) {
                    block16[i * 4 + 0] = rgb[i * 3 + 0];
                    block16[i * 4 + 1] = rgb[i * 3 + 1];
                    block16[i * 4 + 2] = rgb[i * 3 + 2];
                    block16[i * 4 + 3] = 0x3C00;
                }
                break;
            default: bcdec_bc7(src, block, pitch); break;
            }
            // copy the texels inside the image (edge blocks reach past it)
            uint32_t w = width - bx * 4, h = height - by * 4;
            if (w > 4) w = 4;
            if (h > 4) h = 4;
            uint8_t *dst = out + ((uint64_t)by * 4 * width + bx * 4) * texel_size;
            for (uint32_t y = 0; y < h; y++)
                memcpy(dst + (uint64_t)y * width * texel_size, block + y * pitch, w * texel_size);
        }
    }
}
