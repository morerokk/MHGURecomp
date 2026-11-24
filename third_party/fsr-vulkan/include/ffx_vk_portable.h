/*
 * Copyright (c) 2026 Q2RTX FSR Vulkan contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef FFX_VK_PORTABLE_H
#define FFX_VK_PORTABLE_H

#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#if defined(__cplusplus)
extern "C" {
#endif

#define FFX_VK_PORTABLE_ABI_VERSION 2u

typedef enum FfxVkPortableResult {
    FFX_VK_PORTABLE_OK = 0,
    FFX_VK_PORTABLE_ERROR_INVALID_POINTER = -1,
    FFX_VK_PORTABLE_ERROR_INVALID_STRUCT_SIZE = -2,
    FFX_VK_PORTABLE_ERROR_VULKAN = -3,
    FFX_VK_PORTABLE_ERROR_INVALID_ARGUMENT = -4,
    FFX_VK_PORTABLE_ERROR_OUT_OF_MEMORY = -5,
    FFX_VK_PORTABLE_ERROR_UNSUPPORTED = -6,
    FFX_VK_PORTABLE_ERROR_BACKEND = -7
} FfxVkPortableResult;

/* (MHGU Recomp) Where the wrapper's diagnostics go: FidelityFX's own messages (FFX_VK_PORTABLE_CONTEXT_DEBUG_CHECKING),
 * the source file and line of each FFX_ERROR_BACKEND_API_ERROR, the FidelityFX error code behind a failed call, a pass
 * shader that could not be built. NULL (the default): nowhere. Called from any thread (shaders build on their own). */
typedef void (*FfxVkPortableLogFn)(const char* message);
void ffxVkPortableSetLog(FfxVkPortableLogFn fn);

typedef struct FfxVkPortableUpscaleContext FfxVkPortableUpscaleContext;
typedef struct FfxVkPortableFrameGenerationContext FfxVkPortableFrameGenerationContext;

/* SDK-reported memory owned by the FSR effect. Application-provided color,
 * depth, motion, and output images are deliberately excluded. */
typedef struct FfxVkPortableMemoryUsage {
    uint32_t structSize;
    uint64_t totalUsageInBytes;
    uint64_t aliasableUsageInBytes;
} FfxVkPortableMemoryUsage;

typedef enum FfxVkPortableResourceState {
    FFX_VK_PORTABLE_RESOURCE_STATE_UNDEFINED = 0,
    FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ,
    FFX_VK_PORTABLE_RESOURCE_STATE_COMPUTE_READ,
    FFX_VK_PORTABLE_RESOURCE_STATE_UNORDERED_ACCESS,
    FFX_VK_PORTABLE_RESOURCE_STATE_TRANSFER_SOURCE,
    FFX_VK_PORTABLE_RESOURCE_STATE_TRANSFER_DESTINATION,
    FFX_VK_PORTABLE_RESOURCE_STATE_COLOR_ATTACHMENT,
    FFX_VK_PORTABLE_RESOURCE_STATE_DEPTH_ATTACHMENT,
    FFX_VK_PORTABLE_RESOURCE_STATE_PRESENT
} FfxVkPortableResourceState;

typedef enum FfxVkPortableContextFlagBits {
    FFX_VK_PORTABLE_CONTEXT_HDR_COLOR_INPUT = 1u << 0,
    FFX_VK_PORTABLE_CONTEXT_DISPLAY_RESOLUTION_MOTION_VECTORS = 1u << 1,
    FFX_VK_PORTABLE_CONTEXT_JITTERED_MOTION_VECTORS = 1u << 2,
    FFX_VK_PORTABLE_CONTEXT_DEPTH_INVERTED = 1u << 3,
    FFX_VK_PORTABLE_CONTEXT_DEPTH_INFINITE = 1u << 4,
    FFX_VK_PORTABLE_CONTEXT_AUTO_EXPOSURE = 1u << 5,
    FFX_VK_PORTABLE_CONTEXT_DYNAMIC_RESOLUTION = 1u << 6,
    FFX_VK_PORTABLE_CONTEXT_DEBUG_CHECKING = 1u << 7
} FfxVkPortableContextFlagBits;

