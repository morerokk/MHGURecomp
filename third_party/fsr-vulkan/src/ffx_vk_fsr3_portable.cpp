/*
 * Copyright (c) 2026 Q2RTX FSR Vulkan contributors
 * SPDX-License-Identifier: MIT
 */

#include "ffx_vk_portable.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wmissing-braces"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#pragma GCC diagnostic ignored "-Wunused-variable"
#endif

#include <FidelityFX/host/backends/vk/ffx_vk.h>
/* vendoring patch: frame generation (FI/OF) is not vendored; define
 * FFX_VK_PORTABLE_HAS_FSR3_FRAMEGEN=1 with the full upstream tree to restore it. */
#if defined(FFX_VK_PORTABLE_HAS_FSR3_FRAMEGEN) && FFX_VK_PORTABLE_HAS_FSR3_FRAMEGEN
#include <FidelityFX/host/ffx_frameinterpolation.h>
#include <FidelityFX/host/ffx_opticalflow.h>
#endif
#include <FidelityFX/host/ffx_fsr3upscaler.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <limits>
#include <mutex>
#include <new>

namespace {

constexpr uint64_t kContextMagic = UINT64_C(0x46535233564b4354); /* FSR3VKCT */
constexpr uint64_t kFrameGenerationContextMagic = UINT64_C(0x4653523346474354); /* FSR3FGCT */
constexpr uint32_t kKnownContextFlags =
    FFX_VK_PORTABLE_CONTEXT_HDR_COLOR_INPUT |
    FFX_VK_PORTABLE_CONTEXT_DISPLAY_RESOLUTION_MOTION_VECTORS |
    FFX_VK_PORTABLE_CONTEXT_JITTERED_MOTION_VECTORS |
    FFX_VK_PORTABLE_CONTEXT_DEPTH_INVERTED |
    FFX_VK_PORTABLE_CONTEXT_DEPTH_INFINITE |
    FFX_VK_PORTABLE_CONTEXT_AUTO_EXPOSURE |
    FFX_VK_PORTABLE_CONTEXT_DYNAMIC_RESOLUTION |
    FFX_VK_PORTABLE_CONTEXT_DEBUG_CHECKING;

struct OwnedImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    FfxResourceDescription description{};
    const wchar_t* name = nullptr;
};

struct BackendCreateTracker {
    FfxCreateBackendContextFunc original = nullptr;
    bool created = false;
};

thread_local BackendCreateTracker* activeBackendCreateTracker = nullptr;

static FfxErrorCode trackedCreateBackendContext(
    FfxInterface* backendInterface,
    FfxEffect effect,
    FfxEffectBindlessConfig* bindlessConfig,
    FfxUInt32* effectContextId)
{
    if (activeBackendCreateTracker == nullptr ||
        activeBackendCreateTracker->original == nullptr)
        return static_cast<FfxErrorCode>(FFX_ERROR_INCOMPLETE_INTERFACE);
    const FfxErrorCode result = activeBackendCreateTracker->original(
        backendInterface, effect, bindlessConfig, effectContextId);
    if (result == FFX_OK)
        activeBackendCreateTracker->created = true;
    return result;
}

static FfxVkPortableResult fromFfxResult(FfxErrorCode result)
{
    if (result != FFX_OK) ffxVkPortableLogf("FidelityFX call failed: error 0x%08x", static_cast<uint32_t>(result));
    switch (static_cast<uint32_t>(result)) {
    case FFX_OK:
        return FFX_VK_PORTABLE_OK;
    case FFX_ERROR_INVALID_POINTER:
    case FFX_ERROR_NULL_DEVICE:
        return FFX_VK_PORTABLE_ERROR_INVALID_POINTER;
    case FFX_ERROR_INVALID_SIZE:
    case FFX_ERROR_INVALID_ENUM:
    case FFX_ERROR_INVALID_ARGUMENT:
    case FFX_ERROR_OUT_OF_RANGE:
        return FFX_VK_PORTABLE_ERROR_INVALID_ARGUMENT;
    case FFX_ERROR_OUT_OF_MEMORY:
    case FFX_ERROR_INSUFFICIENT_MEMORY:
        return FFX_VK_PORTABLE_ERROR_OUT_OF_MEMORY;
    default:
        return FFX_VK_PORTABLE_ERROR_BACKEND;
    }
}

static FfxVkPortableResult fromVkResult(VkResult result)
{
    if (result == VK_SUCCESS)
        return FFX_VK_PORTABLE_OK;
    if (result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY)
        return FFX_VK_PORTABLE_ERROR_OUT_OF_MEMORY;
    if (result == VK_ERROR_FORMAT_NOT_SUPPORTED || result == VK_ERROR_FEATURE_NOT_PRESENT)
        return FFX_VK_PORTABLE_ERROR_UNSUPPORTED;
    return FFX_VK_PORTABLE_ERROR_VULKAN;
}

static FfxResourceStates toFfxState(FfxVkPortableResourceState state)
{
    switch (state) {
    case FFX_VK_PORTABLE_RESOURCE_STATE_UNDEFINED:
        return FFX_RESOURCE_STATE_COMMON;
    case FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ:
        return FFX_RESOURCE_STATE_GENERIC_READ;
    case FFX_VK_PORTABLE_RESOURCE_STATE_COMPUTE_READ:
        return FFX_RESOURCE_STATE_COMPUTE_READ;
    case FFX_VK_PORTABLE_RESOURCE_STATE_UNORDERED_ACCESS:
        return FFX_RESOURCE_STATE_UNORDERED_ACCESS;
    case FFX_VK_PORTABLE_RESOURCE_STATE_TRANSFER_SOURCE:
        return FFX_RESOURCE_STATE_COPY_SRC;
    case FFX_VK_PORTABLE_RESOURCE_STATE_TRANSFER_DESTINATION:
        return FFX_RESOURCE_STATE_COPY_DEST;
    case FFX_VK_PORTABLE_RESOURCE_STATE_COLOR_ATTACHMENT:
        return FFX_RESOURCE_STATE_RENDER_TARGET;
    case FFX_VK_PORTABLE_RESOURCE_STATE_DEPTH_ATTACHMENT:
        return FFX_RESOURCE_STATE_DEPTH_ATTACHEMENT;
    case FFX_VK_PORTABLE_RESOURCE_STATE_PRESENT:
        return FFX_RESOURCE_STATE_PRESENT;
    default:
        return FFX_RESOURCE_STATE_COMMON;
    }
}

static uint32_t toFfxContextFlags(uint32_t flags)
{
    uint32_t result = flags & (FFX_VK_PORTABLE_CONTEXT_DYNAMIC_RESOLUTION * 2u - 1u);
    if (flags & FFX_VK_PORTABLE_CONTEXT_DEBUG_CHECKING)
        result |= FFX_FSR3UPSCALER_ENABLE_DEBUG_CHECKING;
    return result;
}

