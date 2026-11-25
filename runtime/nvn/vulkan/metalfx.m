// MetalFX temporal upscaling under MoltenVK ([Upscaler] MetalFX = On; see metalfx.h).
//
// The renderer is Vulkan on MoltenVK; MetalFX takes Metal objects. VK_EXT_metal_objects (device.c enables it) hands
// out the MTLDevice, the MTLTexture behind each VkImage and the MTLSharedEvent behind a timeline semaphore. A frame:
//   1. fsr.c records its prep passes (depth, motion vectors, reactive mask) as for FSR and submits them (submission S,
//      which signals the renderer's timeline g_vk.timeline to S when done);
//   2. vk_mfx_encode, on our own MTLCommandQueue: wait for the timeline's event to reach S, the scaler, signal our
//      event to N;
//   3. the next Vulkan submission waits for our timeline semaphore (whose event that is) to reach N
//      (exec.c vk_wait_next_submit), so the HUD and the present are drawn over the upscaled picture.
// Two queues and shared events rather than MoltenVK's own queue: the order then never depends on when MoltenVK commits
// its command buffers, or on hazard tracking of its textures.
//
//   MHGU_MFX_JITTER_SIGN=sx,sy  (debug) other sign conventions for the jitter offset, to check this one
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include "metalfx.h"
#include <vulkan/vulkan_metal.h>

// (rt.h's LOG; rt.h itself clashes with the NEON types Metal's headers bring in)
#ifndef LOG
#define LOG(...) fprintf(stderr, __VA_ARGS__)
#endif

static PFN_vkExportMetalObjectsEXT export_objects;
static VkDevice vdev;
static id<MTLDevice> mtl_dev;
static id<MTLCommandQueue> mtl_queue;
static id<MTLSharedEvent> ev_timeline;  // the renderer's timeline semaphore's: submission numbers
static VkSemaphore mfx_sem;              // our timeline semaphore; ev_done is its event
static id<MTLSharedEvent> ev_done;
static uint64_t done_value;
static id<MTLTexture> exposure_one;      // 1x1 R16F = 1.0 (the input is already the final, LDR picture)
static id<MTLFXTemporalScaler> scaler;
static MTLPixelFormat sc_color, sc_depth, sc_motion, sc_react, sc_out;
static bool sc_failed;
static uint32_t rw, rh, ow, oh;
static float jsign[2] = {1.0f, 1.0f};
static _Atomic uint64_t gpu_ns, gpu_n;  // (vk_mfx_gpu_ns) the scaler's command buffers' GPU time, from their handlers

uint32_t vk_mfx_gpu_ns(uint64_t *total_ns) {
    *total_ns = atomic_exchange(&gpu_ns, 0);
    return (uint32_t)atomic_exchange(&gpu_n, 0);
}

static id<MTLTexture> texture_of(VkImage image) {
    if (!image) return nil;
    VkExportMetalTextureInfoEXT ti = {.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT, .image = image,
                                      .plane = VK_IMAGE_ASPECT_PLANE_0_BIT};
    VkExportMetalObjectsInfoEXT oi = {.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT, .pNext = &ti};
    export_objects(vdev, &oi);
    return ti.mtlTexture;
}

static id<MTLSharedEvent> event_of(VkSemaphore s) {
    VkExportMetalSharedEventInfoEXT ei = {.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_SHARED_EVENT_INFO_EXT, .semaphore = s};
    VkExportMetalObjectsInfoEXT oi = {.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT, .pNext = &ei};
    export_objects(vdev, &oi);
    return ei.mtlSharedEvent;
}

