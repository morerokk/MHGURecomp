// Vulkan instance, device, swapchain and presentation (on the game window from runtime/ui.c).
//   MHGU_VK_VALIDATION=1   Khronos validation layer (MSYS2 package vulkan-validation-layers)
//   MHGU_GPU=<index>       which Vulkan device (default: the first discrete GPU)
//   MHGU_VSYNC=0|1         0: no vsync (tearing possible, lowest latency), 1: classic vsync (FIFO). Default: the
//                          settings' VSync - On: mailbox (no tearing; the game paces itself), FIFO where there is
//                          none (the R9's driver), Off: immediate. MHGU_VK_PRESENT=fifo|mailbox|immediate picks the present mode directly.
//   MHGU_SHOT_AT=<frame>[,<frame>...]   save the window image at those presented frames as shots/frame_N.png
//   MHGU_FPS=1             frames per second in the log every 2 s
//   MHGU_NO_BC=1           act as if the GPU had no BC textures, as phone GPUs (they are decoded on the CPU then)
//   MHGU_VK11=1            take the path for Vulkan 1.1 devices on any device (g_vk.api11), to test it on a PC
//   MHGU_NO_LOADING_SCREEN=1   no loading indicator before the game's first frame (a black window, as before)
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../engine_module.h"
#include "../../overlay.h"
#include "fsr.h"
#include "hdr.h"
#include "vk.h"

VkG g_vk;
static VkDebugUtilsMessengerEXT messenger;
// Windows: the swapchain says it never wants exclusive full screen (VK_EXT_full_screen_exclusive). Otherwise NVIDIA's
// driver takes a borderless window covering the screen into exclusive mode, and alt-tab or F11 then blacks out the
// monitor until its "Vulkan/OpenGL present method" is set to "Prefer layered on DXGI Swapchain" (issue #100).
// VkSurfaceFullScreenExclusiveInfoEXT and its enum live in vulkan_win32.h, which needs windows.h: the same layout here.
typedef struct {
    VkStructureType sType;
    void *pNext;
    int32_t fullScreenExclusive;  // VkFullScreenExclusiveEXT
} FullScreenExclusiveInfo;
enum { FULL_SCREEN_EXCLUSIVE_DISALLOWED = 2 };  // VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT
static bool surface_caps2, no_exclusive;  // VK_KHR_get_surface_capabilities2 (instance), VK_EXT_full_screen_exclusive
// HDR ([HDR] HDR, hdr.c): VK_EXT_swapchain_colorspace (instance; without it no surface lists HDR formats) and
// VK_EXT_hdr_metadata (device; the display is told the peak brightness)
static bool colorspace_ext;
static PFN_vkSetHdrMetadataEXT set_hdr_metadata;

static pthread_mutex_t g_vk_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile bool g_vk_owner;  // who holds it: exit() may run inside a submission, from the guest's own code
static volatile bool exiting;     // exit() has begun: the driver's own teardown may be one step away
static pthread_t vk_exit_thread;  // the thread running exit(): it may still enter here (the atexit handler drains)

bool vk_exiting(void) { return exiting; }

void vk_lock(void) {
    pthread_mutex_lock(&g_vk_mutex);
    if (exiting && pthread_self() != vk_exit_thread) {  // a late guest thread: park it away from the driver
        pthread_mutex_unlock(&g_vk_mutex);
        for (;;) sleep(3600);  // (not pause(): mingw-w64 has no pause, sleep is in both unistd.h)
    }
    if (g_vk_owner != pthread_self()) g_vk_owner = pthread_self();  // (re-entry by the holder: leave it recorded)
}

void vk_unlock(void) { pthread_mutex_unlock(&g_vk_mutex); }

// ================================================================== instance / device
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT type,
                                               const VkDebugUtilsMessengerCallbackDataEXT *d, void *user) {
    (void)type, (void)user;
    static int n;
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT && n++ < 500) LOG("[vk] %s\n", d->pMessage);
    return VK_FALSE;
}

// Overwolf's implicit Vulkan layers hook vkCreateDevice and crash the process on startup, so switch them off for
// this process (the loader reads these when the instance is created). MHGU_KEEP_VK_OVERLAYS=1 leaves them alone.
static void disable_overwolf_layers(void) {
    if (getenv("MHGU_KEEP_VK_OVERLAYS")) return;
    static const char *const vars[] = {"DISABLE_VULKAN_OW_OVERLAY_LAYER", "DISABLE_VULKAN_OW_OBS_CAPTURE"};
    for (size_t i = 0; i < sizeof vars / sizeof *vars; i++)
        if (!getenv(vars[i])) SDL_setenv_unsafe(vars[i], "1", 0);
}

static void create_instance(void) {
    disable_overwolf_layers();
    Uint32 n_sdl = 0;
    const char *const *sdl_ext = SDL_Vulkan_GetInstanceExtensions(&n_sdl);
    if (!sdl_ext) FATAL("SDL_Vulkan_GetInstanceExtensions: %s (is the window a Vulkan window?)\n", SDL_GetError());
    const char *ext[16];
    uint32_t n_ext = 0;
    for (Uint32 i = 0; i < n_sdl && n_ext < 13; i++) ext[n_ext++] = sdl_ext[i];
    uint32_t ni = 0;  // VK_EXT_full_screen_exclusive (create_device) needs this one
    vkEnumerateInstanceExtensionProperties(NULL, &ni, NULL);
    VkExtensionProperties *ip = calloc(ni ? ni : 1, sizeof *ip);
    vkEnumerateInstanceExtensionProperties(NULL, &ni, ip);
    for (uint32_t i = 0; i < ni; i++) {
        if (!strcmp(ip[i].extensionName, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME)) surface_caps2 = true;
        else if (!strcmp(ip[i].extensionName, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME)) colorspace_ext = true;
    }
    free(ip);
    if (surface_caps2) ext[n_ext++] = VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME;
    if (colorspace_ext) ext[n_ext++] = VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME;  // (HDR window images)
    bool validate = getenv("MHGU_VK_VALIDATION") != NULL;
    if (validate) ext[n_ext++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "MHGURecomp",
                             .pEngineName = "MHGURecomp",
                             .apiVersion = VK_API_VERSION_1_2};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                               .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext,
                               .enabledLayerCount = validate ? 1 : 0, .ppEnabledLayerNames = layers};
    VkResult r = vkCreateInstance(&ci, NULL, &g_vk.inst);
    if (r != VK_SUCCESS && validate) {
        LOG("[vk] validation layer unavailable (%d); continuing without\n", r);
        ci.enabledLayerCount = 0;
        ci.enabledExtensionCount--;
        validate = false;
        r = vkCreateInstance(&ci, NULL, &g_vk.inst);
    }
    if (r != VK_SUCCESS) FATAL("vkCreateInstance: %d (is a Vulkan 1.1 driver installed?)\n", r);
    if (validate) {
        PFN_vkCreateDebugUtilsMessengerEXT f =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_vk.inst, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mi = {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
            .pfnUserCallback = debug_cb};
        if (f) f(g_vk.inst, &mi, NULL, &messenger);
    }
}

static bool has_ext(VkExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return true;
    return false;
}

// Discrete GPU first (MHGU_GPU=<index> picks one), every Vulkan 1.2 device before any 1.1 one, a GPU's own
// driver before Windows' D3D12 one; needs a queue that can
// draw, compute and present.
// The device can't run the game: the player sees why, not "the details are in log.txt" (which a phone can't easily open)
static void no_vulkan(const char *why) {
    LOG("[vk] %s (Vulkan 1.1 required)\n", why);
    plat_error_box("This device's GPU driver does not support Vulkan 1.1, which the game needs. A system or GPU driver "
                   "update may help.");
    exit(1);
}