#if defined(FFX_VK_PORTABLE_HAS_FSR3_FRAMEGEN) && FFX_VK_PORTABLE_HAS_FSR3_FRAMEGEN
static uint32_t toFrameInterpolationContextFlags(uint32_t flags)
{
    uint32_t result = 0;
    if (flags & FFX_VK_PORTABLE_CONTEXT_HDR_COLOR_INPUT)
        result |= FFX_FRAMEINTERPOLATION_ENABLE_HDR_COLOR_INPUT;
    if (flags & FFX_VK_PORTABLE_CONTEXT_DISPLAY_RESOLUTION_MOTION_VECTORS)
        result |= FFX_FRAMEINTERPOLATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS;
    if (flags & FFX_VK_PORTABLE_CONTEXT_JITTERED_MOTION_VECTORS)
        result |= FFX_FRAMEINTERPOLATION_ENABLE_JITTER_MOTION_VECTORS;
    if (flags & FFX_VK_PORTABLE_CONTEXT_DEPTH_INVERTED)
        result |= FFX_FRAMEINTERPOLATION_ENABLE_DEPTH_INVERTED;
    if (flags & FFX_VK_PORTABLE_CONTEXT_DEPTH_INFINITE)
        result |= FFX_FRAMEINTERPOLATION_ENABLE_DEPTH_INFINITE;
    if (flags & FFX_VK_PORTABLE_CONTEXT_DEBUG_CHECKING)
        result |= FFX_FRAMEINTERPOLATION_ENABLE_DEBUG_CHECKING;
    return result;
}

static FfxBackbufferTransferFunction toFfxTransferFunction(
    FfxVkPortableTransferFunction transferFunction)
{
    switch (transferFunction) {
    case FFX_VK_PORTABLE_TRANSFER_FUNCTION_PQ:
        return FFX_BACKBUFFER_TRANSFER_FUNCTION_PQ;
    case FFX_VK_PORTABLE_TRANSFER_FUNCTION_SCRGB:
        return FFX_BACKBUFFER_TRANSFER_FUNCTION_SCRGB;
    case FFX_VK_PORTABLE_TRANSFER_FUNCTION_SRGB:
    default:
        return FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    }
}
#endif /* FFX_VK_PORTABLE_HAS_FSR3_FRAMEGEN */

// (MHGU Recomp) Diagnostics go to the application's log (ffxVkPortableSetLog). FidelityFX's messages used to be
// fwprintf'd to stderr, which never reaches an Android log and is lost on a byte-oriented stderr anyway.
static std::atomic<FfxVkPortableLogFn> logFn{nullptr};
static std::atomic<int> logLines{0};

extern "C" void ffxVkPortableSetLog(FfxVkPortableLogFn fn) { logFn.store(fn); }

extern "C" void ffxVkPortableLogf(const char* format, ...)
{
    FfxVkPortableLogFn fn = logFn.load();
    if (!fn || logLines.fetch_add(1) >= 64) return;  // (a failure repeated every frame would fill the log)
    char line[512];
    va_list ap;
    va_start(ap, format);
    std::vsnprintf(line, sizeof line, format, ap);
    va_end(ap);
    fn(line);
}

extern "C" void ffxVkPortableNoteError(const char* file, int line)
{
    const char* base = file;
    for (const char* p = file; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    ffxVkPortableLogf("FidelityFX backend error at %s:%d", base, line);
}

static void ffxMessage(FfxMsgType type, const wchar_t* message)
{
    char narrow[480];
    size_t n = 0;
    for (const wchar_t* p = message ? message : L"(no message)"; *p && n < sizeof narrow - 1; p++)
        narrow[n++] = *p < 0x80 ? (char)*p : '?';
    narrow[n] = 0;
    ffxVkPortableLogf("FidelityFX %s: %s", type == FFX_MESSAGE_TYPE_ERROR ? "error" : "warning", narrow);
}

static uint32_t findMemoryType(
    VkPhysicalDevice physicalDevice,
    uint32_t allowedTypes,
    VkBool32 deviceCoherentMemoryEnabled)
{
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &properties);

    uint32_t fallback = UINT32_MAX;
    for (uint32_t index = 0; index < properties.memoryTypeCount; ++index) {
        if (!(allowedTypes & (1u << index)))
            continue;
        const VkMemoryPropertyFlags flags = properties.memoryTypes[index].propertyFlags;
        if (deviceCoherentMemoryEnabled != VK_TRUE &&
            (flags & VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD))
            continue;
        if (fallback == UINT32_MAX)
            fallback = index;
        if (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
            return index;
    }
    return fallback;
}

static VkFormat toVkFormat(FfxSurfaceFormat format)
{
    switch (format) {
    case FFX_SURFACE_FORMAT_R32_FLOAT:
        return VK_FORMAT_R32_SFLOAT;
    case FFX_SURFACE_FORMAT_R16G16_FLOAT:
        return VK_FORMAT_R16G16_SFLOAT;
    case FFX_SURFACE_FORMAT_R16G16_SINT:
        return VK_FORMAT_R16G16_SINT;
    case FFX_SURFACE_FORMAT_R32_UINT:
        return VK_FORMAT_R32_UINT;
    default:
        return VK_FORMAT_UNDEFINED;
    }
}

static void destroyOwnedImage(const FfxVkPortableDeviceInfo& deviceInfo, OwnedImage* image)
{
    if (image->image != VK_NULL_HANDLE)
        vkDestroyImage(deviceInfo.device, image->image, deviceInfo.allocationCallbacks);
    if (image->memory != VK_NULL_HANDLE)
        vkFreeMemory(deviceInfo.device, image->memory, deviceInfo.allocationCallbacks);
    *image = {};
}

static FfxVkPortableResult createOwnedImage(
    const FfxVkPortableDeviceInfo& deviceInfo,
    const FfxCreateResourceDescription& sharedDescription,
    const wchar_t* name,
    OwnedImage* output)
{
    const FfxResourceDescription& description = sharedDescription.resourceDescription;
    const VkFormat format = toVkFormat(description.format);
    if (format == VK_FORMAT_UNDEFINED)
        return FFX_VK_PORTABLE_ERROR_UNSUPPORTED;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {description.width, description.height, 1};
    imageInfo.mipLevels = description.mipCount;
    imageInfo.arrayLayers = description.depth;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                      VK_IMAGE_USAGE_STORAGE_BIT |
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (description.usage & FFX_RESOURCE_USAGE_RENDERTARGET)
        imageInfo.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkResult vkResult = vkCreateImage(
        deviceInfo.device, &imageInfo, deviceInfo.allocationCallbacks, &output->image);
    if (vkResult != VK_SUCCESS)
        return fromVkResult(vkResult);

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(deviceInfo.device, output->image, &requirements);
    const uint32_t memoryType = findMemoryType(
        deviceInfo.physicalDevice,
        requirements.memoryTypeBits,
        deviceInfo.deviceCoherentMemoryEnabled);
    if (memoryType == UINT32_MAX) {
        destroyOwnedImage(deviceInfo, output);
        return FFX_VK_PORTABLE_ERROR_UNSUPPORTED;
    }

    VkMemoryAllocateInfo allocationInfo{};
    allocationInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocationInfo.allocationSize = requirements.size;
    allocationInfo.memoryTypeIndex = memoryType;
    vkResult = vkAllocateMemory(
        deviceInfo.device, &allocationInfo, deviceInfo.allocationCallbacks, &output->memory);
    if (vkResult != VK_SUCCESS) {
        const FfxVkPortableResult result = fromVkResult(vkResult);
        destroyOwnedImage(deviceInfo, output);
        return result;
    }

    vkResult = vkBindImageMemory(deviceInfo.device, output->image, output->memory, 0);
    if (vkResult != VK_SUCCESS) {
        const FfxVkPortableResult result = fromVkResult(vkResult);
        destroyOwnedImage(deviceInfo, output);
        return result;
    }

    output->description = description;
    output->name = name;
    return FFX_VK_PORTABLE_OK;
}

static FfxResource externalResource(const FfxVkPortableImage& image, const wchar_t* name)
{
    if (image.image == VK_NULL_HANDLE)
        return {};

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = image.format;
    imageInfo.extent = {image.extent.width, image.extent.height, 1};
    imageInfo.mipLevels = image.mipCount;
    imageInfo.arrayLayers = image.arrayLayers;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = image.usage;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    FfxResourceDescription description =
        ffxGetImageResourceDescriptionVK(image.image, imageInfo);
    if (image.state == FFX_VK_PORTABLE_RESOURCE_STATE_UNDEFINED) {
        description.flags = static_cast<FfxResourceFlags>(
            description.flags | FFX_RESOURCE_FLAGS_UNDEFINED);
    }
    return ffxGetResourceVK(
        reinterpret_cast<void*>(image.image), description, name, toFfxState(image.state));
}

static FfxResource ownedResource(const OwnedImage& image, bool undefined)
{
    FfxResourceDescription description = image.description;
    if (undefined) {
        description.flags = static_cast<FfxResourceFlags>(
            description.flags | FFX_RESOURCE_FLAGS_UNDEFINED);
    }
    return ffxGetResourceVK(
        reinterpret_cast<void*>(image.image), description, image.name,
        FFX_RESOURCE_STATE_UNORDERED_ACCESS);
}

struct BackendState {
    void* scratchAllocation = nullptr;
    size_t scratchSize = 0;
    FfxInterface interface{};
};

static FfxVkPortableResult createBackendState(
    const FfxVkPortableDeviceInfo& deviceInfo,
    BackendState* output)
{
    output->scratchSize = ffxGetScratchMemorySizeVK(deviceInfo.physicalDevice, 1);
    if (output->scratchSize == 0 ||
        output->scratchSize > std::numeric_limits<size_t>::max() - 31u)
        return FFX_VK_PORTABLE_ERROR_OUT_OF_MEMORY;
    /* ffxGetInterfaceVK examines BackendContext_VK::refCount before it
     * initializes the scratch arena.  The upstream helper therefore expects
     * a zeroed caller-owned allocation; malloc makes context creation depend
     * on recycled heap contents after a resize. */
    output->scratchAllocation = std::calloc(1, output->scratchSize + 31u);
    if (output->scratchAllocation == nullptr)
        return FFX_VK_PORTABLE_ERROR_OUT_OF_MEMORY;
    const uintptr_t alignedAddress =
        (reinterpret_cast<uintptr_t>(output->scratchAllocation) + 31u) & ~uintptr_t{31u};
    VkDeviceContext vkDeviceContext{
        deviceInfo.device,
        deviceInfo.physicalDevice,
        deviceInfo.getDeviceProcAddr,
        deviceInfo.deviceCoherentMemoryEnabled,
        deviceInfo.shaderFloat16Enabled,
        deviceInfo.subgroupSizeControlEnabled,
        deviceInfo.computeFullSubgroupsEnabled,
        deviceInfo.synchronization2Enabled,
        deviceInfo.bufferMarkerEnabled,
        deviceInfo.debugUtilsEnabled,
        deviceInfo.shaderStorageBufferArrayNonUniformIndexingEnabled,
        deviceInfo.accelerationStructureEnabled,
    };
    const FfxErrorCode ffxResult = ffxGetInterfaceVK(
        &output->interface, ffxGetDeviceVK(&vkDeviceContext),
        reinterpret_cast<void*>(alignedAddress), output->scratchSize, 1);
    if (ffxResult != FFX_OK) {
        std::free(output->scratchAllocation);
        *output = {};
        return fromFfxResult(ffxResult);
    }
    return FFX_VK_PORTABLE_OK;
}

static void destroyBackendState(BackendState* state)
{
    std::free(state->scratchAllocation);
    *state = {};
}

} // namespace