bool vk_mfx_init(VkDevice dev, VkSemaphore timeline, uint32_t render_w, uint32_t render_h, uint32_t out_w, uint32_t out_h) {
    @autoreleasepool {
        vdev = dev;
        rw = render_w, rh = render_h, ow = out_w, oh = out_h;
        export_objects = (PFN_vkExportMetalObjectsEXT)vkGetDeviceProcAddr(dev, "vkExportMetalObjectsEXT");
        if (!export_objects) { LOG("[mfx] no vkExportMetalObjectsEXT: MetalFX off\n"); return false; }
        VkExportMetalDeviceInfoEXT di = {.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_DEVICE_INFO_EXT};
        VkExportMetalObjectsInfoEXT oi = {.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT, .pNext = &di};
        export_objects(dev, &oi);
        mtl_dev = di.mtlDevice;
        if (!mtl_dev) { LOG("[mfx] MoltenVK gave no MTLDevice: MetalFX off\n"); return false; }
        if (![MTLFXTemporalScalerDescriptor supportsDevice:mtl_dev]) {
            LOG("[mfx] %s has no MetalFX temporal scaling: MetalFX off\n", mtl_dev.name.UTF8String);
            return false;
        }
        float lo = 1.0f, hi = 2.0f;
        if (@available(macOS 14.0, *)) {
            lo = [MTLFXTemporalScalerDescriptor supportedInputContentMinScaleForDevice:mtl_dev];
            hi = [MTLFXTemporalScalerDescriptor supportedInputContentMaxScaleForDevice:mtl_dev];
        }
        float sx = (float)ow / rw, sy = (float)oh / rh;
        if (sx < lo - 0.01f || sy < lo - 0.01f || sx > hi + 0.01f || sy > hi + 0.01f) {
            LOG("[mfx] %ux%u -> %ux%u (x%.2f) is outside what MetalFX scales on %s (x%.2f..x%.2f): MetalFX off\n", rw, rh, ow,
                oh, sx, mtl_dev.name.UTF8String, lo, hi);
            return false;
        }
        ev_timeline = event_of(timeline);
        VkExportMetalObjectCreateInfoEXT mex = {.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT,
                                                .exportObjectType = VK_EXPORT_METAL_OBJECT_TYPE_METAL_SHARED_EVENT_BIT_EXT};
        VkSemaphoreTypeCreateInfo st = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, .pNext = &mex,
                                        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0};
        VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &st};
        if (vkCreateSemaphore(dev, &si, NULL, &mfx_sem) != VK_SUCCESS) { LOG("[mfx] no semaphore: MetalFX off\n"); return false; }
        ev_done = event_of(mfx_sem);
        if (!ev_timeline || !ev_done) {
            LOG("[mfx] MoltenVK gave no MTLSharedEvent for its timeline semaphores: MetalFX off\n");
            return false;
        }
        mtl_queue = [mtl_dev newCommandQueue];
        mtl_queue.label = @"MetalFX";
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR16Float width:1
                                                                                     height:1 mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModeShared;
        exposure_one = [mtl_dev newTextureWithDescriptor:td];
        const uint16_t one = 0x3c00;  // (1.0 as a half)
        [exposure_one replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&one bytesPerRow:2];
        const char *e = getenv("MHGU_MFX_JITTER_SIGN");
        if (e && sscanf(e, "%f,%f", &jsign[0], &jsign[1]) != 2) jsign[0] = jsign[1] = 1.0f;
        LOG("[mfx] MetalFX temporal scaler on %s: %ux%u -> %ux%u (it scales x%.2f..x%.2f here), jitter sign %g,%g\n",
            mtl_dev.name.UTF8String, rw, rh, ow, oh, lo, hi, jsign[0], jsign[1]);
        return true;
    }
}