static void pick_device(void) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g_vk.inst, &n, NULL);
    if (!n) FATAL("no Vulkan device\n");
    VkPhysicalDevice *pd = calloc(n, sizeof *pd);
    vkEnumeratePhysicalDevices(g_vk.inst, &n, pd);
    int want = getenv("MHGU_GPU") ? atoi(getenv("MHGU_GPU")) : -1, best = -1, best_score = -100;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd[i], &p);
        LOG("[vk] gpu %u: %s (api %u.%u.%u)\n", i, p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
            VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion));
        if (p.apiVersion < VK_API_VERSION_1_1) continue;
        int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if (p.apiVersion < VK_API_VERSION_1_2) score -= 3;
        // Windows' "OpenCL, OpenGL, and Vulkan Compatibility Pack" (Mesa's Dozen) lists each GPU a second time, as
        // "Microsoft Direct3D12 (...)": Vulkan run over D3D12, and listed first. It crashes inside D3D12Core.dll when
        // a quest loads, so the GPU's own driver goes first whenever there is one.
        if (!strncmp(p.deviceName, "Microsoft Direct3D12", 20)) score -= 4;
        if ((int)i == want) score = 100;
        if (score > best_score) { best_score = score; best = (int)i; }
    }
    if (best < 0) no_vulkan("no Vulkan 1.1 device");
    g_vk.phys = pd[best];
    free(pd);
    vkGetPhysicalDeviceProperties(g_vk.phys, &g_vk.props);
    g_vk.api11 = g_vk.props.apiVersion < VK_API_VERSION_1_2 || (getenv("MHGU_VK11") && atoi(getenv("MHGU_VK11")));
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nq, NULL);
    VkQueueFamilyProperties *qp = calloc(nq, sizeof *qp);
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nq, qp);
    g_vk.qfam = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(g_vk.phys, i, g_vk.surface, &present);
        if ((qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && present) { g_vk.qfam = i; break; }
    }
    free(qp);
    if (g_vk.qfam == UINT32_MAX) FATAL("no graphics+present queue\n");
    LOG("[vk] using %s, driver %#x\n", g_vk.props.deviceName, g_vk.props.driverVersion);
}

static void create_device(void) {
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne, sizeof *ep);
    vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ne, ep);
    const char *ext[16];
    uint32_t n_ext = 0;
    ext[n_ext++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    g_vk.custom_border = has_ext(ep, ne, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
    if (g_vk.custom_border) ext[n_ext++] = VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME;
    g_vk.divisor = has_ext(ep, ne, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
    if (g_vk.divisor) ext[n_ext++] = VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME;
    g_vk.stencil_export = has_ext(ep, ne, VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME);  // (MSAA stencil resolve)
    if (g_vk.stencil_export) ext[n_ext++] = VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME;
    no_exclusive = surface_caps2 && has_ext(ep, ne, "VK_EXT_full_screen_exclusive") && !getenv("MHGU_VK_EXCLUSIVE");
    if (no_exclusive) ext[n_ext++] = "VK_EXT_full_screen_exclusive";
    // (only where DEPTH24_STENCIL8 is made as D32S8: textures.c; MHGU_VK_NO_BIAS_CONTROL=1 leaves it off)
#ifdef VK_EXT_depth_bias_control
    bool bias_ctl = vk_depth24_stencil8() == VK_FORMAT_D32_SFLOAT_S8_UINT &&
                    has_ext(ep, ne, VK_EXT_DEPTH_BIAS_CONTROL_EXTENSION_NAME) && !getenv("MHGU_VK_NO_BIAS_CONTROL");
    if (bias_ctl) {
        VkPhysicalDeviceDepthBiasControlFeaturesEXT bc = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_BIAS_CONTROL_FEATURES_EXT};
        VkPhysicalDeviceFeatures2 q = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &bc};
        vkGetPhysicalDeviceFeatures2(g_vk.phys, &q);
        bias_ctl = bc.depthBiasControl && bc.leastRepresentableValueForceUnormRepresentation;
    }
    if (bias_ctl) ext[n_ext++] = VK_EXT_DEPTH_BIAS_CONTROL_EXTENSION_NAME;
#endif
    bool hdr_meta = has_ext(ep, ne, VK_EXT_HDR_METADATA_EXTENSION_NAME);  // (HDR: hdr.c)
    if (hdr_meta) ext[n_ext++] = VK_EXT_HDR_METADATA_EXTENSION_NAME;
    if (g_vk.api11) {  // (Vulkan 1.2 has it as the feature samplerMirrorClampToEdge)
        g_vk.no_mirror_clamp = !has_ext(ep, ne, VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME);
        if (!g_vk.no_mirror_clamp) ext[n_ext++] = VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME;
    }
    free(ep);

    // optional features: whatever the GPU has of what the backend can use
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT div_have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT cbc_have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT,
                                                             .pNext = g_vk.divisor ? &div_have : NULL};
    VkPhysicalDeviceVulkan12Features f12_have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                                 .pNext = g_vk.custom_border ? (void *)&cbc_have : g_vk.divisor ? (void *)&div_have : NULL};
    VkPhysicalDeviceVulkan11Features f11_have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &f12_have};
    // (a Vulkan 1.1 device doesn't know the Vulkan11 / Vulkan12 structures: f12_have stays all off there)
    VkPhysicalDevice16BitStorageFeatures s16_have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES,
                                                     .pNext = f12_have.pNext};
    VkPhysicalDeviceFeatures2 have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                      .pNext = g_vk.api11 ? (void *)&s16_have : (void *)&f11_have};
    vkGetPhysicalDeviceFeatures2(g_vk.phys, &have);
    g_vk.fences = !f12_have.timelineSemaphore;
    g_vk.divisor = g_vk.divisor && div_have.vertexAttributeInstanceRateDivisor;
    g_vk.custom_border = g_vk.custom_border && cbc_have.customBorderColors && cbc_have.customBorderColorWithoutFormat;

    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT div = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT, .vertexAttributeInstanceRateDivisor = VK_TRUE};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT cbc = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, .customBorderColors = VK_TRUE,
        .customBorderColorWithoutFormat = VK_TRUE, .pNext = g_vk.divisor ? &div : NULL};
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                            .timelineSemaphore = f12_have.timelineSemaphore, .shaderInt8 = f12_have.shaderInt8,
                                            .samplerMirrorClampToEdge = f12_have.samplerMirrorClampToEdge,
                                            .uniformAndStorageBuffer8BitAccess = f12_have.uniformAndStorageBuffer8BitAccess,
                                            .storageBuffer8BitAccess = f12_have.storageBuffer8BitAccess,
                                            .scalarBlockLayout = f12_have.scalarBlockLayout,
                                            .pNext = g_vk.custom_border ? (void *)&cbc : g_vk.divisor ? (void *)&div : NULL};
    VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
                                            .storageBuffer16BitAccess = f11_have.storageBuffer16BitAccess, .pNext = &f12};
    VkPhysicalDevice16BitStorageFeatures s16 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES,
                                                .storageBuffer16BitAccess = s16_have.storageBuffer16BitAccess, .pNext = f12.pNext};
    VkPhysicalDeviceFeatures2 f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                   .pNext = g_vk.api11 ? (void *)&s16 : (void *)&f11};
    VkPhysicalDeviceFeatures *b = &f.features, *h = &have.features;
    b->independentBlend = h->independentBlend;
    b->dualSrcBlend = h->dualSrcBlend;
    b->logicOp = h->logicOp;
    b->fillModeNonSolid = h->fillModeNonSolid;
    b->samplerAnisotropy = h->samplerAnisotropy;
    b->textureCompressionBC = g_vk.bc = h->textureCompressionBC && !(getenv("MHGU_NO_BC") && atoi(getenv("MHGU_NO_BC")));
    b->textureCompressionASTC_LDR = h->textureCompressionASTC_LDR;
    b->shaderInt64 = h->shaderInt64;
    b->depthBiasClamp = h->depthBiasClamp;
    b->depthClamp = h->depthClamp;
    g_vk.depth_clamp = h->depthClamp;
    b->largePoints = h->largePoints;
    b->wideLines = h->wideLines;
    b->imageCubeArray = h->imageCubeArray;
    b->sampleRateShading = g_vk.sample_shading = h->sampleRateShading;
    b->fragmentStoresAndAtomics = h->fragmentStoresAndAtomics;
    b->vertexPipelineStoresAndAtomics = h->vertexPipelineStoresAndAtomics;
    b->robustBufferAccess = h->robustBufferAccess;  // guest shaders index cbufs freely: keep OOB reads harmless
    b->shaderClipDistance = h->shaderClipDistance;
    b->multiViewport = h->multiViewport;
    if (g_config.fsr && !g_vk.api11) {  // FSR: its shaders write images declared without a format, and have 16-bit float variants
        b->shaderStorageImageWriteWithoutFormat = g_vk.fsr_storage_wo_format = h->shaderStorageImageWriteWithoutFormat;
        b->shaderStorageImageReadWithoutFormat = h->shaderStorageImageReadWithoutFormat;
        f12.shaderFloat16 = b->shaderInt16 = g_vk.fsr_fp16 = f12_have.shaderFloat16 && h->shaderInt16;  // (both, or neither)
    }
    if (!h->shaderInt64) LOG("[vk] warning: no shaderInt64 (64-bit address math in shaders)\n");