struct FfxVkPortableUpscaleContext {
    uint64_t magic = kContextMagic;
    FfxVkPortableDeviceInfo deviceInfo{};
    FfxVkPortableUpscaleCreateInfo createInfo{};
    void* scratchAllocation = nullptr;
    size_t scratchSize = 0;
    FfxFsr3UpscalerContext ffxContext{};
    bool ffxContextCreated = false;
    bool sharedResourcesInitialized = false;
    OwnedImage dilatedDepth{};
    OwnedImage dilatedMotionVectors{};
    OwnedImage reconstructedPreviousDepth{};
    std::mutex mutex;
};

extern "C" FfxVkPortableResult ffxVkPortableUpscaleContextCreate(
    const FfxVkPortableDeviceInfo* deviceInfo,
    const FfxVkPortableUpscaleCreateInfo* createInfo,
    FfxVkPortableUpscaleContext** context)
{
    if (context == nullptr)
        return FFX_VK_PORTABLE_ERROR_INVALID_POINTER;
    *context = nullptr;
    if (deviceInfo == nullptr || createInfo == nullptr)
        return FFX_VK_PORTABLE_ERROR_INVALID_POINTER;
    if (deviceInfo->structSize < sizeof(*deviceInfo) || createInfo->structSize < sizeof(*createInfo))
        return FFX_VK_PORTABLE_ERROR_INVALID_STRUCT_SIZE;
    if (deviceInfo->physicalDevice == VK_NULL_HANDLE || deviceInfo->device == VK_NULL_HANDLE ||
        deviceInfo->getDeviceProcAddr == nullptr)
        return FFX_VK_PORTABLE_ERROR_INVALID_POINTER;
    if (ffxVkPortableValidateUpscaleCreateInfo(createInfo) != FFX_VK_PORTABLE_VALIDATION_NONE ||
        (createInfo->flags & ~kKnownContextFlags) != 0)
        return FFX_VK_PORTABLE_ERROR_INVALID_ARGUMENT;

    FfxVkPortableDeviceCapabilities capabilities{};
    capabilities.structSize = sizeof(capabilities);
    const FfxVkPortableResult capabilityResult =
        ffxVkPortableQueryDeviceCapabilities(deviceInfo->physicalDevice, &capabilities);
    if (capabilityResult != FFX_VK_PORTABLE_OK)
        return capabilityResult;
    if (!capabilities.fsr3ComputePrerequisites)
        return FFX_VK_PORTABLE_ERROR_UNSUPPORTED;
    /* The capability query reports physical support. The generated FSR3
     * accumulate shaders additionally require this feature on the actual
     * logical device, which Vulkan cannot query after device creation. */
    if (deviceInfo->shaderStorageImageWriteWithoutFormatEnabled != VK_TRUE)
        return FFX_VK_PORTABLE_ERROR_UNSUPPORTED;

    FfxVkPortableUpscaleContext* result = new (std::nothrow) FfxVkPortableUpscaleContext;
    if (result == nullptr)
        return FFX_VK_PORTABLE_ERROR_OUT_OF_MEMORY;
    result->deviceInfo = *deviceInfo;
    result->createInfo = *createInfo;

    result->scratchSize = ffxGetScratchMemorySizeVK(deviceInfo->physicalDevice, 1);
    if (result->scratchSize == 0 ||
        result->scratchSize > std::numeric_limits<size_t>::max() - 31u) {
        delete result;
        return FFX_VK_PORTABLE_ERROR_OUT_OF_MEMORY;
    }
    /* See createBackendState: the upstream backend reads its scratch header
     * before clearing it, so this storage has a required zero-initial state. */
    result->scratchAllocation = std::calloc(1, result->scratchSize + 31u);
    if (result->scratchAllocation == nullptr) {
        delete result;
        return FFX_VK_PORTABLE_ERROR_OUT_OF_MEMORY;
    }
    const uintptr_t alignedAddress =
        (reinterpret_cast<uintptr_t>(result->scratchAllocation) + 31u) & ~uintptr_t{31u};

    VkDeviceContext vkDeviceContext{
        deviceInfo->device,
        deviceInfo->physicalDevice,
        deviceInfo->getDeviceProcAddr,
        deviceInfo->deviceCoherentMemoryEnabled,
        deviceInfo->shaderFloat16Enabled,
        deviceInfo->subgroupSizeControlEnabled,
        deviceInfo->computeFullSubgroupsEnabled,
        deviceInfo->synchronization2Enabled,
        deviceInfo->bufferMarkerEnabled,
        deviceInfo->debugUtilsEnabled,
        deviceInfo->shaderStorageBufferArrayNonUniformIndexingEnabled,
        deviceInfo->accelerationStructureEnabled,
    };
    FfxInterface backend{};
    FfxErrorCode ffxResult = ffxGetInterfaceVK(
        &backend, ffxGetDeviceVK(&vkDeviceContext), reinterpret_cast<void*>(alignedAddress),
        result->scratchSize, 1);
    if (ffxResult != FFX_OK) {
        std::free(result->scratchAllocation);
        delete result;
        return fromFfxResult(ffxResult);
    }

    FfxFsr3UpscalerContextDescription description{};
    description.flags = toFfxContextFlags(createInfo->flags);
    description.maxRenderSize = {createInfo->maxRenderSize.width, createInfo->maxRenderSize.height};
    description.maxUpscaleSize = {createInfo->maxOutputSize.width, createInfo->maxOutputSize.height};
    description.fpMessage = (createInfo->flags & FFX_VK_PORTABLE_CONTEXT_DEBUG_CHECKING)
        ? ffxMessage
        : nullptr;
    BackendCreateTracker createTracker{backend.fpCreateBackendContext, false};
    backend.fpCreateBackendContext = trackedCreateBackendContext;
    description.backendInterface = backend;

    activeBackendCreateTracker = &createTracker;
    ffxResult = ffxFsr3UpscalerContextCreate(&result->ffxContext, &description);
    activeBackendCreateTracker = nullptr;
    if (ffxResult != FFX_OK) {
        if (createTracker.created)
            ffxFsr3UpscalerContextDestroy(&result->ffxContext);
        std::free(result->scratchAllocation);
        delete result;
        return fromFfxResult(ffxResult);
    }
    result->ffxContextCreated = true;

    FfxFsr3UpscalerSharedResourceDescriptions shared{};
    ffxResult = ffxFsr3UpscalerGetSharedResourceDescriptions(&result->ffxContext, &shared);
    FfxVkPortableResult createResult = fromFfxResult(ffxResult);
    if (createResult == FFX_VK_PORTABLE_OK) {
        createResult = createOwnedImage(
            *deviceInfo, shared.dilatedDepth, L"portable FSR3 dilated depth",
            &result->dilatedDepth);
    }
    if (createResult == FFX_VK_PORTABLE_OK) {
        createResult = createOwnedImage(
            *deviceInfo, shared.dilatedMotionVectors, L"portable FSR3 dilated motion vectors",
            &result->dilatedMotionVectors);
    }
    if (createResult == FFX_VK_PORTABLE_OK) {
        createResult = createOwnedImage(
            *deviceInfo, shared.reconstructedPrevNearestDepth,
            L"portable FSR3 reconstructed previous depth",
            &result->reconstructedPreviousDepth);
    }
    if (createResult != FFX_VK_PORTABLE_OK) {
        ffxFsr3UpscalerContextDestroy(&result->ffxContext);
        destroyOwnedImage(*deviceInfo, &result->dilatedDepth);
        destroyOwnedImage(*deviceInfo, &result->dilatedMotionVectors);
        destroyOwnedImage(*deviceInfo, &result->reconstructedPreviousDepth);
        std::free(result->scratchAllocation);
        delete result;
        return createResult;
    }

    *context = result;
    return FFX_VK_PORTABLE_OK;
}