typedef enum FfxVkPortableValidationIssueBits {
    FFX_VK_PORTABLE_VALIDATION_NONE = 0,
    FFX_VK_PORTABLE_VALIDATION_STRUCT_SIZE = 1ull << 0,
    FFX_VK_PORTABLE_VALIDATION_NULL_HANDLE = 1ull << 1,
    FFX_VK_PORTABLE_VALIDATION_ZERO_EXTENT = 1ull << 2,
    FFX_VK_PORTABLE_VALIDATION_RENDER_SIZE_EXCEEDS_MAXIMUM = 1ull << 3,
    FFX_VK_PORTABLE_VALIDATION_OUTPUT_SIZE_EXCEEDS_MAXIMUM = 1ull << 4,
    FFX_VK_PORTABLE_VALIDATION_RESOURCE_TOO_SMALL = 1ull << 5,
    FFX_VK_PORTABLE_VALIDATION_RESOURCE_FORMAT_UNDEFINED = 1ull << 6,
    FFX_VK_PORTABLE_VALIDATION_RESOURCE_USAGE = 1ull << 7,
    FFX_VK_PORTABLE_VALIDATION_NONFINITE_VALUE = 1ull << 8,
    FFX_VK_PORTABLE_VALIDATION_FRAME_TIME = 1ull << 9,
    FFX_VK_PORTABLE_VALIDATION_PRE_EXPOSURE = 1ull << 10,
    FFX_VK_PORTABLE_VALIDATION_SHARPNESS = 1ull << 11,
    FFX_VK_PORTABLE_VALIDATION_CAMERA = 1ull << 12,
    FFX_VK_PORTABLE_VALIDATION_INTERPOLATION_RECT = 1ull << 13,
    FFX_VK_PORTABLE_VALIDATION_LUMINANCE_RANGE = 1ull << 14
} FfxVkPortableValidationIssueBits;

typedef struct FfxVkPortableExtent2D {
    uint32_t width;
    uint32_t height;
} FfxVkPortableExtent2D;

typedef struct FfxVkPortableFloat2 {
    float x;
    float y;
} FfxVkPortableFloat2;

typedef struct FfxVkPortableFloat3 {
    float x;
    float y;
    float z;
} FfxVkPortableFloat3;

typedef struct FfxVkPortableRect2D {
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
} FfxVkPortableRect2D;

typedef struct FfxVkPortableImage {
    uint32_t structSize;
    VkImage image;
    VkFormat format;
    FfxVkPortableExtent2D extent;
    uint32_t mipCount;
    uint32_t arrayLayers;
    VkImageUsageFlags usage;
    VkImageAspectFlags aspect;
    FfxVkPortableResourceState state;
} FfxVkPortableImage;

/* Application-owned buffer metadata for provider-neutral contracts. The
 * contract never maps, allocates, or frees this buffer; callers retain those
 * responsibilities and keep it alive through submitted GPU work. */
typedef struct FfxVkPortableBuffer {
    uint32_t structSize;
    VkBuffer buffer;
    VkDeviceSize size;
    VkBufferUsageFlags usage;
    FfxVkPortableResourceState state;
} FfxVkPortableBuffer;

/*
 * Application-owned Vulkan objects used by a portable effect context.
 *
 * The upscaler currently requires physicalDevice, device, and
 * getDeviceProcAddr.  Instance and queue metadata are retained in this common
 * device contract for the frame-generation presenter; record-only upscaling
 * does not submit to queue.  allocationCallbacks may be NULL, but when it is
 * non-NULL the pointed-to callbacks must outlive the effect context.
 */
typedef struct FfxVkPortableDeviceInfo {
    uint32_t structSize;
    VkInstance instance;
    VkPhysicalDevice physicalDevice;
    VkDevice device;
    PFN_vkGetDeviceProcAddr getDeviceProcAddr;
    const VkAllocationCallbacks* allocationCallbacks;
    VkQueue queue;
    uint32_t queueFamilyIndex;
    /*
     * Set to VK_TRUE only if deviceCoherentMemory was explicitly enabled in
     * VkPhysicalDeviceCoherentMemoryFeaturesAMD when device was created.
     * The default VK_FALSE excludes memory types which Vulkan forbids without
     * that logical-device feature.
     */
    VkBool32 deviceCoherentMemoryEnabled;
    /* Optional Vulkan features/extensions actually enabled by the owner. */
    VkBool32 shaderFloat16Enabled;
    VkBool32 subgroupSizeControlEnabled;
    VkBool32 computeFullSubgroupsEnabled;
    VkBool32 synchronization2Enabled;
    VkBool32 bufferMarkerEnabled;
    VkBool32 debugUtilsEnabled;
    VkBool32 shaderStorageBufferArrayNonUniformIndexingEnabled;
    VkBool32 accelerationStructureEnabled;
    /* Required by the checked FSR3 accumulate shaders. Physical-device
     * support is insufficient: this must have been enabled in the owner's
     * VkPhysicalDeviceFeatures chain when creating the logical device. */
    VkBool32 shaderStorageImageWriteWithoutFormatEnabled;
} FfxVkPortableDeviceInfo;