#ifdef VK_EXT_depth_bias_control
    VkPhysicalDeviceDepthBiasControlFeaturesEXT bias = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_BIAS_CONTROL_FEATURES_EXT, .depthBiasControl = VK_TRUE,
        .leastRepresentableValueForceUnormRepresentation = VK_TRUE, .pNext = f.pNext};
    if (bias_ctl) f.pNext = &bias;
#endif
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = g_vk.qfam,
                                  .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f, .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qi, .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext};
    VK_CHECK(vkCreateDevice(g_vk.phys, &di, NULL, &g_vk.dev));
#ifdef VK_EXT_depth_bias_control
    if (bias_ctl) g_vk.set_depth_bias2 = (PFN_vkCmdSetDepthBias2EXT)vkGetDeviceProcAddr(g_vk.dev, "vkCmdSetDepthBias2EXT");
#endif
    if (hdr_meta) set_hdr_metadata = (PFN_vkSetHdrMetadataEXT)vkGetDeviceProcAddr(g_vk.dev, "vkSetHdrMetadataEXT");
    LOG("[vk] polygon offsets on the D32S8 made for D24S8: %s\n",
        vk_depth24_stencil8() != VK_FORMAT_D32_SFLOAT_S8_UINT ? "not needed (D24S8)"
        : g_vk.set_depth_bias2                               ? "in D24 steps (VK_EXT_depth_bias_control)"
                                                             : "as given (no VK_EXT_depth_bias_control)");
    if (!g_vk.fences) {
        g_vk.get_semaphore_counter = (PFN_vkGetSemaphoreCounterValue)vkGetDeviceProcAddr(g_vk.dev, "vkGetSemaphoreCounterValue");
        g_vk.wait_semaphores = (PFN_vkWaitSemaphores)vkGetDeviceProcAddr(g_vk.dev, "vkWaitSemaphores");
        g_vk.fences = !g_vk.get_semaphore_counter || !g_vk.wait_semaphores;
    }
    if (g_vk.api11 || g_vk.fences)
        LOG("[vk] Vulkan 1.1 path: api11 %d, fences %d, no mirror clamp %d\n", g_vk.api11, g_vk.fences, g_vk.no_mirror_clamp);
    vkGetDeviceQueue(g_vk.dev, g_vk.qfam, 0, &g_vk.queue);
    LOG("[vk] features: divisor %d, custom border %d, BC %d, ASTC %d, exclusive full screen off %d\n", g_vk.divisor,
        g_vk.custom_border, g_vk.bc, h->textureCompressionASTC_LDR, no_exclusive);

    // MSAA (settings): as many samples as asked for that colour and depth attachments both support and shaders can
    // read from depth (its resolve is a shader: exec.c vk_ms_resolve)
    const VkPhysicalDeviceLimits *lim = &g_vk.props.limits;
    VkSampleCountFlags counts = lim->framebufferColorSampleCounts & lim->framebufferDepthSampleCounts &
                                lim->framebufferStencilSampleCounts & lim->sampledImageDepthSampleCounts;
    if (g_vk.stencil_export) counts &= lim->sampledImageStencilSampleCounts;
    g_vk.msaa = 1;
    g_vk.msaa_counts = counts & (VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT | VK_SAMPLE_COUNT_8_BIT);
    for (uint32_t s = (uint32_t)g_config.msaa; s > 1 && g_vk.msaa == 1; s >>= 1)
        if (g_vk.msaa_counts & s) g_vk.msaa = s;
    if (g_vk.msaa != (uint32_t)g_config.msaa) LOG("[vk] MSAA %dx not supported: %ux\n", g_config.msaa, g_vk.msaa);

    VmaAllocatorCreateInfo ai = {.physicalDevice = g_vk.phys, .device = g_vk.dev, .instance = g_vk.inst,
                                 .vulkanApiVersion = g_vk.api11 ? VK_API_VERSION_1_1 : VK_API_VERSION_1_2};
    VK_CHECK(vmaCreateAllocator(&ai, &g_vk.vma));

    VkSemaphoreTypeCreateInfo st = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                    .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0};
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &st};
    if (!g_vk.fences) VK_CHECK(vkCreateSemaphore(g_vk.dev, &si, NULL, &g_vk.timeline));
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = g_vk.qfam};
    VK_CHECK(vkCreateCommandPool(g_vk.dev, &pi, NULL, &g_vk.pool));

    // pipeline cache: cache/vk_pipelines.bin (written at exit)
    size_t sz = 0;
    void *data = NULL;
    FILE *fp = fopen("cache/vk_pipelines.bin", "rb");
    if (fp) {
        fseek(fp, 0, SEEK_END);
        sz = (size_t)ftell(fp);
        fseek(fp, 0, SEEK_SET);
        data = malloc(sz);
        if (fread(data, 1, sz, fp) != sz) sz = 0;
        fclose(fp);
    }
    VkPipelineCacheCreateInfo pc = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = sz, .pInitialData = data};
    if (vkCreatePipelineCache(g_vk.dev, &pc, NULL, &g_vk.pcache) != VK_SUCCESS) {
        pc.initialDataSize = 0;
        VK_CHECK(vkCreatePipelineCache(g_vk.dev, &pc, NULL, &g_vk.pcache));
    }
    free(data);
}