// The scaler for these textures' formats (made again when one changes: SmoothGradients' 16-bit picture, for example).
static bool scaler_for(id<MTLTexture> c, id<MTLTexture> d, id<MTLTexture> m, id<MTLTexture> r, id<MTLTexture> o) {
    MTLPixelFormat rf = r ? r.pixelFormat : MTLPixelFormatInvalid;
    if (scaler && sc_color == c.pixelFormat && sc_depth == d.pixelFormat && sc_motion == m.pixelFormat && sc_react == rf &&
        sc_out == o.pixelFormat)
        return true;
    if (sc_failed) return false;
    MTLFXTemporalScalerDescriptor *ds = [MTLFXTemporalScalerDescriptor new];
    ds.colorTextureFormat = c.pixelFormat;
    ds.depthTextureFormat = d.pixelFormat;
    ds.motionTextureFormat = m.pixelFormat;
    ds.outputTextureFormat = o.pixelFormat;
    ds.inputWidth = rw, ds.inputHeight = rh;
    ds.outputWidth = ow, ds.outputHeight = oh;
    ds.autoExposureEnabled = NO;
    if (r) {
        if (@available(macOS 14.4, *)) {
            ds.reactiveMaskTextureEnabled = YES;
            ds.reactiveMaskTextureFormat = rf;
        } else {
            rf = MTLPixelFormatInvalid;
        }
    }
    scaler = [ds newTemporalScalerWithDevice:mtl_dev];
    if (!scaler) {
        LOG("[mfx] MetalFX made no temporal scaler for colour %lu, depth %lu, motion %lu, output %lu: plain scaling\n",
            (unsigned long)c.pixelFormat, (unsigned long)d.pixelFormat, (unsigned long)m.pixelFormat, (unsigned long)o.pixelFormat);
        sc_failed = true;
        return false;
    }
    sc_color = c.pixelFormat, sc_depth = d.pixelFormat, sc_motion = m.pixelFormat, sc_react = rf, sc_out = o.pixelFormat;
    // (the usages MetalFX asks for: MoltenVK made them from the images' Vulkan usages)
    struct { id<MTLTexture> t; MTLTextureUsage need; const char *what; } chk[] = {
        {c, scaler.colorTextureUsage, "colour"}, {d, scaler.depthTextureUsage, "depth"},
        {m, scaler.motionTextureUsage, "motion"}, {o, scaler.outputTextureUsage, "output"}};
    for (int i = 0; i < 4; i++)
        if ((chk[i].t.usage & chk[i].need) != chk[i].need)
            LOG("[mfx] the %s texture's usage %#lx lacks %#lx that MetalFX asks for\n", chk[i].what,
                (unsigned long)chk[i].t.usage, (unsigned long)chk[i].need);
    if (o.storageMode != MTLStorageModePrivate)
        LOG("[mfx] the output texture's storage mode is %lu, not private as MetalFX asks\n", (unsigned long)o.storageMode);
    LOG("[mfx] temporal scaler: colour %lu, depth %lu, motion %lu, reactive %lu, output %lu\n", (unsigned long)sc_color,
        (unsigned long)sc_depth, (unsigned long)sc_motion, (unsigned long)sc_react, (unsigned long)sc_out);
    return true;
}

bool vk_mfx_encode(const MfxDispatch *f, uint64_t wait_value, VkSemaphore *done, uint64_t *done_val) {
    @autoreleasepool {
        id<MTLTexture> c = texture_of(f->color), d = texture_of(f->depth), m = texture_of(f->motion),
                       r = texture_of(f->reactive), o = texture_of(f->output);
        if (!c || !d || !m || !o) {
            static int nlog;
            if (nlog++ < 20) LOG("[mfx] MoltenVK gave no MTLTexture for an input or the output: not upscaled\n");
            return false;
        }
        if (!scaler_for(c, d, m, r, o)) return false;
        // (the wait in a command buffer of its own: the scaler's then times the scaler alone, vk_mfx_gpu_ns)
        id<MTLCommandBuffer> wcb = [mtl_queue commandBuffer];
        wcb.label = @"MetalFX wait";
        [wcb encodeWaitForEvent:ev_timeline value:wait_value];
        [wcb commit];
        id<MTLCommandBuffer> cb = [mtl_queue commandBuffer];
        cb.label = @"MetalFX upscale";
        scaler.colorTexture = c;
        scaler.depthTexture = d;
        scaler.motionTexture = m;
        scaler.outputTexture = o;
        scaler.exposureTexture = exposure_one;
        if (sc_react != MTLPixelFormatInvalid) {
            if (@available(macOS 14.4, *)) scaler.reactiveMaskTexture = r;
        }
        scaler.inputContentWidth = rw, scaler.inputContentHeight = rh;
        scaler.jitterOffsetX = jsign[0] * f->jitter[0];
        scaler.jitterOffsetY = jsign[1] * f->jitter[1];
        scaler.motionVectorScaleX = 1.0f;  // (render pixels, current + motion = previous: as MetalFX's 1.0 means)
        scaler.motionVectorScaleY = 1.0f;
        scaler.preExposure = 1.0f;
        scaler.depthReversed = NO;
        scaler.reset = f->reset;
        [scaler encodeToCommandBuffer:cb];
        [cb encodeSignalEvent:ev_done value:++done_value];
        [cb addCompletedHandler:^(id<MTLCommandBuffer> b) {
            if (b.status != MTLCommandBufferStatusCompleted || b.GPUEndTime <= b.GPUStartTime) return;
            atomic_fetch_add(&gpu_ns, (uint64_t)((b.GPUEndTime - b.GPUStartTime) * 1e9));
            atomic_fetch_add(&gpu_n, 1);
        }];
        [cb commit];
        *done = mfx_sem, *done_val = done_value;
        return true;
    }
}
