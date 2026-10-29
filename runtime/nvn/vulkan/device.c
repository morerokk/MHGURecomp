// Vulkan instance, device, swapchain and presentation (on the game window from runtime/ui.c).
//   MHGU_VK_VALIDATION=1   Khronos validation layer (MSYS2 package vulkan-validation-layers)
//   MHGU_GPU=<index>       which Vulkan device (default: the first discrete GPU)
//   MHGU_VSYNC=0|1         0: no vsync (tearing possible, lowest latency), 1: classic vsync (FIFO). Default: the
//                          settings' VSync - On: mailbox (no tearing; the game paces itself), FIFO where there is
//                          none (the R9's driver), Off: immediate. MHGU_VK_PRESENT=fifo|mailbox|immediate picks the present mode directly.
//   MHGU_SHOT_AT=<frame>[,<frame>...]   save the window image at those presented frames as shots/frame_N.png
//   MHGU_FPS=1             frames per second in the log every 2 s
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../overlay.h"
#include "fsr.h"
#include "fsr4.h"
#include "vk.h"

VkG g_vk;
static VkDebugUtilsMessengerEXT messenger;

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
    for (Uint32 i = 0; i < n_sdl && n_ext < 14; i++) ext[n_ext++] = sdl_ext[i];
    bool validate = getenv("MHGU_VK_VALIDATION") != NULL;
    if (validate) ext[n_ext++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "MHGURecomp",
                             .pEngineName = "MHGURecomp",
                             .apiVersion = g_config.fsr && g_config.fsr_upscaler == FSR_UP_4 ? VK_API_VERSION_1_3 : VK_API_VERSION_1_2};  // (FSR 4's shaders use SPIR-V 1.6)
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
    if (r != VK_SUCCESS) FATAL("vkCreateInstance: %d (is a Vulkan 1.2 driver installed?)\n", r);
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

