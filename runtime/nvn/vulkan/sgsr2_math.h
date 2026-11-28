// The data SGSR 2's two fragment shaders consume, and the conversion of the port's motion vectors into theirs.
// Kept independent of Vulkan so tests/test_sgsr2.c can check the layout and the sign conventions on the host.
#pragma once
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// The std140 Params block of glsl_2_pass_fs/sgsr2_convert.fs and sgsr2_upscale.fs (128 bytes).
typedef struct {
    float clip[16];       // 0:   vec4 clipToPrevClip[4]
    float render[2];      // 64:  vec2 renderSize
    float output[2];      // 72:  vec2 outputSize
    float render_rcp[2];  // 80:  vec2 renderSizeRcp
    float output_rcp[2];  // 88:  vec2 outputSizeRcp
    float jitter[2];      // 96:  vec2 jitterOffset, render pixels, +y down
    float scale[2];       // 104: vec2 scaleRatio
    float fov_hor;        // 112: float cameraFovAngleHor
    float min_lerp;       // 116: float minLerpContribution
    float reset;          // 120: float reset
    uint32_t same_camera; // 124: uint bSameCamera
} Sgsr2Params;
_Static_assert(sizeof(Sgsr2Params) == 128, "the SGSR 2 UBO must match the shaders' std140 Params block");
_Static_assert(offsetof(Sgsr2Params, render) == 64 && offsetof(Sgsr2Params, output) == 72 &&
               offsetof(Sgsr2Params, render_rcp) == 80 && offsetof(Sgsr2Params, output_rcp) == 88 &&
               offsetof(Sgsr2Params, jitter) == 96 && offsetof(Sgsr2Params, scale) == 104 &&
               offsetof(Sgsr2Params, fov_hor) == 112 && offsetof(Sgsr2Params, min_lerp) == 116 &&
               offsetof(Sgsr2Params, reset) == 120 && offsetof(Sgsr2Params, same_camera) == 124,
               "the SGSR 2 UBO fields must match the GLSL declaration order");

// The port's object vectors are (previous - current) in render pixels, +y down; the shaders decode (current - previous)
// in NDC with +y up (REQUEST_NDC_Y_UP). This is the inverse of the shaders' decodeVelocityFromTexture(). It is used
// only for moving objects: static pixels are written as literal zero so the shaders reproject them from depth.
static inline void sgsr2_encode_motion(float mvx, float mvy, uint32_t rw, uint32_t rh, float encoded[2]) {
    encoded[0] = (-2.0f * mvx / rw) * 0.2495f + 0.5f;
    encoded[1] = (2.0f * mvy / rh) * 0.2495f + 0.5f;
}

static inline void sgsr2_params(Sgsr2Params *p, const float clip[16], uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh,
                                const float jitter[2], float fovy, bool same, bool reset, int same_frames) {
    memset(p, 0, sizeof *p);
    memcpy(p->clip, clip, sizeof p->clip);  // row-vector order: GLSL sees its transpose, which is what convert needs
    // At a history reset fsr.c has no previous camera and leaves its matrix all zero: the convert shader would then
    // divide 0 by 0 (PreClip.w = 0) and NaN handling differs between drivers. Identity = no camera motion.
    if (reset) {
        memset(p->clip, 0, sizeof p->clip);
        p->clip[0] = p->clip[5] = p->clip[10] = p->clip[15] = 1.0f;
    }
    p->render[0] = (float)rw, p->render[1] = (float)rh;
    p->output[0] = (float)ow, p->output[1] = (float)oh;
    p->render_rcp[0] = 1.0f / rw, p->render_rcp[1] = 1.0f / rh;
    p->output_rcp[0] = 1.0f / ow, p->output_rcp[1] = 1.0f / oh;
    p->jitter[0] = jitter[0], p->jitter[1] = jitter[1];
    p->scale[0] = (float)ow / rw;
    p->scale[1] = fminf(20.0f, powf(p->scale[0] * (float)oh / rh, 3.0f));
    p->fov_hor = tanf((fovy > 0.05f && fovy < 3.0f ? fovy : 1.0f) * 0.5f) * (float)rw / rh;
    p->min_lerp = same_frames > 5 ? 0.3f : 0.0f;
    p->same_camera = same ? 1u : 0u;
    p->reset = reset ? 1.0f : 0.0f;
}