static void pipeline_cache_save(void) {
    size_t sz = 0;
    if (vkGetPipelineCacheData(g_vk.dev, g_vk.pcache, &sz, NULL) != VK_SUCCESS || !sz) return;
    void *data = malloc(sz);
    if (vkGetPipelineCacheData(g_vk.dev, g_vk.pcache, &sz, data) == VK_SUCCESS) {
        mkdir("cache", 0755);
        FILE *f = fopen("cache/vk_pipelines.bin.tmp", "wb");
        if (f) {
            bool ok = fwrite(data, 1, sz, f) == sz;
            fclose(f);
            if (ok) { remove("cache/vk_pipelines.bin"); rename("cache/vk_pipelines.bin.tmp", "cache/vk_pipelines.bin"); }
        }
    }
    free(data);
}

// F7 (host_input.c): MSAA off -> 2x -> 4x -> 8x (those the GPU has) -> off, from the next pass on.
void vk_cycle_msaa(void) {
    if (g_fsr_on) { LOG("[vk] MSAA stays off while FSR is on\n"); return; }
    uint32_t s = g_vk.msaa;
    do s = s >= 8 ? 1 : s * 2; while (s > 1 && !(g_vk.msaa_counts & s));
    g_vk.msaa = s;
    LOG("[vk] MSAA %ux\n", s);
}

// MHGU_CONTROL "msaa <n>": as many samples as the GPU allows up to n (test runs).
void vk_set_msaa(uint32_t n) {
    if (g_fsr_on) { LOG("[vk] MSAA stays off while FSR is on\n"); return; }
    uint32_t s = 1;
    for (uint32_t c = 2; c <= 8; c *= 2) if (c <= n && (g_vk.msaa_counts & c)) s = c;
    g_vk.msaa = s;
    LOG("[vk] MSAA %ux\n", s);
}

// ================================================================== swapchain
static int swapchain_vsync = -1;
static int configured_msaa = -1;
static VkPresentModeKHR present_mode(void) {
    swapchain_vsync = config_live_vsync();
    // MAILBOX by default: no tearing, and the game paces itself (its own 30 fps vsync waits); with FIFO the game's frames
    // also queued behind the display's, dropping to ~26 fps on the R9. FIFO where there is no mailbox.
    const char *e = getenv("MHGU_VK_PRESENT"), *v = getenv("MHGU_VSYNC");
    if (!e && v) e = atoi(v) ? "fifo" : "immediate";
    if (!e) e = swapchain_vsync ? "mailbox" : "immediate";  // (mhgurecomp.ini VSync)
    VkPresentModeKHR want = !strcmp(e, "fifo") ? VK_PRESENT_MODE_FIFO_KHR
                          : !strcmp(e, "immediate") ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_MAILBOX_KHR;
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &n, NULL);
    VkPresentModeKHR m[16];
    if (n > 16) n = 16;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &n, m);
    for (uint32_t i = 0; i < n; i++) if (m[i] == want) return want;
    LOG("[vk] present mode %s not available: FIFO (vsync)\n", e);
    return VK_PRESENT_MODE_FIFO_KHR;
}

static VkRenderPass sc_pass;
static VkFramebuffer sc_fb[SC_MAX];

// HDR ([HDR] HDR = On, hdr.c): the game and the overlay draw into the composition image (sc_pass, RGBA16F, ends in
// GENERAL) instead of the window image; hdr_pass then draws it into the HDR10 / scRGB window image (hdr.c
// vk_hdr_encode). Everything that draws "into the window" (the present, the overlay, the loading screen) is unchanged.
#define COMPOSE_FORMAT VK_FORMAT_R16G16B16A16_SFLOAT
static int swapchain_hdr = -1;  // the setting the swapchain was made for: 0 off, else HDRFormat + 1
static TexBackend compose;
static VkImageView compose_view;  // (its own: a cached view can be evicted while the framebuffer still uses it)
static VkFramebuffer compose_fb;
static VkRenderPass hdr_pass;
static VkFramebuffer hdr_fb[SC_MAX];

static bool hdr_broken;  // the composition image could not be made: SDR
static int hdr_wanted(void) { return config_live_hdr() && !hdr_broken ? config_live_hdr_format() + 1 : 0; }

static void compose_free(void) {
    if (compose_fb) vkDestroyFramebuffer(g_vk.dev, compose_fb, NULL);
    if (compose_view) vkDestroyImageView(g_vk.dev, compose_view, NULL);
    compose_fb = VK_NULL_HANDLE;
    compose_view = VK_NULL_HANDLE;
    if (compose.image) {
        vk_views_forget(&compose);
        vk_defer_image(compose.image, compose.mem);
    }
    memset(&compose, 0, sizeof compose);
}

static bool compose_make(VkExtent2D ext) {
    if (compose.image && compose.width == ext.width && compose.height == ext.height && compose_fb) return true;
    compose_free();
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = COMPOSE_FORMAT,
                            .extent = {ext.width, ext.height, 1}, .mipLevels = 1, .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(g_vk.vma, &ci, &ai, &compose.image, &compose.mem, NULL) != VK_SUCCESS) {
        memset(&compose, 0, sizeof compose);
        return false;
    }
    compose.format = COMPOSE_FORMAT, compose.base_type = VK_IMAGE_VIEW_TYPE_2D;
    compose.width = ext.width, compose.height = ext.height;
    compose.depth = compose.layers = compose.levels = compose.samples = 1;
    compose.aspect = VK_IMAGE_ASPECT_COLOR_BIT, compose.gpu_owned = true;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = compose.image,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = COMPOSE_FORMAT,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(g_vk.dev, &vi, NULL, &compose_view) != VK_SUCCESS) compose_view = VK_NULL_HANDLE;
    VkFramebufferCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = sc_pass, .attachmentCount = 1,
                                  .pAttachments = &compose_view, .width = ext.width, .height = ext.height, .layers = 1};
    if (!compose_view || vkCreateFramebuffer(g_vk.dev, &fi, NULL, &compose_fb) != VK_SUCCESS) {
        compose_fb = VK_NULL_HANDLE;
        compose_free();
        return false;
    }
    return true;
}

// The window format for HDR: HDR10 (A2B10G10R10 / A2R10G10B10 + ST.2084) or scRGB (RGBA16F + extended sRGB linear),
// as HDRFormat asks and the surface offers. -1: none (SDR).
static int hdr_surface_format(const VkSurfaceFormatKHR *fmts, uint32_t nf, int want, int *out) {
    int pq = -1, lin = -1;
    for (uint32_t i = 0; i < nf; i++) {
        if (fmts[i].colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT &&
            (fmts[i].format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || fmts[i].format == VK_FORMAT_A2R10G10B10_UNORM_PACK32) && pq < 0)
            pq = (int)i;
        if (fmts[i].colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT && fmts[i].format == VK_FORMAT_R16G16B16A16_SFLOAT && lin < 0)
            lin = (int)i;
    }
    int fmt = want - 1;  // HDR_FORMAT_*
    if (fmt == HDR_FORMAT_SCRGB && lin >= 0) return *out = HDR_OUT_SCRGB, lin;
    if (pq >= 0) return *out = HDR_OUT_PQ, pq;
    if (lin >= 0) return *out = HDR_OUT_SCRGB, lin;
    return *out = HDR_OUT_NONE, -1;
}

