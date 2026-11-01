// BC1-BC7 decoder: bcdec (third_party/bcdec, github.com/iOrange/bcdec, MIT / Unlicense) per 4x4 block, into a
// block-sized scratch, then the texels inside the image are copied out (edge blocks are partly outside it).
// BCDEC_BC4BC5_PRECISE: BC4 / BC5 with their signed variants (the SNORM formats).
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
    uint32_t bsize = kind == 1 || kind == 4 ? 8 : 16, ts = bcn_texel_size(kind);
    uint32_t wb = (width + 3) / 4, hb = (height + 3) / 4;
    uint16_t tmp16[4 * 4 * 4];  // (one decoded block of the largest texel, RGBA16F; uint16_t for BC6H's alignment)
    uint8_t *tmp = (uint8_t *)tmp16;
    uint16_t rgb[4 * 4 * 3];
    for (uint32_t by = 0; by < hb; by++) {
        for (uint32_t bx = 0; bx < wb; bx++) {
            const uint8_t *b = blocks + ((uint64_t)by * wb + bx) * bsize;
            int pitch = 4 * (int)ts;
            switch (kind) {
            case 1: bcdec_bc1(b, tmp, pitch); break;
            case 2: bcdec_bc2(b, tmp, pitch); break;
            case 3: bcdec_bc3(b, tmp, pitch); break;
            case 4: bcdec_bc4(b, tmp, pitch, snorm); break;
            case 5: bcdec_bc5(b, tmp, pitch, snorm); break;
            case 6: {  // RGB half floats -> RGBA16F with alpha 1.0
                bcdec_bc6h_half(b, rgb, 4 * 3, snorm);
                for (int i = 0; i < 16; i++) {
                    tmp16[i * 4] = rgb[i * 3], tmp16[i * 4 + 1] = rgb[i * 3 + 1], tmp16[i * 4 + 2] = rgb[i * 3 + 2];
                    tmp16[i * 4 + 3] = 0x3C00;
                }
                break;
            }
            default: bcdec_bc7(b, tmp, pitch); break;
            }
            uint32_t w = width - bx * 4 < 4 ? width - bx * 4 : 4, h = height - by * 4 < 4 ? height - by * 4 : 4;
            for (uint32_t y = 0; y < h; y++)
                memcpy(out + (((uint64_t)by * 4 + y) * width + bx * 4) * ts, tmp + y * pitch, (uint64_t)w * ts);
        }
    }
}