extern "C" FfxVkPortableResult ffxVkPortableUpscaleContextGetMemoryUsage(
    FfxVkPortableUpscaleContext* context,
    FfxVkPortableMemoryUsage* usage)
{
    if (context == nullptr || usage == nullptr)
        return FFX_VK_PORTABLE_ERROR_INVALID_POINTER;
    if (usage->structSize < sizeof(*usage))
        return FFX_VK_PORTABLE_ERROR_INVALID_STRUCT_SIZE;
    if (context->magic != kContextMagic || !context->ffxContextCreated)
        return FFX_VK_PORTABLE_ERROR_INVALID_ARGUMENT;

    std::lock_guard<std::mutex> lock(context->mutex);
    FfxEffectMemoryUsage ffxUsage{};
    const FfxErrorCode result = ffxFsr3UpscalerContextGetGpuMemoryUsage(
        &context->ffxContext, &ffxUsage);
    if (result != FFX_OK)
        return fromFfxResult(result);
    usage->totalUsageInBytes = ffxUsage.totalUsageInBytes;
    usage->aliasableUsageInBytes = ffxUsage.aliasableUsageInBytes;
    return FFX_VK_PORTABLE_OK;
}

extern "C" FfxVkPortableResult ffxVkPortableUpscaleContextRecordDispatch(
    FfxVkPortableUpscaleContext* context,
    const FfxVkPortableUpscaleDispatchInfo* dispatchInfo)
{
    if (context == nullptr || dispatchInfo == nullptr)
        return FFX_VK_PORTABLE_ERROR_INVALID_POINTER;
    if (dispatchInfo->structSize < sizeof(*dispatchInfo))
        return FFX_VK_PORTABLE_ERROR_INVALID_STRUCT_SIZE;
    if (context->magic != kContextMagic || !context->ffxContextCreated)
        return FFX_VK_PORTABLE_ERROR_INVALID_ARGUMENT;
    if (ffxVkPortableValidateUpscaleDispatchInfo(&context->createInfo, dispatchInfo) !=
        FFX_VK_PORTABLE_VALIDATION_NONE)
        return FFX_VK_PORTABLE_ERROR_INVALID_ARGUMENT;

    std::lock_guard<std::mutex> lock(context->mutex);

    FfxFsr3UpscalerDispatchDescription dispatch{};
    dispatch.commandList = ffxGetCommandListVK(dispatchInfo->commandBuffer);
    dispatch.color = externalResource(dispatchInfo->color, L"portable FSR3 color");
    dispatch.depth = externalResource(dispatchInfo->depth, L"portable FSR3 depth");
    dispatch.motionVectors = externalResource(
        dispatchInfo->motionVectors, L"portable FSR3 motion vectors");
    dispatch.exposure = externalResource(dispatchInfo->exposure, L"portable FSR3 exposure");
    dispatch.reactive = externalResource(dispatchInfo->reactiveMask, L"portable FSR3 reactive mask");
    dispatch.transparencyAndComposition = externalResource(
        dispatchInfo->transparencyAndCompositionMask,
        L"portable FSR3 transparency and composition mask");
    dispatch.dilatedDepth = ownedResource(
        context->dilatedDepth, !context->sharedResourcesInitialized);
    dispatch.dilatedMotionVectors = ownedResource(
        context->dilatedMotionVectors, !context->sharedResourcesInitialized);
    dispatch.reconstructedPrevNearestDepth = ownedResource(
        context->reconstructedPreviousDepth, !context->sharedResourcesInitialized);
    dispatch.output = externalResource(dispatchInfo->output, L"portable FSR3 output");
    dispatch.jitterOffset = {dispatchInfo->jitterOffset.x, dispatchInfo->jitterOffset.y};
    dispatch.motionVectorScale = {
        dispatchInfo->motionVectorScale.x,
        dispatchInfo->motionVectorScale.y,
    };
    dispatch.renderSize = {dispatchInfo->renderSize.width, dispatchInfo->renderSize.height};
    dispatch.upscaleSize = {dispatchInfo->outputSize.width, dispatchInfo->outputSize.height};
    dispatch.enableSharpening = dispatchInfo->enableSharpening != VK_FALSE;
    dispatch.sharpness = dispatchInfo->sharpness;
    dispatch.frameTimeDelta = dispatchInfo->frameTimeMilliseconds;
    dispatch.preExposure = dispatchInfo->preExposure;
    dispatch.reset = dispatchInfo->reset != VK_FALSE;
    dispatch.cameraNear = dispatchInfo->cameraNear;
    dispatch.cameraFar = dispatchInfo->cameraFar;
    dispatch.cameraFovAngleVertical = dispatchInfo->cameraVerticalFovRadians;
    dispatch.viewSpaceToMetersFactor = dispatchInfo->viewSpaceToMeters;

    const FfxErrorCode ffxResult =
        ffxFsr3UpscalerContextDispatch(&context->ffxContext, &dispatch);
    if (ffxResult == FFX_OK)
        context->sharedResourcesInitialized = true;
    return fromFfxResult(ffxResult);
}

