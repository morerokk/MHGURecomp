// The game's main 3D camera, captured for the FSR upscaler, and the sub-pixel jitter FSR needs in it
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

void camjit_init(void);  // hooks (only with [Upscaler] FSR = On)
// Renderer: jitter on or off from the next capture on, the 3D render size and the Halton sequence's length.
void camjit_set(bool on, uint32_t render_w, uint32_t render_h, uint32_t phases);
bool camjit_latest(CamRecord *out);              // the newest capture (false: none yet)
bool camjit_get(uint64_t seq, CamRecord *out);   // capture seq, while it is in the ring (the last 64)
// Menu cameras (other aspects: the smithy's equipment preview): the ones computed this frame or the last, as their
// projection's m[0] / m[5] (up to max; returns how many). The renderer gives a camera's viewport (render pixels) once
// it has seen a draw with it; from then on that camera gets the same jitter as the main one, in its viewport's pixels.
int camjit_menu_cameras(float (*m05)[2], int max);
void camjit_menu_viewport(int i, float w, float h);