// Discrete GPU first (MHGU_GPU=<index> picks one); needs Vulkan 1.2 and a queue that can draw, compute and present.
static void pick_device(void) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g_vk.inst, &n, NULL);
    if (!n) FATAL("no Vulkan device\n");
    VkPhysicalDevice *pd = calloc(n, sizeof *pd);
    vkEnumeratePhysicalDevices(g_vk.inst, &n, pd);
    int want = getenv("MHGU_GPU") ? atoi(getenv("MHGU_GPU")) : -1, best = -1, best_score = -1;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd[i], &p);
        LOG("[vk] gpu %u: %s (api %u.%u.%u)\n", i, p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
            VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion));
        if (p.apiVersion < VK_API_VERSION_1_2) continue;
        int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if ((int)i == want) score = 100;
        if (score > best_score) { best_score = score; best = (int)i; }
    }
    if (best < 0) FATAL("no Vulkan 1.2 device\n");
    g_vk.phys = pd[best];
    free(pd);
    vkGetPhysicalDeviceProperties(g_vk.phys, &g_vk.props);
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
    bool fsr4 = g_config.fsr && g_config.fsr_upscaler == FSR_UP_4 && vk_fsr4_device_exts(ep, ne, ext, &n_ext);  // FSR 4: its extensions
    free(ep);

    // optional features: whatever the GPU has of what the backend can use
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT div_have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT cbc_have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT,
                                                             .pNext = g_vk.divisor ? &div_have : NULL};
    VkPhysicalDeviceVulkan12Features f12_have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                                 .pNext = g_vk.custom_border ? (void *)&cbc_have : g_vk.divisor ? (void *)&div_have : NULL};
    VkPhysicalDeviceVulkan11Features f11_have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &f12_have};
    VkPhysicalDeviceFeatures2 have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f11_have};
    vkGetPhysicalDeviceFeatures2(g_vk.phys, &have);
    if (!f12_have.timelineSemaphore) FATAL("the GPU driver has no timeline semaphores (Vulkan 1.2 required)\n");
    g_vk.divisor = g_vk.divisor && div_have.vertexAttributeInstanceRateDivisor;
    g_vk.custom_border = g_vk.custom_border && cbc_have.customBorderColors && cbc_have.customBorderColorWithoutFormat;

    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT div = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT, .vertexAttributeInstanceRateDivisor = VK_TRUE};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT cbc = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, .customBorderColors = VK_TRUE,
        .customBorderColorWithoutFormat = VK_TRUE, .pNext = g_vk.divisor ? &div : NULL};
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                            .timelineSemaphore = VK_TRUE, .shaderInt8 = f12_have.shaderInt8,
                                            .samplerMirrorClampToEdge = f12_have.samplerMirrorClampToEdge,
                                            .uniformAndStorageBuffer8BitAccess = f12_have.uniformAndStorageBuffer8BitAccess,
                                            .storageBuffer8BitAccess = f12_have.storageBuffer8BitAccess,
                                            .scalarBlockLayout = f12_have.scalarBlockLayout,
                                            .pNext = g_vk.custom_border ? (void *)&cbc : g_vk.divisor ? (void *)&div : NULL};
    VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
                                            .storageBuffer16BitAccess = f11_have.storageBuffer16BitAccess, .pNext = &f12};
    VkPhysicalDeviceFeatures2 f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f11};
    VkPhysicalDeviceFeatures *b = &f.features, *h = &have.features;
    b->independentBlend = h->independentBlend;
    b->dualSrcBlend = h->dualSrcBlend;
    b->logicOp = h->logicOp;
    b->fillModeNonSolid = h->fillModeNonSolid;
    b->samplerAnisotropy = h->samplerAnisotropy;
    b->textureCompressionBC = h->textureCompressionBC;
    b->textureCompressionASTC_LDR = h->textureCompressionASTC_LDR;
    b->shaderInt64 = h->shaderInt64;
    b->depthBiasClamp = h->depthBiasClamp;
    b->depthClamp = h->depthClamp;
    g_vk.depth_clamp = h->depthClamp;
    b->largePoints = h->largePoints;
    b->wideLines = h->wideLines;
    b->imageCubeArray = h->imageCubeArray;
    b->sampleRateShading = h->sampleRateShading;
    b->fragmentStoresAndAtomics = h->fragmentStoresAndAtomics;
    b->vertexPipelineStoresAndAtomics = h->vertexPipelineStoresAndAtomics;
    b->robustBufferAccess = h->robustBufferAccess;  // guest shaders index cbufs freely: keep OOB reads harmless
    b->shaderClipDistance = h->shaderClipDistance;
    b->multiViewport = h->multiViewport;
    if (g_config.fsr) {  // FSR: its shaders write images declared without a format, and have 16-bit float variants
        b->shaderStorageImageWriteWithoutFormat = g_vk.fsr_storage_wo_format = h->shaderStorageImageWriteWithoutFormat;
        b->shaderStorageImageReadWithoutFormat = h->shaderStorageImageReadWithoutFormat;
        f12.shaderFloat16 = b->shaderInt16 = g_vk.fsr_fp16 = f12_have.shaderFloat16 && h->shaderInt16;  // (both, or neither)
    }
    if (fsr4) vk_fsr4_device_features(&f11, &f12, b);  // FSR 4: cooperative matrix, float8... (only when asked for)
    if (!h->shaderInt64) LOG("[vk] warning: no shaderInt64 (64-bit address math in shaders)\n");
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = g_vk.qfam,
                                  .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f, .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qi, .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext};
    VK_CHECK(vkCreateDevice(g_vk.phys, &di, NULL, &g_vk.dev));
    vkGetDeviceQueue(g_vk.dev, g_vk.qfam, 0, &g_vk.queue);
    LOG("[vk] features: divisor %d, custom border %d, BC %d, ASTC %d\n", g_vk.divisor, g_vk.custom_border,
        h->textureCompressionBC, h->textureCompressionASTC_LDR);

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
                                 .vulkanApiVersion = VK_API_VERSION_1_2};
    VK_CHECK(vmaCreateAllocator(&ai, &g_vk.vma));

    VkSemaphoreTypeCreateInfo st = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                    .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0};
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &st};
    VK_CHECK(vkCreateSemaphore(g_vk.dev, &si, NULL, &g_vk.timeline));
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
static VkFramebuffer sc_fb[8];

