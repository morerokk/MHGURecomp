// FSR 4 (AMD's machine-learning upscaler) as an alternative to FSR 3.1 ([Upscaler] Upscaler = FSR4): runtime/nvn/vulkan/fsr4.c.
// It takes exactly the inputs fsr.c prepares for FSR 3.1 and is dispatched where the FSR 3.1 dispatch would be.
// Windows only: the upscaler is AMD's FSR SDK Vulkan upscaler (amd_fidelityfx_loader_vk.dll); on other systems, or when it
// cannot start, everything here says "no" and fsr.c keeps using FSR 3.1.
#pragma once
#include "vk.h"

typedef struct {
    VkImage image;
    VkFormat format;
    uint32_t width, height;
} Fsr4Image;

typedef struct {
    VkCommandBuffer cb;
    Fsr4Image color, depth, motion, reactive, output;  // reactive.image == VK_NULL_HANDLE: none; all live in GENERAL
    float jitter[2];                                    // the picture shift in render pixels (as for FSR 3.1)
    uint32_t render[2], out[2];
    float frame_ms, cam_near, cam_far, fovy, view_to_meters, sharpness;
    bool sharpen, reset;
} Fsr4Dispatch;

// device.c create_device, only for FSR on + Upscaler = FSR4: the device extensions / features the driver's FSR 4 asks
// for (cooperative matrix, float8, ...). exts: before the extension list is used up; features: once the feature chain
// exists. Both return false when the GPU lacks something (then FSR 4 is not attempted and FSR 3.1 runs).
bool vk_fsr4_device_exts(const VkExtensionProperties *ep, uint32_t n, const char **ext, uint32_t *n_ext);
bool vk_fsr4_device_features(VkPhysicalDeviceVulkan11Features *f11, VkPhysicalDeviceVulkan12Features *f12,
                             VkPhysicalDeviceFeatures *f);

bool vk_fsr4_init(void);    // fsr.c vk_fsr_init, device exists: load the driver provider, check the device. false: FSR 3.1
bool vk_fsr4_ready(void);   // FSR 4 loaded and has not failed (yet)
const char *vk_fsr4_name(void);  // "FSR 4.1.2"
// Create (first time) the context for these sizes and record the upscale into d->cb. false: it failed, FSR 4 is off
// from now on (logged) and the caller falls back to FSR 3.1.
bool vk_fsr4_dispatch(const Fsr4Dispatch *d);