extern "C" FfxVkPortableResult ffxVkPortableUpscaleContextDestroy(
    FfxVkPortableUpscaleContext* context)
{
    if (context == nullptr)
        return FFX_VK_PORTABLE_OK;
    if (context->magic != kContextMagic)
        return FFX_VK_PORTABLE_ERROR_INVALID_ARGUMENT;

    FfxErrorCode ffxResult = FFX_OK;
    {
        std::lock_guard<std::mutex> lock(context->mutex);
        context->magic = 0;
        if (context->ffxContextCreated) {
            ffxResult = ffxFsr3UpscalerContextDestroy(&context->ffxContext);
            context->ffxContextCreated = false;
        }
        destroyOwnedImage(context->deviceInfo, &context->dilatedDepth);
        destroyOwnedImage(context->deviceInfo, &context->dilatedMotionVectors);
        destroyOwnedImage(context->deviceInfo, &context->reconstructedPreviousDepth);
        std::free(context->scratchAllocation);
        context->scratchAllocation = nullptr;
    }
    delete context;
    return fromFfxResult(ffxResult);
}

#if !(defined(FFX_VK_PORTABLE_HAS_FSR3_FRAMEGEN) && FFX_VK_PORTABLE_HAS_FSR3_FRAMEGEN)
/* vendoring patch: frame generation is not part of this subset. The
 * public entry points stay linkable and report UNSUPPORTED. */
extern "C" FfxVkPortableResult ffxVkPortableFrameGenerationContextCreate(
    const FfxVkPortableDeviceInfo* deviceInfo,
    const FfxVkPortableFrameGenerationCreateInfo* createInfo,
    FfxVkPortableFrameGenerationContext** context)
{
    (void)deviceInfo;
    (void)createInfo;
    if (context != nullptr)
        *context = nullptr;
    return FFX_VK_PORTABLE_ERROR_UNSUPPORTED;
}

extern "C" FfxVkPortableResult ffxVkPortableFrameGenerationContextPrepare(
    FfxVkPortableFrameGenerationContext* context,
    const FfxVkPortableFrameGenerationPrepareInfo* prepareInfo,
    const FfxVkPortableImage* interpolationSource)
{
    (void)context;
    (void)prepareInfo;
    (void)interpolationSource;
    return FFX_VK_PORTABLE_ERROR_UNSUPPORTED;
}

extern "C" FfxVkPortableResult ffxVkPortableFrameGenerationContextRecordDispatch(
    FfxVkPortableFrameGenerationContext* context,
    const FfxVkPortableFrameGenerationDispatchInfo* dispatchInfo)
{
    (void)context;
    (void)dispatchInfo;
    return FFX_VK_PORTABLE_ERROR_UNSUPPORTED;
}

extern "C" FfxVkPortableResult ffxVkPortableFrameGenerationContextDestroy(
    FfxVkPortableFrameGenerationContext* context)
{
    return context == nullptr ? FFX_VK_PORTABLE_OK : FFX_VK_PORTABLE_ERROR_INVALID_ARGUMENT;
}
#else
struct FfxVkPortableFrameGenerationContext {
    uint64_t magic = kFrameGenerationContextMagic;
    FfxVkPortableDeviceInfo deviceInfo{};
    FfxVkPortableFrameGenerationCreateInfo createInfo{};
    BackendState opticalFlowBackend{};
    BackendState interpolationBackend{};
    FfxOpticalflowContext opticalFlowContext{};
    FfxFrameInterpolationContext interpolationContext{};
    bool opticalFlowCreated = false;
    bool interpolationCreated = false;
    bool sharedResourcesInitialized = false;
    bool prepared = false;
    bool dispatched = false;
    uint64_t preparedFrameId = 0;
    FfxVkPortableExtent2D preparedRenderSize{};
    VkImage preparedInterpolationSource = VK_NULL_HANDLE;
    VkBool32 preparedReset = VK_FALSE;
    OwnedImage opticalFlowVector{};
    OwnedImage opticalFlowScd{};
    OwnedImage dilatedDepth{};
    OwnedImage dilatedMotionVectors{};
    OwnedImage reconstructedPreviousDepth{};
    std::mutex mutex;
};