typedef struct FfxVkPortableUpscaleCreateInfo {
    uint32_t structSize;
    uint32_t flags;
    FfxVkPortableExtent2D maxRenderSize;
    FfxVkPortableExtent2D maxOutputSize;
} FfxVkPortableUpscaleCreateInfo;

typedef struct FfxVkPortableUpscaleDispatchInfo {
    uint32_t structSize;
    VkCommandBuffer commandBuffer;
    FfxVkPortableImage color;
    FfxVkPortableImage depth;
    FfxVkPortableImage motionVectors;
    FfxVkPortableImage exposure;
    FfxVkPortableImage reactiveMask;
    FfxVkPortableImage transparencyAndCompositionMask;
    FfxVkPortableImage output;
    FfxVkPortableFloat2 jitterOffset;
    FfxVkPortableFloat2 motionVectorScale;
    FfxVkPortableExtent2D renderSize;
    FfxVkPortableExtent2D outputSize;
    float frameTimeMilliseconds;
    float preExposure;
    float cameraNear;
    float cameraFar;
    float cameraVerticalFovRadians;
    float viewSpaceToMeters;
    float sharpness;
    VkBool32 enableSharpening;
    VkBool32 reset;
    uint64_t frameId;
} FfxVkPortableUpscaleDispatchInfo;

typedef struct FfxVkPortableFrameGenerationCreateInfo {
    uint32_t structSize;
    uint32_t flags;
    FfxVkPortableExtent2D maxRenderSize;
    FfxVkPortableExtent2D displaySize;
    VkFormat interpolationSourceFormat;
    VkFormat outputFormat;
} FfxVkPortableFrameGenerationCreateInfo;

typedef enum FfxVkPortableTransferFunction {
    FFX_VK_PORTABLE_TRANSFER_FUNCTION_SRGB = 0,
    FFX_VK_PORTABLE_TRANSFER_FUNCTION_PQ,
    FFX_VK_PORTABLE_TRANSFER_FUNCTION_SCRGB
} FfxVkPortableTransferFunction;

typedef struct FfxVkPortableFrameGenerationPrepareInfo {
    uint32_t structSize;
    VkCommandBuffer commandBuffer;
    FfxVkPortableImage depth;
    FfxVkPortableImage motionVectors;
    FfxVkPortableExtent2D renderSize;
    FfxVkPortableFloat2 jitterOffset;
    FfxVkPortableFloat2 motionVectorScale;
    float frameTimeMilliseconds;
    float cameraNear;
    float cameraFar;
    float cameraVerticalFovRadians;
    float viewSpaceToMeters;
    float minLuminance;
    float maxLuminance;
    FfxVkPortableTransferFunction transferFunction;
    FfxVkPortableFloat3 cameraPosition;
    FfxVkPortableFloat3 cameraUp;
    FfxVkPortableFloat3 cameraRight;
    FfxVkPortableFloat3 cameraForward;
    VkBool32 reset;
    uint64_t frameId;
} FfxVkPortableFrameGenerationPrepareInfo;

typedef struct FfxVkPortableFrameGenerationDispatchInfo {
    uint32_t structSize;
    VkCommandBuffer commandBuffer;
    FfxVkPortableImage currentColor;
    FfxVkPortableImage hudlessColor;
    FfxVkPortableImage distortionField;
    FfxVkPortableImage output;
    FfxVkPortableExtent2D displaySize;
    FfxVkPortableRect2D interpolationRect;
    float frameTimeMilliseconds;
    float cameraNear;
    float cameraFar;
    float cameraVerticalFovRadians;
    float viewSpaceToMeters;
    float minLuminance;
    float maxLuminance;
    FfxVkPortableTransferFunction transferFunction;
    VkBool32 reset;
    uint64_t frameId;
} FfxVkPortableFrameGenerationDispatchInfo;

typedef struct FfxVkPortableDeviceCapabilities {
    uint32_t structSize;
    uint32_t apiVersion;
    uint32_t driverVersion;
    uint32_t vendorId;
    uint32_t deviceId;
    uint32_t subgroupSize;
    uint32_t minSubgroupSize;
    uint32_t maxSubgroupSize;
    VkBool32 hasComputeQueue;
    VkBool32 subgroupOperations;
    VkBool32 subgroupSizeControl;
    VkBool32 computeFullSubgroups;
    VkBool32 shaderFloat16;
    VkBool32 shaderInt8;
    VkBool32 storageBuffer8BitAccess;
    VkBool32 uniformAndStorageBuffer8BitAccess;
    VkBool32 shaderIntegerDotProduct;
    VkBool32 acceleratedSignedInt8DotProduct;
    VkBool32 timelineSemaphore;
    VkBool32 synchronization2;
    VkBool32 requiredFsr3StorageFormats;
    VkBool32 fsr3ComputePrerequisites;
    VkBool32 fsr3FrameGenerationPrerequisites;
    VkBool32 fsr4Int8Prerequisites;
} FfxVkPortableDeviceCapabilities;

