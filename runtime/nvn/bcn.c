// BC1-BC5 decoder (bcn.h), after the Khronos Data Format spec's S3TC and RGTC chapters:
//   colour (BC1-BC3): RGB565 endpoints widened to 8 bits by bit replication, the two middle colours at 1/3 and 2/3
//   (rounded). BC1 with color0 <= color1 has the midpoint and transparent black instead (this port makes every BC1
//   texture BC1_RGBA, as on the GPU); BC2 / BC3 always use four colours.
//   channels (BC4, BC5, the BC3 alpha): two 8-bit endpoints and six interpolated values (endpoint0 > endpoint1) or
//   four and the range's ends (UNORM 0 / 255, SNORM -127 / 127), rounded to the nearest 8-bit value. The SNORM
//   endpoint -128 counts as -127 (both are -1.0).
// A GPU interpolates with more than 8 bits and its own rounding: results here can differ from its decode by a code.
#include "bcn.h"

#include <stdbool.h>
#include <string.h>

static inline uint32_t rd16(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8; }
static inline uint32_t rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }
static inline uint64_t rd48(const uint8_t *p) { return rd32(p) | (uint64_t)rd16(p + 4) << 32; }
static inline int div_round(int n, int d) { return n >= 0 ? (n + d / 2) / d : -((-n + d / 2) / d); }

// the colour half of a BC1-BC3 block -> 16 RGBA texels
static void color_block(const uint8_t *b, bool four, uint8_t out[16][4]) {
    uint32_t c0 = rd16(b), c1 = rd16(b + 2), idx = rd32(b + 4);
    uint8_t pal[4][4];
    for (int e = 0; e < 2; e++) {
        uint32_t c = e ? c1 : c0, r = c >> 11, g = (c >> 5) & 63, bl = c & 31;
        pal[e][0] = (uint8_t)(r << 3 | r >> 2), pal[e][1] = (uint8_t)(g << 2 | g >> 4), pal[e][2] = (uint8_t)(bl << 3 | bl >> 2);
        pal[e][3] = 255;
    }
    if (four || c0 > c1) {
        for (int k = 0; k < 3; k++) {
            pal[2][k] = (uint8_t)((2 * pal[0][k] + pal[1][k] + 1) / 3);
            pal[3][k] = (uint8_t)((pal[0][k] + 2 * pal[1][k] + 1) / 3);
        }
        pal[2][3] = pal[3][3] = 255;
    } else {
        for (int k = 0; k < 3; k++) pal[2][k] = (uint8_t)((pal[0][k] + pal[1][k] + 1) / 2);
        pal[2][3] = 255;
        memset(pal[3], 0, 4);
    }
    for (int i = 0; i < 16; i++) memcpy(out[i], pal[(idx >> (2 * i)) & 3], 4);
}

// one channel (8 bytes) -> 16 values, at out[i * stride]
static void channel_block(const uint8_t *b, bool snorm, uint8_t *out, int stride) {
    int e0 = snorm ? (int8_t)b[0] : b[0], e1 = snorm ? (int8_t)b[1] : b[1];
    bool six = e0 > e1;  // (compared as stored: -128 and -127 are different encodings)
    if (e0 < -127) e0 = -127;
    if (e1 < -127) e1 = -127;
    int pal[8] = {e0, e1};
    if (six) {
        for (int k = 1; k < 7; k++) pal[k + 1] = div_round((7 - k) * e0 + k * e1, 7);
    } else {
        for (int k = 1; k < 5; k++) pal[k + 1] = div_round((5 - k) * e0 + k * e1, 5);
        pal[6] = snorm ? -127 : 0, pal[7] = snorm ? 127 : 255;
    }
    uint64_t idx = rd48(b + 2);
    for (int i = 0; i < 16; i++) out[i * stride] = (uint8_t)pal[(idx >> (3 * i)) & 7];
}

uint32_t bcn_texel_size(int kind) { return kind <= BCN_BC3 ? 4 : kind <= BCN_BC4S ? 1 : 2; }

void bcn_decode(int kind, const uint8_t *blocks, uint32_t width, uint32_t height, uint8_t *out) {
    uint32_t ts = bcn_texel_size(kind), bsz = kind == BCN_BC1 || kind == BCN_BC4U || kind == BCN_BC4S ? 8 : 16;
    uint32_t wb = (width + 3) / 4, hb = (height + 3) / 4;
    for (uint32_t by = 0; by < hb; by++) {
        for (uint32_t bx = 0; bx < wb; bx++, blocks += bsz) {
            uint8_t t[16][4];  // the block's texels, rows of 4 * ts bytes
            uint8_t *tb = &t[0][0];
            switch (kind) {
            case BCN_BC1: color_block(blocks, false, t); break;
            case BCN_BC2:
                color_block(blocks + 8, true, t);
                for (int i = 0; i < 16; i++) t[i][3] = (uint8_t)(((blocks[i / 2] >> (4 * (i & 1))) & 15) * 17);
                break;
            case BCN_BC3: color_block(blocks + 8, true, t), channel_block(blocks, false, &t[0][3], 4); break;
            case BCN_BC4U: case BCN_BC4S: channel_block(blocks, kind == BCN_BC4S, tb, 1); break;
            default: channel_block(blocks, kind == BCN_BC5S, tb, 2), channel_block(blocks + 8, kind == BCN_BC5S, tb + 1, 2); break;
            }
            uint32_t w = width - bx * 4 < 4 ? width - bx * 4 : 4, h = height - by * 4 < 4 ? height - by * 4 : 4;
            for (uint32_t y = 0; y < h; y++)
                memcpy(out + ((uint64_t)(by * 4 + y) * width + bx * 4) * ts, tb + y * 4 * ts, w * ts);
        }
    }
}