static bool validFrameGenerationSource(
    const FfxVkPortableImage* image,
    FfxVkPortableExtent2D requiredExtent,
    VkFormat expectedFormat)
{
    return image != nullptr && image->structSize >= sizeof(*image) &&
           image->image != VK_NULL_HANDLE && image->format == expectedFormat &&
           image->extent.width >= requiredExtent.width && image->extent.height >= requiredExtent.height &&
           image->mipCount != 0 && image->arrayLayers == 1 &&
           (image->usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) ==
               (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
}

static bool validFrameGenerationOutput(
    const FfxVkPortableImage* image,
    FfxVkPortableExtent2D requiredExtent,
    VkFormat expectedFormat)
{
    return image != nullptr && image->structSize >= sizeof(*image) &&
           image->image != VK_NULL_HANDLE && image->format == expectedFormat &&
           image->extent.width >= requiredExtent.width && image->extent.height >= requiredExtent.height &&
           image->mipCount != 0 && image->arrayLayers == 1 &&
           (image->usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT)) ==
               (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT);
}

static void destroyFrameGenerationContextResources(FfxVkPortableFrameGenerationContext* context)
{
    if (context->interpolationCreated) {
        ffxFrameInterpolationContextDestroy(&context->interpolationContext);
        context->interpolationCreated = false;
    }
    if (context->opticalFlowCreated) {
        ffxOpticalflowContextDestroy(&context->opticalFlowContext);
        context->opticalFlowCreated = false;
    }
    destroyOwnedImage(context->deviceInfo, &context->opticalFlowVector);
    destroyOwnedImage(context->deviceInfo, &context->opticalFlowScd);
    destroyOwnedImage(context->deviceInfo, &context->dilatedDepth);
    destroyOwnedImage(context->deviceInfo, &context->dilatedMotionVectors);
    destroyOwnedImage(context->deviceInfo, &context->reconstructedPreviousDepth);
    destroyBackendState(&context->interpolationBackend);
    destroyBackendState(&context->opticalFlowBackend);
}

extern "C" FfxVkPortableResult ffxVkPortableFrameGenerationContextCreate(
    const FfxVkPortableDeviceInfo* deviceInfo,
    const FfxVkPortableFrameGenerationCreateInfo* createInfo,
    FfxVkPortableFrameGenerationContext** context)
{
    if (context == nullptr)
        return FFX_VK_PORTABLE_ERROR_INVALID_POINTER;
    *context = nullptr;
    if (deviceInfo == nullptr || createInfo == nullptr)
        return FFX_VK_PORTABLE_ERROR_INVALID_POINTER;
    if (deviceInfo->structSize < sizeof(*deviceInfo) || createInfo->structSize < sizeof(*createInfo))
        return FFX_VK_PORTABLE_ERROR_INVALID_STRUCT_SIZE;
    if (deviceInfo->physicalDevice == VK_NULL_HANDLE || deviceInfo->device == VK_NULL_HANDLE ||
        deviceInfo->getDeviceProcAddr == nullptr)
        return FFX_VK_PORTABLE_ERROR_INVALID_POINTER;
    if (ffxVkPortableValidateFrameGenerationCreateInfo(createInfo) !=
            FFX_VK_PORTABLE_VALIDATION_NONE ||
        (createInfo->flags & ~kKnownContextFlags) != 0 ||
        ffxGetSurfaceFormatVK(createInfo->interpolationSourceFormat) == FFX_SURFACE_FORMAT_UNKNOWN ||
        ffxGetSurfaceFormatVK(createInfo->outputFormat) == FFX_SURFACE_FORMAT_UNKNOWN)
        return FFX_VK_PORTABLE_ERROR_INVALID_ARGUMENT;

    FfxVkPortableDeviceCapabilities capabilities{};
    capabilities.structSize = sizeof(capabilities);
    const FfxVkPortableResult capabilityResult =
        ffxVkPortableQueryDeviceCapabilities(deviceInfo->physicalDevice, &capabilities);
    if (capabilityResult != FFX_VK_PORTABLE_OK)
        return capabilityResult;
    if (!capabilities.fsr3ComputePrerequisites)
        return FFX_VK_PORTABLE_ERROR_UNSUPPORTED;

    FfxVkPortableFrameGenerationContext* result =
        new (std::nothrow) FfxVkPortableFrameGenerationContext;
    if (result == nullptr)
        return FFX_VK_PORTABLE_ERROR_OUT_OF_MEMORY;
    result->deviceInfo = *deviceInfo;
    result->createInfo = *createInfo;

    FfxVkPortableResult createResult = createBackendState(*deviceInfo, &result->opticalFlowBackend);
    if (createResult == FFX_VK_PORTABLE_OK)
        createResult = createBackendState(*deviceInfo, &result->interpolationBackend);
    if (createResult != FFX_VK_PORTABLE_OK) {
        destroyFrameGenerationContextResources(result);
        delete result;
        return createResult;
    }

    FfxOpticalflowContextDescription opticalFlowDescription{};
    opticalFlowDescription.backendInterface = result->opticalFlowBackend.interface;
    opticalFlowDescription.resolution = {createInfo->displaySize.width, createInfo->displaySize.height};
    BackendCreateTracker opticalFlowTracker{
        opticalFlowDescription.backendInterface.fpCreateBackendContext, false};
    opticalFlowDescription.backendInterface.fpCreateBackendContext = trackedCreateBackendContext;
    activeBackendCreateTracker = &opticalFlowTracker;
    FfxErrorCode ffxResult = ffxOpticalflowContextCreate(
        &result->opticalFlowContext, &opticalFlowDescription);
    activeBackendCreateTracker = nullptr;
    if (ffxResult != FFX_OK) {
        if (opticalFlowTracker.created)
            ffxOpticalflowContextDestroy(&result->opticalFlowContext);
        destroyFrameGenerationContextResources(result);
        delete result;
        return fromFfxResult(ffxResult);
    }
    result->opticalFlowCreated = true;

    FfxFrameInterpolationContextDescription interpolationDescription{};
    interpolationDescription.backendInterface = result->interpolationBackend.interface;
    interpolationDescription.flags = toFrameInterpolationContextFlags(createInfo->flags);
    interpolationDescription.maxRenderSize = {createInfo->maxRenderSize.width, createInfo->maxRenderSize.height};
    interpolationDescription.displaySize = {createInfo->displaySize.width, createInfo->displaySize.height};
    interpolationDescription.backBufferFormat = ffxGetSurfaceFormatVK(createInfo->outputFormat);
    interpolationDescription.previousInterpolationSourceFormat =
        ffxGetSurfaceFormatVK(createInfo->interpolationSourceFormat);
    BackendCreateTracker interpolationTracker{
        interpolationDescription.backendInterface.fpCreateBackendContext, false};
    interpolationDescription.backendInterface.fpCreateBackendContext = trackedCreateBackendContext;
    activeBackendCreateTracker = &interpolationTracker;
    ffxResult = ffxFrameInterpolationContextCreate(
        &result->interpolationContext, &interpolationDescription);
    activeBackendCreateTracker = nullptr;
    if (ffxResult != FFX_OK) {
        if (interpolationTracker.created)
            ffxFrameInterpolationContextDestroy(&result->interpolationContext);
        destroyFrameGenerationContextResources(result);
        delete result;
        return fromFfxResult(ffxResult);
    }
    result->interpolationCreated = true;

    FfxOpticalflowSharedResourceDescriptions opticalFlowShared{};
    FfxFrameInterpolationSharedResourceDescriptions interpolationShared{};
    ffxResult = ffxOpticalflowGetSharedResourceDescriptions(
        &result->opticalFlowContext, &opticalFlowShared);
    createResult = fromFfxResult(ffxResult);
    if (createResult == FFX_VK_PORTABLE_OK)
        createResult = createOwnedImage(*deviceInfo, opticalFlowShared.opticalFlowVector,
                                        L"portable FSR3 optical-flow vector", &result->opticalFlowVector);
    if (createResult == FFX_VK_PORTABLE_OK)
        createResult = createOwnedImage(*deviceInfo, opticalFlowShared.opticalFlowSCD,
                                        L"portable FSR3 optical-flow scene change", &result->opticalFlowScd);
    if (createResult == FFX_VK_PORTABLE_OK) {
        ffxResult = ffxFrameInterpolationGetSharedResourceDescriptions(
            &result->interpolationContext, &interpolationShared);
        createResult = fromFfxResult(ffxResult);
    }
    if (createResult == FFX_VK_PORTABLE_OK)
        createResult = createOwnedImage(*deviceInfo, interpolationShared.dilatedDepth,
                                        L"portable FSR3 FI dilated depth", &result->dilatedDepth);
    if (createResult == FFX_VK_PORTABLE_OK)
        createResult = createOwnedImage(*deviceInfo, interpolationShared.dilatedMotionVectors,
                                        L"portable FSR3 FI dilated motion vectors", &result->dilatedMotionVectors);
    if (createResult == FFX_VK_PORTABLE_OK)
        createResult = createOwnedImage(*deviceInfo, interpolationShared.reconstructedPrevNearestDepth,
                                        L"portable FSR3 FI reconstructed previous depth",
                                        &result->reconstructedPreviousDepth);
    if (createResult != FFX_VK_PORTABLE_OK) {
        destroyFrameGenerationContextResources(result);
        delete result;
        return createResult;
    }

    *context = result;
    return FFX_VK_PORTABLE_OK;
}