static void hdr_send_metadata(bool force) {
    static float sent_peak = -1, sent_avg = -1;
    if (!set_hdr_metadata || !g_hdr_out || !g_vk.swapchain) return;
    float peak, avg;
    vk_hdr_metadata_values(&peak, &avg);
    if (!force && peak == sent_peak && avg == sent_avg) return;
    VkHdrMetadataEXT md = {.sType = VK_STRUCTURE_TYPE_HDR_METADATA_EXT,
                           .displayPrimaryRed = {0.708f, 0.292f}, .displayPrimaryGreen = {0.170f, 0.797f},
                           .displayPrimaryBlue = {0.131f, 0.046f}, .whitePoint = {0.3127f, 0.3290f},
                           .maxLuminance = peak, .minLuminance = 0.0f, .maxContentLightLevel = peak,
                           .maxFrameAverageLightLevel = avg};
    set_hdr_metadata(g_vk.dev, 1, &g_vk.swapchain, &md);
    sent_peak = peak, sent_avg = avg;
}

// Android takes the app's window away when it leaves the screen (minimised, a file picker over it) and makes a new one
// when it comes back. A surface dies with its window: everything on it says VK_ERROR_SURFACE_LOST_KHR from then on, so
// frame_begin makes a new surface (surface_ready). The window can also go while a swapchain is being made: not fatal.
#ifdef __ANDROID__
static bool surface_lost;
#define SURFACE_CHECK(x) do { VkResult sr_ = (x); \
        if (sr_ == VK_ERROR_SURFACE_LOST_KHR || sr_ == VK_ERROR_NATIVE_WINDOW_IN_USE_KHR) { surface_lost = true; return; } \
        VK_CHECK(sr_); } while (0)
#else
#define SURFACE_CHECK(x) VK_CHECK(x)
#endif

// The window's render passes, one per (format, load, final layout), kept for good: the overlay and the present
// pipeline are remade when the pass they were made for changes, which a destroyed pass's handle, reused, would hide.
// clear: cleared first (the game's picture is drawn into it), else every pixel is drawn (HDR: hdr.c).
static VkRenderPass window_pass(VkFormat fmt, bool clear, VkImageLayout final) {
    typedef struct { VkFormat fmt; bool clear; VkImageLayout final; VkRenderPass rp; } Made;
    static Made made[8];
    static int nmade;
    for (int i = 0; i < nmade; i++)
        if (made[i].fmt == fmt && made[i].clear == clear && made[i].final == final) return made[i].rp;
    VkAttachmentDescription a = {.format = fmt, .samples = VK_SAMPLE_COUNT_1_BIT,
                                 .loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                 .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                 .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                 .finalLayout = final};
    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ref};
    VkSubpassDependency dep = {.srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
                               .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                               .srcAccessMask = VK_ACCESS_ALL_WRITES,
                               .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT};
    VkRenderPassCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &a,
                                 .subpassCount = 1, .pSubpasses = &sp, .dependencyCount = 1, .pDependencies = &dep};
    VkRenderPass rp;
    VK_CHECK(vkCreateRenderPass(g_vk.dev, &ci, NULL, &rp));
    if (nmade < 8) made[nmade++] = (Made){fmt, clear, final, rp};
    return rp;
}

static void create_swapchain(void) {
    VkSurfaceCapabilitiesKHR caps;
    SURFACE_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_vk.phys, g_vk.surface, &caps));
    int w, h;
    SDL_GetWindowSizeInPixels(ui_window(), &w, &h);
    VkExtent2D ext = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : (VkExtent2D){(uint32_t)w, (uint32_t)h};
    if (!ext.width || !ext.height) return;  // minimised
    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &nf, NULL);
    VkSurfaceFormatKHR fmts[64];
    if (nf > 64) nf = 64;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &nf, fmts);
#ifdef __ANDROID__
    if (!nf) { surface_lost = true; return; }
#endif
    // scanout shows the bytes the game wrote: a UNORM swapchain (sRGB game images are read through UNORM views)
    VkSurfaceFormatKHR sf = fmts[0];
    for (uint32_t i = 0; i < nf; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { sf = fmts[i]; break; }
    // HDR: an HDR10 / scRGB window image where the surface offers one (hdr.c draws the composition into it)
    int want_hdr = hdr_wanted(), out = HDR_OUT_NONE;
    if (want_hdr) {
        int i = hdr_surface_format(fmts, nf, want_hdr, &out);
        if (i >= 0) sf = fmts[i];
        if (want_hdr != swapchain_hdr) {
            if (out) {
                LOG("[hdr] on: %s window image (format %d, colour space %d)\n", out == HDR_OUT_PQ ? "HDR10 (PQ)" : "scRGB",
                    sf.format, sf.colorSpace);
            } else {
                LOG("[hdr] the display offers no HDR format (%s): SDR\n",
                    colorspace_ext ? "is HDR on in the system's display settings?" : "no VK_EXT_swapchain_colorspace");
                overlay_notice("HDR", "This display offers no HDR output. Turn HDR on in the system's display settings, "
                                      "then turn HDR off and on again here.");
            }
        }
    } else if (swapchain_hdr > 0) {
        LOG("[hdr] off\n");
    }
    swapchain_hdr = want_hdr;
    uint32_t count = caps.minImageCount + 1 < 3 ? 3 : caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    if (count > SC_MAX) count = SC_MAX;
    // The window image is drawn upright and the system turns it to the screen (a phone's screen is portrait at
    // heart: in landscape currentTransform is a quarter turn, which would show the upright image squashed).
    VkSurfaceTransformFlagBitsKHR xf = caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                                           ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
#ifdef __ANDROID__
    // (the image then has the window's shape: some drivers give currentExtent in the screen's own portrait shape)
    if (xf == VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR && (w > h) != (ext.width > ext.height))
        ext = (VkExtent2D){ext.height, ext.width};
    LOG("[vk] window %dx%d, screen turned %#x\n", w, h, caps.currentTransform);
#endif
    VkSwapchainKHR old = g_vk.swapchain;
    VkSwapchainCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = g_vk.surface, .minImageCount = count,
        .imageFormat = sf.format, .imageColorSpace = sf.colorSpace, .imageExtent = ext, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = xf,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = present_mode(), .clipped = VK_TRUE,
        .oldSwapchain = old};
    FullScreenExclusiveInfo fse = {.sType = VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT,
                                   .fullScreenExclusive = FULL_SCREEN_EXCLUSIVE_DISALLOWED};
    if (no_exclusive) ci.pNext = &fse;
    SURFACE_CHECK(vkCreateSwapchainKHR(g_vk.dev, &ci, NULL, &g_vk.swapchain));
    for (uint32_t i = 0; i < g_vk.sc_count; i++) {
        vkDestroyFramebuffer(g_vk.dev, sc_fb[i], NULL);
        vkDestroyFramebuffer(g_vk.dev, hdr_fb[i], NULL);
        sc_fb[i] = hdr_fb[i] = VK_NULL_HANDLE;
        vkDestroyImageView(g_vk.dev, g_vk.sc_views[i], NULL);
        g_vk.sc_views[i] = VK_NULL_HANDLE;
    }
    g_vk.sc_count = 0;
    if (old) vkDestroySwapchainKHR(g_vk.dev, old, NULL);
    g_hdr_out = out;
    // (HDR: the game draws into the composition image, which stays in GENERAL to be sampled)
    VkFormat wfmt = out ? COMPOSE_FORMAT : sf.format;
    VkRenderPass pass = window_pass(wfmt, true, out ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    if (pass != sc_pass || !out) compose_free();  // (its framebuffer was made for the old pass)
    sc_pass = pass;
    if (out) hdr_pass = window_pass(sf.format, false, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    g_vk.sc_format = sf.format;
    g_vk.sc_extent = ext;
    if (out && !compose_make(ext)) {  // (no memory for it: SDR from now on, the swapchain made again)
        LOG("[hdr] could not create the %ux%u composition image: SDR\n", ext.width, ext.height);
        hdr_broken = true;
        create_swapchain();
        return;
    }
    // (all of them: a capped list leaves images the swapchain can still hand out, with no framebuffer)
    uint32_t n = 0;
    vkGetSwapchainImagesKHR(g_vk.dev, g_vk.swapchain, &n, NULL);
    if (n > SC_MAX) FATAL("the window's swapchain has %u images (asked for %u): at most %d are supported\n", n, count, SC_MAX);
    g_vk.sc_count = n;
    vkGetSwapchainImagesKHR(g_vk.dev, g_vk.swapchain, &g_vk.sc_count, g_vk.sc_images);
    for (uint32_t i = 0; i < g_vk.sc_count; i++) {
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g_vk.sc_images[i],
                                    .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = sf.format,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VK_CHECK(vkCreateImageView(g_vk.dev, &vi, NULL, &g_vk.sc_views[i]));
        VkFramebufferCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = g_hdr_out ? hdr_pass : sc_pass,
                                      .attachmentCount = 1, .pAttachments = &g_vk.sc_views[i], .width = ext.width,
                                      .height = ext.height, .layers = 1};
        VK_CHECK(vkCreateFramebuffer(g_vk.dev, &fi, NULL, g_hdr_out ? &hdr_fb[i] : &sc_fb[i]));
    }
    if (g_hdr_out) hdr_send_metadata(true);
    LOG("[vk] swapchain %ux%u, %u images (asked for %u), format %d%s\n", ext.width, ext.height, g_vk.sc_count, count, sf.format,
        g_hdr_out == HDR_OUT_PQ ? " (HDR10)" : g_hdr_out == HDR_OUT_SCRGB ? " (scRGB)" : "");
}