static void create_swapchain(void) {
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_vk.phys, g_vk.surface, &caps));
    int w, h;
    SDL_GetWindowSizeInPixels(ui_window(), &w, &h);
    VkExtent2D ext = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : (VkExtent2D){(uint32_t)w, (uint32_t)h};
    if (!ext.width || !ext.height) return;  // minimised
    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &nf, NULL);
    VkSurfaceFormatKHR fmts[64];
    if (nf > 64) nf = 64;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &nf, fmts);
    // scanout shows the bytes the game wrote: a UNORM swapchain (sRGB game images are read through UNORM views)
    VkSurfaceFormatKHR sf = fmts[0];
    for (uint32_t i = 0; i < nf; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { sf = fmts[i]; break; }
    uint32_t count = caps.minImageCount + 1 < 3 ? 3 : caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    if (count > 8) count = 8;
    VkSwapchainKHR old = g_vk.swapchain;
    VkSwapchainCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = g_vk.surface, .minImageCount = count,
        .imageFormat = sf.format, .imageColorSpace = sf.colorSpace, .imageExtent = ext, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = present_mode(), .clipped = VK_TRUE,
        .oldSwapchain = old};
    VK_CHECK(vkCreateSwapchainKHR(g_vk.dev, &ci, NULL, &g_vk.swapchain));
    for (uint32_t i = 0; i < g_vk.sc_count; i++) {
        vkDestroyFramebuffer(g_vk.dev, sc_fb[i], NULL);
        vkDestroyImageView(g_vk.dev, g_vk.sc_views[i], NULL);
    }
    if (old) vkDestroySwapchainKHR(g_vk.dev, old, NULL);
    if (sc_pass && sf.format != g_vk.sc_format) { vkDestroyRenderPass(g_vk.dev, sc_pass, NULL); sc_pass = VK_NULL_HANDLE; }
    g_vk.sc_format = sf.format;
    g_vk.sc_extent = ext;
    if (!sc_pass) {  // the window image: cleared, then presented (copied from for screenshots first)
        VkAttachmentDescription a = {.format = sf.format, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                     .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                     .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                     .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};
        VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sp = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ref};
        VkSubpassDependency dep = {.srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
                                   .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                   .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                   .srcAccessMask = VK_ACCESS_ALL_WRITES,
                                   .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT};
        VkRenderPassCreateInfo rp = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &a,
                                     .subpassCount = 1, .pSubpasses = &sp, .dependencyCount = 1, .pDependencies = &dep};
        VK_CHECK(vkCreateRenderPass(g_vk.dev, &rp, NULL, &sc_pass));
    }
    g_vk.sc_count = 8;
    vkGetSwapchainImagesKHR(g_vk.dev, g_vk.swapchain, &g_vk.sc_count, g_vk.sc_images);
    for (uint32_t i = 0; i < g_vk.sc_count; i++) {
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g_vk.sc_images[i],
                                    .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = sf.format,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VK_CHECK(vkCreateImageView(g_vk.dev, &vi, NULL, &g_vk.sc_views[i]));
        VkFramebufferCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = sc_pass, .attachmentCount = 1,
                                      .pAttachments = &g_vk.sc_views[i], .width = ext.width, .height = ext.height, .layers = 1};
        VK_CHECK(vkCreateFramebuffer(g_vk.dev, &fi, NULL, &sc_fb[i]));
    }
    LOG("[vk] swapchain %ux%u, %u images, format %d\n", ext.width, ext.height, g_vk.sc_count, sf.format);
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
static VkSemaphore acquire_sem[FRAMES], done_sem[8];
static uint64_t slot_seq[FRAMES];
static int slot;

static VkSemaphore binary_semaphore(void) {
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore s;
    VK_CHECK(vkCreateSemaphore(g_vk.dev, &si, NULL, &s));
    return s;
}