extern "C" FfxVkPortableResult ffxVkPortableFrameGenerationContextPrepare(
    FfxVkPortableFrameGenerationContext* context,
    const FfxVkPortableFrameGenerationPrepareInfo* prepareInfo,
    const FfxVkPortableImage* interpolationSource)
{
    if (context == nullptr || prepareInfo == nullptr || interpolationSource == nullptr)
        return FFX_VK_PORTABLE_ERROR_INVALID_POINTER;
    if (context->magic != kFrameGenerationContextMagic || !context->opticalFlowCreated ||
        !context->interpolationCreated)
        return FFX_VK_PORTABLE_ERROR_INVALID_ARGUMENT;
    if (ffxVkPortableValidateFrameGenerationPrepareInfo(&context->createInfo, prepareInfo) !=
            FFX_VK_PORTABLE_VALIDATION_NONE ||
        !validFrameGenerationSource(interpolationSource, context->createInfo.displaySize,
                                    context->createInfo.interpolationSourceFormat))
        return FFX_VK_PORTABLE_ERROR_INVALID_ARGUMENT;

    std::lock_guard<std::mutex> lock(context->mutex);
    FfxOpticalflowDispatchDescription opticalFlowDispatch{};
    opticalFlowDispatch.commandList = ffxGetCommandListVK(prepareInfo->commandBuffer);
    opticalFlowDispatch.color = externalResource(*interpolationSource,
                                                  L"portable FSR3 FI interpolation source");
    opticalFlowDispatch.opticalFlowVector = ownedResource(
        context->opticalFlowVector, !context->sharedResourcesInitialized);
    opticalFlowDispatch.opticalFlowSCD = ownedResource(
        context->opticalFlowScd, !context->sharedResourcesInitialized);
    opticalFlowDispatch.reset = prepareInfo->reset != VK_FALSE;
    opticalFlowDispatch.backbufferTransferFunction = static_cast<int>(
        toFfxTransferFunction(prepareInfo->transferFunction));
    opticalFlowDispatch.minMaxLuminance = {prepareInfo->minLuminance, prepareInfo->maxLuminance};
    FfxErrorCode ffxResult = ffxOpticalflowContextDispatch(
        &context->opticalFlowContext, &opticalFlowDispatch);
    if (ffxResult != FFX_OK)
        return fromFfxResult(ffxResult);

    FfxFrameInterpolationPrepareDescription interpolationPrepare{};
    interpolationPrepare.commandList = ffxGetCommandListVK(prepareInfo->commandBuffer);
    interpolationPrepare.renderSize = {prepareInfo->renderSize.width, prepareInfo->renderSize.height};
    interpolationPrepare.depth = externalResource(prepareInfo->depth, L"portable FSR3 FI depth");
    interpolationPrepare.motionVectors = externalResource(
        prepareInfo->motionVectors, L"portable FSR3 FI motion vectors");
    interpolationPrepare.jitterOffset = {prepareInfo->jitterOffset.x, prepareInfo->jitterOffset.y};
    interpolationPrepare.motionVectorScale = {
        prepareInfo->motionVectorScale.x, prepareInfo->motionVectorScale.y};
    interpolationPrepare.frameTimeDelta = prepareInfo->frameTimeMilliseconds;
    interpolationPrepare.cameraNear = prepareInfo->cameraNear;
    interpolationPrepare.cameraFar = prepareInfo->cameraFar;
    interpolationPrepare.viewSpaceToMetersFactor = prepareInfo->viewSpaceToMeters;
    interpolationPrepare.cameraFovAngleVertical = prepareInfo->cameraVerticalFovRadians;
    interpolationPrepare.frameID = prepareInfo->frameId;
    interpolationPrepare.dilatedDepth = ownedResource(
        context->dilatedDepth, !context->sharedResourcesInitialized);
    interpolationPrepare.dilatedMotionVectors = ownedResource(
        context->dilatedMotionVectors, !context->sharedResourcesInitialized);
    interpolationPrepare.reconstructedPrevDepth = ownedResource(
        context->reconstructedPreviousDepth, !context->sharedResourcesInitialized);
    interpolationPrepare.cameraPosition[0] = prepareInfo->cameraPosition.x;
    interpolationPrepare.cameraPosition[1] = prepareInfo->cameraPosition.y;
    interpolationPrepare.cameraPosition[2] = prepareInfo->cameraPosition.z;
    interpolationPrepare.cameraUp[0] = prepareInfo->cameraUp.x;
    interpolationPrepare.cameraUp[1] = prepareInfo->cameraUp.y;
    interpolationPrepare.cameraUp[2] = prepareInfo->cameraUp.z;
    interpolationPrepare.cameraRight[0] = prepareInfo->cameraRight.x;
    interpolationPrepare.cameraRight[1] = prepareInfo->cameraRight.y;
    interpolationPrepare.cameraRight[2] = prepareInfo->cameraRight.z;
    interpolationPrepare.cameraForward[0] = prepareInfo->cameraForward.x;
    interpolationPrepare.cameraForward[1] = prepareInfo->cameraForward.y;
    interpolationPrepare.cameraForward[2] = prepareInfo->cameraForward.z;
    ffxResult = ffxFrameInterpolationPrepare(&context->interpolationContext, &interpolationPrepare);
    if (ffxResult != FFX_OK)
        return fromFfxResult(ffxResult);
    context->sharedResourcesInitialized = true;
    context->prepared = true;
    context->dispatched = false;
    context->preparedFrameId = prepareInfo->frameId;
    context->preparedRenderSize = prepareInfo->renderSize;
    context->preparedInterpolationSource = interpolationSource->image;
    context->preparedReset = prepareInfo->reset;
    return FFX_VK_PORTABLE_OK;
}