FfxVkPortableResult ffxVkPortableQueryDeviceCapabilities(
    VkPhysicalDevice physicalDevice,
    FfxVkPortableDeviceCapabilities* capabilities);

uint64_t ffxVkPortableValidateUpscaleCreateInfo(
    const FfxVkPortableUpscaleCreateInfo* createInfo);

uint64_t ffxVkPortableValidateUpscaleDispatchInfo(
    const FfxVkPortableUpscaleCreateInfo* createInfo,
    const FfxVkPortableUpscaleDispatchInfo* dispatchInfo);

/*
 * Create the native Vulkan FSR 3.1.4 upscaler implementation.
 *
 * The returned context owns its FidelityFX backend, temporal resources, and
 * shared dilated-depth/motion-vector resources.  It never owns any handle in
 * deviceInfo.  *context is set to NULL before creation is attempted.
 */
FfxVkPortableResult ffxVkPortableUpscaleContextCreate(
    const FfxVkPortableDeviceInfo* deviceInfo,
    const FfxVkPortableUpscaleCreateInfo* createInfo,
    FfxVkPortableUpscaleContext** context);

FfxVkPortableResult ffxVkPortableUpscaleContextGetMemoryUsage(
    FfxVkPortableUpscaleContext* context,
    FfxVkPortableMemoryUsage* usage);

/*
 * Record one upscaler dispatch into an already-recording primary or secondary
 * command buffer.  No queue submission or wait is performed.  Calls for a
 * single context must be externally serialized and submitted in record order.
 */
FfxVkPortableResult ffxVkPortableUpscaleContextRecordDispatch(
    FfxVkPortableUpscaleContext* context,
    const FfxVkPortableUpscaleDispatchInfo* dispatchInfo);

/*
 * Destroy a context.  NULL is accepted.  The application must first ensure
 * that no submitted command buffer can still reference this context's
 * resources (normally by waiting for the relevant frame fences).
 */
FfxVkPortableResult ffxVkPortableUpscaleContextDestroy(
    FfxVkPortableUpscaleContext* context);

/*
 * Create the native analytical FSR3 1.1.3 Frame Interpolation and 1.1.2
 * Optical Flow contexts.  The portable context owns optical-flow outputs and
 * the FI dilated-depth/motion-vector resources; all scene/presentation images
 * passed to Prepare/RecordDispatch remain application-owned.
 */
FfxVkPortableResult ffxVkPortableFrameGenerationContextCreate(
    const FfxVkPortableDeviceInfo* deviceInfo,
    const FfxVkPortableFrameGenerationCreateInfo* createInfo,
    FfxVkPortableFrameGenerationContext** context);

/*
 * Record OF plus FI prepare work for one rendered (real) frame.
 *
 * interpolationSource is the scene colour that optical flow observes.  It
 * must be the same image selected for interpolation in RecordDispatch: pass
 * it as currentColor when no HUDless image is used, or as hudlessColor when
 * UI is composed separately.  This explicit rule prevents OF and FI from
 * observing different frames.
 */
FfxVkPortableResult ffxVkPortableFrameGenerationContextPrepare(
    FfxVkPortableFrameGenerationContext* context,
    const FfxVkPortableFrameGenerationPrepareInfo* prepareInfo,
    const FfxVkPortableImage* interpolationSource);

/* Record the interpolated frame after Prepare.  This never presents or submits. */
FfxVkPortableResult ffxVkPortableFrameGenerationContextRecordDispatch(
    FfxVkPortableFrameGenerationContext* context,
    const FfxVkPortableFrameGenerationDispatchInfo* dispatchInfo);

/* The application must wait for all work referencing the context before destroying it. */
FfxVkPortableResult ffxVkPortableFrameGenerationContextDestroy(
    FfxVkPortableFrameGenerationContext* context);

uint64_t ffxVkPortableValidateFrameGenerationCreateInfo(
    const FfxVkPortableFrameGenerationCreateInfo* createInfo);

uint64_t ffxVkPortableValidateFrameGenerationPrepareInfo(
    const FfxVkPortableFrameGenerationCreateInfo* createInfo,
    const FfxVkPortableFrameGenerationPrepareInfo* prepareInfo);

uint64_t ffxVkPortableValidateFrameGenerationDispatchInfo(
    const FfxVkPortableFrameGenerationCreateInfo* createInfo,
    const FfxVkPortableFrameGenerationDispatchInfo* dispatchInfo);

const char* ffxVkPortableValidationIssueName(uint64_t singleIssueBit);

#if defined(__cplusplus)
}
#endif

#endif