static void recreate_swapchain(void) {
    vk_wait_seq(vk_pending_seq() - 1);
    vkDeviceWaitIdle(g_vk.dev);
    create_swapchain();
}

VkRenderPass vk_window_pass(void) { return sc_pass; }

// ================================================================== frames
// Acquire semaphores rotate per frame in flight; a slot is reused once the submission that waited on it is done.
#define FRAMES 3
static VkSemaphore acquire_sem[FRAMES], done_sem[SC_MAX];
static uint64_t slot_seq[FRAMES];
static int slot;

static VkSemaphore binary_semaphore(void) {
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore s;
    VK_CHECK(vkCreateSemaphore(g_vk.dev, &si, NULL, &s));
    return s;
}

#ifdef __ANDROID__
static void *surface_window;  // the window g_vk.surface was made for

static void *android_window(void) {
    return SDL_GetPointerProperty(SDL_GetWindowProperties(ui_window()), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, NULL);
}

static void surface_destroy(void) {
    vk_wait_seq(vk_pending_seq() - 1);
    vkDeviceWaitIdle(g_vk.dev);
    for (uint32_t i = 0; i < g_vk.sc_count; i++) {
        vkDestroyFramebuffer(g_vk.dev, sc_fb[i], NULL);
        vkDestroyFramebuffer(g_vk.dev, hdr_fb[i], NULL);
        sc_fb[i] = hdr_fb[i] = VK_NULL_HANDLE;
        vkDestroyImageView(g_vk.dev, g_vk.sc_views[i], NULL);
        g_vk.sc_views[i] = VK_NULL_HANDLE;
    }
    g_vk.sc_count = 0;
    for (int i = 0; i < SC_MAX; i++) {  // (a present that failed may have left these signalled)
        if (done_sem[i]) vkDestroySemaphore(g_vk.dev, done_sem[i], NULL);
        done_sem[i] = VK_NULL_HANDLE;
    }
    if (g_vk.swapchain) vkDestroySwapchainKHR(g_vk.dev, g_vk.swapchain, NULL);
    g_vk.swapchain = VK_NULL_HANDLE;
    SDL_Vulkan_DestroySurface(g_vk.inst, g_vk.surface, NULL);
    g_vk.surface = VK_NULL_HANDLE;
    surface_window = NULL;
}

// Is there a surface to draw on? Lets go of a dead one (a window can only have one surface, and the memory is better
// given back while the app is away) and makes one on the window Android has now.
static bool surface_ready(void) {
    void *win = android_window();
    if (win && win == surface_window && !surface_lost) return true;
    if (g_vk.swapchain) LOG("[vk] the window went away: surface closed\n");
    if (g_vk.surface) surface_destroy();
    surface_lost = false;
    if (!win) return false;
    if (!SDL_Vulkan_CreateSurface(ui_window(), g_vk.inst, NULL, &g_vk.surface)) {
        LOG("[vk] no new surface yet: %s\n", SDL_GetError());
        g_vk.surface = VK_NULL_HANDLE;
        return false;
    }
    surface_window = win;
    create_swapchain();
    return !surface_lost;
}
#endif

typedef struct { uint32_t image; VkSemaphore acquired; bool ok; } Frame;
static Frame cur_frame;

// Acquire the next window image and begin its render pass (cleared to black). false: nothing to draw into.
static bool frame_begin(void) {
    cur_frame.ok = false;
    int w, h;
    SDL_GetWindowSizeInPixels(ui_window(), &w, &h);
    if (!w || !h || ui_window_away()) return false;  // minimised (or the window going away: ui.c)
#ifdef __ANDROID__
    if (!surface_ready()) return false;
#endif
    // All GPU mutations happen here under vk_lock(), after resolving the old
    // scene. Existing attachment caches defer old image destruction safely.
    int msaa = config_live_msaa();
    if (configured_msaa < 0) configured_msaa = g_config.msaa;
    if (msaa != configured_msaa) { vk_set_msaa((uint32_t)msaa); configured_msaa = msaa; }
    if (!g_vk.swapchain || (uint32_t)w != g_vk.sc_extent.width || (uint32_t)h != g_vk.sc_extent.height ||
        config_live_vsync() != swapchain_vsync || hdr_wanted() != swapchain_hdr) recreate_swapchain();
    if (!g_vk.swapchain) return false;
    slot = (slot + 1) % FRAMES;
    if (!acquire_sem[slot]) acquire_sem[slot] = binary_semaphore();
    vk_wait_seq(slot_seq[slot]);
    VkResult r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, UINT64_MAX, acquire_sem[slot], VK_NULL_HANDLE, &cur_frame.image);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        recreate_swapchain();
        r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, UINT64_MAX, acquire_sem[slot], VK_NULL_HANDLE, &cur_frame.image);
    }
#ifdef __ANDROID__
    if (r == VK_ERROR_SURFACE_LOST_KHR) { surface_lost = true; return false; }
#endif
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { LOG("[vk] acquire failed: %d\n", r); return false; }
    cur_frame.acquired = acquire_sem[slot];
    slot_seq[slot] = vk_pending_seq();
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);  // (the game's image is complete before it is sampled)
    VkClearValue black = {.color.float32 = {0, 0, 0, 1}};
    VkRenderPassBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = sc_pass,
                                .framebuffer = g_hdr_out ? compose_fb : sc_fb[cur_frame.image], .renderArea = {{0, 0}, g_vk.sc_extent},
                                .clearValueCount = 1, .pClearValues = &black};
    vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp = {0, 0, (float)g_vk.sc_extent.width, (float)g_vk.sc_extent.height, 0, 1};
    VkRect2D sc = {{0, 0}, g_vk.sc_extent};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    cur_frame.ok = true;
    return true;
}

