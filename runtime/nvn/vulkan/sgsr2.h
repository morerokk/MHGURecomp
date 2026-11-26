// SGSR 2 ([Upscaler] Method = SGSR2): the mobile 2-pass fragment upscaler, run by fsr.c after its shared prep passes.
// The frame analysis, stand-ins, camera jitter, object motion and history resets are FSR's (runtime/nvn/vulkan/fsr.c);
// this file only records the two SGSR 2 passes and the small pass that encodes the port's motion vectors for them.
#pragma once
#include "../../camera_jitter.h"
#include "fsr.h"

bool vk_sgsr2_init(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh);
// `clip` is the 16 floats fsr.c already computes for its prep pass (inv(current VP) * previous VP, row-vector order).
// `objvel` is the moving objects' render-pixel vectors with a valid flag (img_vel); static pixels get no vector and
// SGSR 2 reprojects their depth instead (Qualcomm's dynamic-objects-only convention).
bool vk_sgsr2_dispatch(VkCommandBuffer cb, TexBackend *color, TexBackend *depth, TexBackend *objvel, TexBackend *out,
                       const float clip[16], const CamRecord *cam, bool objvel_on, bool same_camera, bool reset);
