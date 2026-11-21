// MetalFX temporal upscaling ([Upscaler] MetalFX = On, macOS): Apple's upscaler in place of FSR 3.1's dispatch, on the
// inputs fsr.c prepares for FSR (runtime/nvn/vulkan/metalfx.m). Everything else of fsr.c (where the HUD starts, the
// stand-ins, the jitter, object motion, the reactive mask) is the same for both.
// (Plain Vulkan types only: metalfx.m can't include vk.h, whose NEON helpers clash with the Metal headers' own.)
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

typedef struct {
    VkImage color;     // render size: the full-screen image H
    VkImage depth;     // render size, R32F, 0 near .. 1 far
    VkImage motion;    // render size, RG32F: previous position - current, in render pixels (FSR's convention)
    VkImage reactive;  // render size, R32F, 0..1 (VK_NULL_HANDLE: none)
    VkImage output;    // output size: H's stand-in
    float jitter[2];   // the picture shift of this frame's jitter, render pixels, x right, y down
    bool reset;        // drop the history (a cut)
} MfxDispatch;

#ifdef __APPLE__
// Once, after the device exists (with VK_EXT_metal_objects, timeline semaphores, `timeline` made exportable as an
// MTLSharedEvent): false (logged why) when MetalFX can't run here (a GPU without temporal scaling, a scale it doesn't
// support).
bool vk_mfx_init(VkDevice dev, VkSemaphore timeline, uint32_t render_w, uint32_t render_h, uint32_t out_w, uint32_t out_h);
// Encodes MetalFX on its own Metal queue, after `timeline` reaches wait_value (the submission that wrote the inputs).
// true: *done / *done_value is the timeline semaphore and value it signals when finished, for the next Vulkan submission
// to wait for. false: nothing encoded (logged).
bool vk_mfx_encode(const MfxDispatch *d, uint64_t wait_value, VkSemaphore *done, uint64_t *done_value);
// (MHGU_FSR_LOG) the GPU time of the scaler's command buffers completed since the last call: their count, total ns.
uint32_t vk_mfx_gpu_ns(uint64_t *total_ns);
#else
static inline bool vk_mfx_init(VkDevice dev, VkSemaphore timeline, uint32_t render_w, uint32_t render_h, uint32_t out_w,
                               uint32_t out_h) {
    (void)dev, (void)timeline, (void)render_w, (void)render_h, (void)out_w, (void)out_h;
    return false;
}
static inline bool vk_mfx_encode(const MfxDispatch *d, uint64_t wait_value, VkSemaphore *done, uint64_t *done_value) {
    (void)d, (void)wait_value, (void)done, (void)done_value;
    return false;
}
static inline uint32_t vk_mfx_gpu_ns(uint64_t *total_ns) { *total_ns = 0; return 0; }
#endif