// A screenshot being written: the window image's pixels (B8G8R8A8 or R8G8B8A8), made RGBA and saved as PNG.
typedef struct { char path[300]; uint8_t *px; uint32_t w, h; bool bgr; uint16_t *half; } ShotJob;
static float half_to_float(uint16_t h) {
    uint32_t e = (h >> 10) & 31, m = h & 1023;
    float v = e == 0 ? ldexpf((float)m, -24) : e == 31 ? 65504.0f : ldexpf((float)(m | 1024), (int)e - 25);
    return h & 0x8000 ? -v : v;
}
static void *shot_thread(void *arg) {
    ShotJob *j = arg;
    if (j->half && j->px) {  // (HDR: the composition image, RGBA16F holding the SDR picture)
        uint64_t n = (uint64_t)j->w * j->h * 4;
        for (uint64_t i = 0; i < n; i++) {
            float v = half_to_float(j->half[i]);
            j->px[i] = (uint8_t)(v <= 0.0f ? 0 : v >= 1.0f ? 255 : lrintf(v * 255.0f));
        }
    }
    free(j->half);
    if (j->px) {
        uint64_t n = (uint64_t)j->w * j->h;
        for (uint64_t i = 0; j->bgr && i < n; i++) { uint8_t t = j->px[i * 4]; j->px[i * 4] = j->px[i * 4 + 2]; j->px[i * 4 + 2] = t; }
        for (uint64_t i = 0; i < n; i++) j->px[i * 4 + 3] = 255;
        vk_save_png(j->path, j->px, j->w, j->h);
    } else {
        LOG("[vk] saving %s failed: out of memory\n", j->path);
    }
    free(j->px);
    free(j);
    return NULL;
}

// End the window image's render pass and present it. shot_path: also save the window image as PNG.
static void frame_end(const char *shot_path) {
    if (!cur_frame.ok) return;
    VkCommandBuffer cb = vk_cmd();
    vkCmdEndRenderPass(cb);
    VkSpan shot = {0};
    uint32_t img = cur_frame.image;
    bool hdr = g_hdr_out != HDR_OUT_NONE;
    if (shot_path && hdr) {  // (HDR) the composition image (GENERAL): the SDR picture with the overlay
        uint32_t w = compose.width, h = compose.height;
        shot = vk_readback((uint64_t)w * h * 8);
        VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                              .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        VkBufferImageCopy rg = {.bufferOffset = shot.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
        vkCmdCopyImageToBuffer(cb, compose.image, VK_IMAGE_LAYOUT_GENERAL, shot.buf, 1, &rg);
    }
    if (hdr) {  // (HDR) the composition -> the window image (hdr.c)
        vk_full_barrier(cb);
        VkRenderPassBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = hdr_pass,
                                    .framebuffer = hdr_fb[img], .renderArea = {{0, 0}, g_vk.sc_extent}};
        vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
        vk_hdr_encode(cb, hdr_pass, &compose);
        vkCmdEndRenderPass(cb);
        hdr_send_metadata(false);
    }
    if (shot_path && !hdr) {  // PRESENT_SRC -> TRANSFER_SRC, copy, back
        uint32_t w = g_vk.sc_extent.width, h = g_vk.sc_extent.height;
        shot = vk_readback((uint64_t)w * h * 4);
        VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                  .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT, .oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                  .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = g_vk.sc_images[img],
                                  .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        VkBufferImageCopy rg = {.bufferOffset = shot.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
        vkCmdCopyImageToBuffer(cb, g_vk.sc_images[img], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, shot.buf, 1, &rg);
        b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.dstAccessMask = 0;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    }
    if (!done_sem[img]) done_sem[img] = binary_semaphore();
    uint64_t seq = vk_pending_seq();
    vk_submit_extra(cur_frame.acquired, done_sem[img]);
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &done_sem[img],
                           .swapchainCount = 1, .pSwapchains = &g_vk.swapchain, .pImageIndices = &img};
    VkResult r = vkQueuePresentKHR(g_vk.queue, &pi);
    // (Android says SUBOPTIMAL whenever the system turns the image: a new swapchain would be no better. A new
    // window size is caught in frame_begin.)
#ifdef __ANDROID__
    if (r == VK_ERROR_OUT_OF_DATE_KHR) recreate_swapchain();
    if (r == VK_ERROR_SURFACE_LOST_KHR) surface_lost = true;
#else
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) recreate_swapchain();
#endif
    cur_frame.ok = false;
    if (shot_path) {  // (the copy is taken here; the PNG is made on a thread of its own: encoding the window's size
                      // stalled the game ~0.8 s on a Retina Mac)
        vk_wait_seq(seq);
        ShotJob *j = malloc(sizeof *j);
        if (!j) { LOG("[vk] saving %s failed: out of memory\n", shot_path); return; }
        j->w = hdr ? compose.width : g_vk.sc_extent.width, j->h = hdr ? compose.height : g_vk.sc_extent.height;
        j->bgr = !hdr && (g_vk.sc_format == VK_FORMAT_B8G8R8A8_UNORM || g_vk.sc_format == VK_FORMAT_B8G8R8A8_SRGB);
        j->px = malloc((size_t)j->w * j->h * 4);
        j->half = hdr ? malloc((size_t)j->w * j->h * 8) : NULL;
        snprintf(j->path, sizeof j->path, "%s", shot_path);
        pthread_t th;
        if (hdr && !j->half) { free(j->px); j->px = NULL; }
        if (j->half) memcpy(j->half, shot.ptr, (size_t)j->w * j->h * 8);
        else if (j->px) memcpy(j->px, shot.ptr, (size_t)j->w * j->h * 4);
        if (!j->px || pthread_create(&th, NULL, shot_thread, j) != 0) shot_thread(j);
        else pthread_detach(th);
    }
}

void vk_save_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h) {
    SDL_Surface *s = SDL_CreateSurfaceFrom((int)w, (int)h, SDL_PIXELFORMAT_ABGR8888, (void *)rgba, (int)w * 4);
#ifdef __ANDROID__
    char abs[1400], cwd[1024];  // (Android's SDL opens a relative path inside the APK's assets: from the app's folder)
    if (path[0] != '/' && getcwd(cwd, sizeof cwd)) snprintf(abs, sizeof abs, "%s/%s", cwd, path), path = abs;
#endif
    if (!s || !SDL_SavePNG(s, path)) LOG("[vk] saving %s failed: %s\n", path, SDL_GetError());
    else LOG("[vk] saved %s (%ux%u)\n", path, w, h);
    SDL_DestroySurface(s);
}

// ================================================================== presentation (nvnQueuePresentTexture)
void vk_present_draw(VkCommandBuffer cb, TexBackend *b, float vx, float vy, float vw, float vh);  // (exec.c)

static uint64_t g_frames;
uint64_t vk_frame_counter(void) { return g_frames; }

// The MHGU_SHOT_AT list: is this frame one of them?
static bool shot_frame(uint64_t f) {
    static const char *list = (const char *)1;
    if (list == (const char *)1) list = getenv("MHGU_SHOT_AT");
    for (const char *p = list; p && *p;) {
        if ((uint64_t)strtoull(p, (char **)&p, 10) == f) return true;
        if (*p == ',') p++; else break;
    }
    return false;
}

