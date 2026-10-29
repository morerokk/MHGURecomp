// The game's main 3D camera, captured for the FSR upscaler and SMAA T2x, and the sub-pixel jitter they need in it
// (runtime/hle/camera_jitter.c; used by runtime/nvn/vulkan/fsr.c).
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint64_t seq;              // capture number (1, 2, ...): one per call of the main camera's projection
    uint64_t present;          // g_present_count when it was captured
    float view[16], proj[16];  // the game's matrices, row-vector (clip = world * view * proj); proj without the jitter
    float jitter[2];           // the jitter put into this capture's projection: picture shift in render pixels, x right,
                               // y down (0, 0: none)
    float gpu8, gpu9;          // the projection's m[8], m[9] as written (jitter in): found again in the GPU's data
    float near, far, fovy;     // clip planes (world units), vertical field of view (radians)
    uint32_t cam;              // the camera object (guest address)
} CamRecord;

void camjit_init(void);  // hooks (the jitter stays off until FSR or SMAA T2x turns it on)
// Renderer: jitter on or off from the next capture on, the 3D render size and the Halton sequence's length.
void camjit_set(bool on, uint32_t render_w, uint32_t render_h, uint32_t phases);
// SMAA T2x's two jitters (a quarter pixel right and down, then left and up, a frame each) in place of the Halton
// sequence while on; smaa4x: SMAA 4x's (an eighth of a pixel right and up, then left and down); camjit_t2x_phase:
// which of the two a capture's jitter is (0 or 1).
void camjit_t2x(bool on, bool smaa4x);
int camjit_t2x_phase(const float jitter[2]);
bool camjit_latest(CamRecord *out);              // the newest capture (false: none yet)
bool camjit_get(uint64_t seq, CamRecord *out);   // capture seq, while it is in the ring (the last 64)
