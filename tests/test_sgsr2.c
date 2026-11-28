// Host-only checks of the SGSR 2 UBO layout and the port->SGSR motion conversion. No Vulkan, game files or shaders.
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../runtime/nvn/vulkan/sgsr2_math.h"

int main(void) {
    float clip[16];
    for (int i = 0; i < 16; i++) clip[i] = (float)(i + 1);
    float jitter[2] = {0.25f, -0.5f};
    Sgsr2Params p;
    sgsr2_params(&p, clip, 1200, 540, 2400, 1080, jitter, 1.0f, false, true, 0);
    for (int i = 0; i < 16; i++) assert(p.clip[i] == (i % 5 == 0 ? 1.0f : 0.0f));  // reset: identity, not fsr.c's zeros
    assert(p.render[0] == 1200 && p.render[1] == 540 && p.output[0] == 2400 && p.output[1] == 1080);
    assert(fabsf(p.render_rcp[0] - 1.0f / 1200) < 1e-9f && fabsf(p.output_rcp[1] - 1.0f / 1080) < 1e-9f);
    assert(p.jitter[0] == 0.25f && p.jitter[1] == -0.5f);
    assert(fabsf(p.scale[0] - 2.0f) < 1e-6f && p.scale[1] == 20.0f);  // (2x2)^3, clamped as in Qualcomm's sample
    assert(fabsf(p.fov_hor - tanf(0.5f) * 1200.0f / 540.0f) < 1e-6f);
    assert(p.min_lerp == 0.0f && p.same_camera == 0 && p.reset == 1.0f);
    sgsr2_params(&p, clip, 1200, 540, 2400, 1080, jitter, 1.0f, true, false, 6);
    assert(!memcmp(p.clip, clip, sizeof clip));
    assert(p.min_lerp == 0.3f && p.same_camera == 1 && p.reset == 0.0f);
    sgsr2_params(&p, clip, 1200, 540, 2400, 1080, jitter, 1.0f, true, false, 5);
    assert(p.min_lerp == 0.0f);
    sgsr2_params(&p, clip, 1200, 540, 2400, 1080, jitter, 0.0f, true, false, 0);
    assert(fabsf(p.fov_hor - tanf(0.5f) * 1200.0f / 540.0f) < 1e-6f);  // the invalid-fovy fallback

    // A moving object: current pixel (100, 100), previous pixel (90, 90): mv = previous - current = (-10, -10).
    float encoded[2];
    sgsr2_encode_motion(-10.0f, -10.0f, 1200, 540, encoded);
    float motion_x = (encoded[0] - 0.5f) / 0.2495f, motion_y = (encoded[1] - 0.5f) / 0.2495f;
    assert(fabsf(motion_x - 20.0f / 1200) < 1e-5f && fabsf(motion_y + 20.0f / 540) < 1e-5f);
    // The upscale shader (REQUEST_NDC_Y_UP): PrevUV = (u - 0.5*motion.x, v + 0.5*motion.y).
    float prev_u = 100.0f / 1200 - 0.5f * motion_x, prev_v = 100.0f / 540 + 0.5f * motion_y;
    assert(fabsf(prev_u - 90.0f / 1200) < 1e-5f && fabsf(prev_v - 90.0f / 540) < 1e-5f);
    // A still object: zero NDC motion, and the encoder's 0.5 sentinel is a positive texture value as the shader expects.
    sgsr2_encode_motion(0.0f, 0.0f, 1200, 540, encoded);
    assert(encoded[0] == 0.5f && encoded[1] == 0.5f && encoded[0] > 0.0f);
    puts("sgsr2 math ok");
    return 0;
}