void backend_present(NvnTexture *t) {
    extern void vi_apply_framerate(void);
    vi_apply_framerate();
    vk_lock();
    if (vk_hdr_tracking()) vk_hdr_present(t);  // (HDR: learns the frame's full-screen image, hdr.c)
    if (g_fsr_on) t = vk_fsr_present(t);  // FSR: its output-size picture with the HUD (fsr.c)
    TexBackend *b = vk_tex(t, true);
    b->presented = true;
    vk_end_pass();
    vk_ms_resolve(b);
    if (frame_begin()) {
        // the game's image, as large as fits with its aspect ratio, centred
        double ww = g_vk.sc_extent.width, wh = g_vk.sc_extent.height, s = fmin(ww / t->width, wh / t->height);
        double vw = t->width * s, vh = t->height * s;
        vk_present_draw(vk_cmd(), b, (float)((ww - vw) / 2), (float)((wh - vh) / 2), (float)vw, (float)vh);
        OverlayTarget ot = {g_vk.inst, g_vk.phys, g_vk.dev, g_vk.qfam, g_vk.queue, g_vk.pcache, sc_pass,
                            g_vk.sc_extent.width, g_vk.sc_extent.height};
        overlay_draw(vk_cmd(), &ot);  // (Forge plugins' ImGui overlay, over the game: runtime/overlay.cpp)
        char path[300];
        extern char g_shot_request[256];  // (runtime/host_input.c: MHGU_CONTROL "shot <name>")
        extern char g_burst_name[200];
        extern int g_burst_left, g_burst_index;
        if (!g_shot_request[0] && g_burst_left > 0) {
            g_burst_left--;
            snprintf(g_shot_request, sizeof g_shot_request, "shots/%s_%d.png", g_burst_name, g_burst_index++);
        }
        bool shot = shot_frame(g_frames + 1) || g_shot_request[0];
        if (shot) {
            mkdir("shots", 0755);
            if (g_shot_request[0]) { snprintf(path, sizeof path, "%s", g_shot_request); g_shot_request[0] = 0; }
            else snprintf(path, sizeof path, "shots/frame_%05llu.png", (unsigned long long)(g_frames + 1));
        }
        frame_end(shot ? path : NULL);
    } else {
        vk_submit_extra(VK_NULL_HANDLE, VK_NULL_HANDLE);
    }
    g_frames++;
    g_present_count = g_frames;
    {   // (debug) MHGU_CONTROL "dump": the next whole frame's passes (exec.c dump_pass)
        extern volatile int g_dump_active, g_dump_request;
        extern int g_dump_no, g_dump_frame;
        static int left;
        if (g_dump_active) {
            LOG("[dump] frame %d done\n", g_dump_frame);
            if (--left > 0) { g_dump_frame++; g_dump_no = 0; }
            else g_dump_active = 0;
        } else if (g_dump_request) { left = g_dump_request; g_dump_request = 0; g_dump_no = 0; g_dump_frame = 0; g_dump_active = 1; }
    }
    {   // (debug) MHGU_CONTROL "hudtrace": the next whole frame's HUD quads (exec.c cbuf_info)
        extern volatile int g_hudtrace_active, g_hudtrace_request;
        if (g_hudtrace_active) { LOG("[hud] frame done\n"); g_hudtrace_active--; }  // (2: the frame recorded next may run late)
        else if (g_hudtrace_request) { g_hudtrace_request = 0; g_hudtrace_active = 2; }
    }
    { void vk_gpuprof_frame(void); vk_gpuprof_frame(); }  // (debug) MHGU_CONTROL "gpuprof"
    vk_unlock();
    static int fps = -1;
    if (fps < 0) fps = getenv("MHGU_FPS") != NULL;
    static uint64_t t0, n0;
    uint64_t now = plat_now_ns();
    if (!t0) t0 = now;
    if (now - t0 >= 2000000000ull) {
        if (fps) LOG("[vk] %.1f fps (frame %llu)\n", (g_frames - n0) * 1e9 / (double)(now - t0), (unsigned long long)g_frames);
        t0 = now, n0 = g_frames;
    }
}

// ================================================================== init
void vk_exec_init(void);
void vk_shaders_init(void);
static SDL_AtomicInt vk_up;  // backend_init has finished (the loading screen waits for it)

// exit() from any thread. The atexit chain is the danger: SDL_Quit and libnvidia's own destructors tear the
// driver down while other threads are still inside nvnQueueSubmitCommands / vkWaitSemaphores (the exit-time
// SIGSEGV in libnvidia-glcore). So: park the guest threads (vk_lock), drain what is in flight, let the GPU
// finish, keep the pipeline cache - and leave with _exit, running no further teardown.
static void exit_handler(void) {
    exiting = true;
    vk_exit_thread = pthread_self();
    if (g_vk.dev && g_vk_owner != pthread_self()) {  // (inside a submission already: the holder is us, don't self-lock)
        pthread_mutex_lock(&g_vk_mutex);
        pthread_mutex_unlock(&g_vk_mutex);
    }
    if (g_vk.dev) {
        vkDeviceWaitIdle(g_vk.dev);
        pipeline_cache_save();
    }
    engine_modules_shutdown();  // (its own atexit handler was registered before this one, so it would never run)
    fflush(stdout);
    fflush(stderr);
    _exit(0);
}

void backend_init(void) {
    static bool done;
    if (done) return;
    done = true;
    if (!ui_window()) FATAL("no window\n");
    create_instance();
#ifdef __ANDROID__
    while (!android_window()) SDL_Delay(50);  // (minimised right at the start: no window until the app comes back)
#endif
    if (!SDL_Vulkan_CreateSurface(ui_window(), g_vk.inst, NULL, &g_vk.surface)) FATAL("SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
#ifdef __ANDROID__
    surface_window = android_window();
#endif
    pick_device();
    create_device();
    vk_exec_init();
    vk_shaders_init();
    vk_fsr_init();  // ([Upscaler] FSR: fsr.c)
    create_swapchain();
    atexit(exit_handler);
    SDL_SetAtomicInt(&vk_up, 1);
}

// ================================================================== loading screen
// From backend_init until the game presents its first frame (its own start-up, then the first programs of the shader
// prewarm: seconds of a black window, more on a first run or a slow PC) a thread of its own presents the window image
// with the overlay's loading indicator (runtime/overlay.cpp overlay_loading), about 30 times a second, between the
// game's submissions (under the Vulkan lock; backend_execute submits all it records). Its frames are not counted as
// the game's (g_frames: MHGU_SHOT_AT, the control file's frame counts). MHGU_NO_LOADING_SCREEN=1: none.
static void *loading_thread(void *arg) {
    (void)arg;
    while (!SDL_GetAtomicInt(&vk_up)) SDL_Delay(20);  // (Android: the game's thread starts the renderer)
    uint64_t t0 = plat_now_ns();
    int n = 0;
    for (;;) {
        vk_lock();
        bool game = g_frames != 0;
        if (!game && frame_begin()) {
            OverlayTarget ot = {g_vk.inst, g_vk.phys, g_vk.dev, g_vk.qfam, g_vk.queue, g_vk.pcache, sc_pass,
                                g_vk.sc_extent.width, g_vk.sc_extent.height};
            overlay_loading(vk_cmd(), &ot);
            frame_end(NULL);
            n++;
        }
        vk_unlock();
        if (game) break;
        SDL_Delay(33);
    }
    LOG("[vk] loading screen: %d frames, %.1f s until the game's first\n", n, (plat_now_ns() - t0) / 1e9);
    return NULL;
}

void backend_loading_screen(void) {
    if (!g_config.loading_indicator || getenv("MHGU_NO_LOADING_SCREEN")) return;  // ([Performance] LoadingIndicator)
    pthread_t t;
    if (pthread_create(&t, NULL, loading_thread, NULL) == 0) pthread_detach(t);
}