extern "C" FfxVkPortableResult ffxVkPortableFrameGenerationContextRecordDispatch(
    FfxVkPortableFrameGenerationContext* context,
    const FfxVkPortableFrameGenerationDispatchInfo* dispatchInfo)
{
    if (context == nullptr || dispatchInfo == nullptr)
        return FFX_VK_PORTABLE_ERROR_INVALID_POINTER;
    if (context->magic != kFrameGenerationContextMagic || !context->interpolationCreated)
        return FFX_VK_PORTABLE_ERROR_INVALID_ARGUMENT;
    if (ffxVkPortableValidateFrameGenerationDispatchInfo(&context->createInfo, dispatchInfo) !=
        FFX_VK_PORTABLE_VALIDATION_NONE)
        return FFX_VK_PORTABLE_ERROR_INVALID_ARGUMENT;

    std::lock_guard<std::mutex> lock(context->mutex);
    if (!context->prepared || context->dispatched || context->preparedFrameId != dispatchInfo->frameId)
        return FFX_VK_PORTABLE_ERROR_INVALID_ARGUMENT;

    const FfxVkPortableImage* interpolationSource = &dispatchInfo->currentColor;
    if (dispatchInfo->hudlessColor.image != VK_NULL_HANDLE)
        interpolationSource = &dispatchInfo->hudlessColor;
    if (!validFrameGenerationSource(interpolationSource, context->createInfo.displaySize,
                                    context->createInfo.interpolationSourceFormat) ||
        interpolationSource->image != context->preparedInterpolationSource ||
        !validFrameGenerationSource(&dispatchInfo->currentColor, context->createInfo.displaySize,
                                    context->createInfo.interpolationSourceFormat) ||
        !validFrameGenerationOutput(&dispatchInfo->output, context->createInfo.displaySize,
                                    context->createInfo.outputFormat))
        return FFX_VK_PORTABLE_ERROR_INVALID_ARGUMENT;

    FfxFrameInterpolationDispatchDescription dispatch{};
    dispatch.commandList = ffxGetCommandListVK(dispatchInfo->commandBuffer);
    dispatch.displaySize = {dispatchInfo->displaySize.width, dispatchInfo->displaySize.height};
    dispatch.renderSize = {context->preparedRenderSize.width, context->preparedRenderSize.height};
    dispatch.currentBackBuffer = externalResource(dispatchInfo->currentColor, L"portable FSR3 FI present color");
    dispatch.currentBackBuffer_HUDLess = externalResource(
        dispatchInfo->hudlessColor, L"portable FSR3 FI HUDless color");
    dispatch.output = externalResource(dispatchInfo->output, L"portable FSR3 FI output");
    dispatch.interpolationRect.left = dispatchInfo->interpolationRect.x;
    dispatch.interpolationRect.top = dispatchInfo->interpolationRect.y;
    dispatch.interpolationRect.width = static_cast<int32_t>(dispatchInfo->interpolationRect.width);
    dispatch.interpolationRect.height = static_cast<int32_t>(dispatchInfo->interpolationRect.height);
    dispatch.opticalFlowVector = ownedResource(context->opticalFlowVector, false);
    dispatch.opticalFlowSceneChangeDetection = ownedResource(context->opticalFlowScd, false);
    dispatch.opticalFlowBufferSize = {
        context->opticalFlowVector.description.width, context->opticalFlowVector.description.height};
    dispatch.opticalFlowScale = {
        1.0f / static_cast<float>(dispatchInfo->displaySize.width),
        1.0f / static_cast<float>(dispatchInfo->displaySize.height)};
    dispatch.opticalFlowBlockSize = 8;
    dispatch.frameTimeDelta = dispatchInfo->frameTimeMilliseconds;
    dispatch.reset = dispatchInfo->reset != VK_FALSE || context->preparedReset != VK_FALSE;
    dispatch.cameraNear = dispatchInfo->cameraNear;
    dispatch.cameraFar = dispatchInfo->cameraFar;
    dispatch.cameraFovAngleVertical = dispatchInfo->cameraVerticalFovRadians;
    dispatch.viewSpaceToMetersFactor = dispatchInfo->viewSpaceToMeters;
    dispatch.backBufferTransferFunction = toFfxTransferFunction(dispatchInfo->transferFunction);
    dispatch.minMaxLuminance[0] = dispatchInfo->minLuminance;
    dispatch.minMaxLuminance[1] = dispatchInfo->maxLuminance;
    dispatch.frameID = dispatchInfo->frameId;
    dispatch.dilatedDepth = ownedResource(context->dilatedDepth, false);
    dispatch.dilatedMotionVectors = ownedResource(context->dilatedMotionVectors, false);
    dispatch.reconstructedPrevDepth = ownedResource(context->reconstructedPreviousDepth, false);
    dispatch.distortionField = externalResource(
        dispatchInfo->distortionField, L"portable FSR3 FI distortion field");
    const FfxErrorCode ffxResult = ffxFrameInterpolationDispatch(
        &context->interpolationContext, &dispatch);
    if (ffxResult == FFX_OK)
        context->dispatched = true;
    return fromFfxResult(ffxResult);
}

extern "C" FfxVkPortableResult ffxVkPortableFrameGenerationContextDestroy(
    FfxVkPortableFrameGenerationContext* context)
{
    if (context == nullptr)
        return FFX_VK_PORTABLE_OK;
    if (context->magic != kFrameGenerationContextMagic)
        return FFX_VK_PORTABLE_ERROR_INVALID_ARGUMENT;
    FfxErrorCode ffxResult = FFX_OK;
    {
        std::lock_guard<std::mutex> lock(context->mutex);
        context->magic = 0;
        if (context->interpolationCreated)
            ffxResult = ffxFrameInterpolationContextDestroy(&context->interpolationContext);
        context->interpolationCreated = false;
        if (context->opticalFlowCreated) {
            const FfxErrorCode opticalFlowResult = ffxOpticalflowContextDestroy(&context->opticalFlowContext);
            if (ffxResult == FFX_OK)
                ffxResult = opticalFlowResult;
        }
        context->opticalFlowCreated = false;
        destroyOwnedImage(context->deviceInfo, &context->opticalFlowVector);
        destroyOwnedImage(context->deviceInfo, &context->opticalFlowScd);
        destroyOwnedImage(context->deviceInfo, &context->dilatedDepth);
        destroyOwnedImage(context->deviceInfo, &context->dilatedMotionVectors);
        destroyOwnedImage(context->deviceInfo, &context->reconstructedPreviousDepth);
        destroyBackendState(&context->interpolationBackend);
        destroyBackendState(&context->opticalFlowBackend);
    }
    delete context;
    return fromFfxResult(ffxResult);
}
#endif /* FFX_VK_PORTABLE_HAS_FSR3_FRAMEGEN */