typedef struct { uint32_t image; VkSemaphore acquired; bool ok; } Frame;
static Frame cur_frame;

// Acquire the next window image and begin its render pass (cleared to black). false: nothing to draw into.
static bool frame_begin(void) {
    cur_frame.ok = false;
    int w, h;
    SDL_GetWindowSizeInPixels(ui_window(), &w, &h);
    if (!w || !h) return false;  // minimised
    // All GPU mutations happen here under vk_lock(), after resolving the old
    // scene. Existing attachment caches defer old image destruction safely.
    int msaa = config_live_msaa();
    if (configured_msaa < 0) configured_msaa = g_config.msaa;
    if (msaa != configured_msaa) { vk_set_msaa((uint32_t)msaa); configured_msaa = msaa; }
    if (!g_vk.swapchain || (uint32_t)w != g_vk.sc_extent.width || (uint32_t)h != g_vk.sc_extent.height ||
        config_live_vsync() != swapchain_vsync) recreate_swapchain();
    if (!g_vk.swapchain) return false;
    slot = (slot + 1) % FRAMES;
    if (!acquire_sem[slot]) acquire_sem[slot] = binary_semaphore();
    vk_wait_seq(slot_seq[slot]);
    VkResult r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, UINT64_MAX, acquire_sem[slot], VK_NULL_HANDLE, &cur_frame.image);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        recreate_swapchain();
        r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, UINT64_MAX, acquire_sem[slot], VK_NULL_HANDLE, &cur_frame.image);
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { LOG("[vk] acquire failed: %d\n", r); return false; }
    cur_frame.acquired = acquire_sem[slot];
    slot_seq[slot] = vk_pending_seq();
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);  // (the game's image is complete before it is sampled)
    VkClearValue black = {.color.float32 = {0, 0, 0, 1}};
    VkRenderPassBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = sc_pass,
                                .framebuffer = sc_fb[cur_frame.image], .renderArea = {{0, 0}, g_vk.sc_extent},
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
typedef struct { char path[300]; uint8_t *px; uint32_t w, h; bool bgr; } ShotJob;
static void *shot_thread(void *arg) {
    ShotJob *j = arg;
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
    if (shot_path) {  // PRESENT_SRC -> TRANSFER_SRC, copy, back
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
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) recreate_swapchain();
    cur_frame.ok = false;
    if (shot_path) {  // (the copy is taken here; the PNG is made on a thread of its own: encoding the window's size
                      // stalled the game ~0.8 s on a Retina Mac)
        vk_wait_seq(seq);
        ShotJob *j = malloc(sizeof *j);
        if (!j) { LOG("[vk] saving %s failed: out of memory\n", shot_path); return; }
        j->w = g_vk.sc_extent.width, j->h = g_vk.sc_extent.height;
        j->bgr = g_vk.sc_format == VK_FORMAT_B8G8R8A8_UNORM || g_vk.sc_format == VK_FORMAT_B8G8R8A8_SRGB;
        j->px = malloc((size_t)j->w * j->h * 4);
        snprintf(j->path, sizeof j->path, "%s", shot_path);
        pthread_t th;
        if (j->px) memcpy(j->px, shot.ptr, (size_t)j->w * j->h * 4);
        if (!j->px || pthread_create(&th, NULL, shot_thread, j) != 0) shot_thread(j);
        else pthread_detach(th);
    }
}

void vk_save_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h) {
    SDL_Surface *s = SDL_CreateSurfaceFrom((int)w, (int)h, SDL_PIXELFORMAT_ABGR8888, (void *)rgba, (int)w * 4);
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
    t = vk_fsr_present(t);  // FSR: its output-size picture with the HUD; SMAA T2x starts / stops here (fsr.c)
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
    if (!SDL_Vulkan_CreateSurface(ui_window(), g_vk.inst, NULL, &g_vk.surface)) FATAL("SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
    pick_device();
    create_device();
    vk_exec_init();
    vk_shaders_init();
    vk_fsr_init();  // ([Upscaler] FSR: fsr.c)
    create_swapchain();
    atexit(exit_handler);
}
