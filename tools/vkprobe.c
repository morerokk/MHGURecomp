// Prints the Vulkan devices and the features the renderer cares about (a tiny vulkaninfo).
#include <stdio.h>
#include <string.h>
#include <vulkan/vulkan.h>

int main(void) {
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkprobe", 1, NULL, 0, VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
#ifdef __APPLE__
    // MoltenVK is a "portability" driver: the loader lists it only for an instance that asks for those.
    const char *iext[] = {VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME};
    ici.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    ici.enabledExtensionCount = 1;
    ici.ppEnabledExtensionNames = iext;
#endif
    VkInstance inst;
    VkResult r = vkCreateInstance(&ici, NULL, &inst);
    if (r) { printf("vkCreateInstance: %d\n", r); return 1; }
    uint32_t n = 8;
    VkPhysicalDevice devs[8];
    vkEnumeratePhysicalDevices(inst, &n, devs);
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devs[i], &p);
        printf("device %u: %s api %u.%u.%u driver %x type %d\n", i, p.deviceName, VK_VERSION_MAJOR(p.apiVersion),
               VK_VERSION_MINOR(p.apiVersion), VK_VERSION_PATCH(p.apiVersion), p.driverVersion, p.deviceType);
        uint32_t ne = 0;
        vkEnumerateDeviceExtensionProperties(devs[i], NULL, &ne, NULL);
        VkExtensionProperties ext[512];
        if (ne > 512) ne = 512;
        vkEnumerateDeviceExtensionProperties(devs[i], NULL, &ne, ext);
        const char *want[] = {"VK_KHR_dynamic_rendering", "VK_KHR_synchronization2", "VK_EXT_extended_dynamic_state",
                              "VK_EXT_extended_dynamic_state2", "VK_EXT_extended_dynamic_state3",
                              "VK_KHR_maintenance4", "VK_EXT_custom_border_color", "VK_EXT_vertex_input_dynamic_state",
                              "VK_KHR_push_descriptor", "VK_EXT_descriptor_indexing", "VK_KHR_swapchain",
                              "VK_EXT_depth_clip_enable", "VK_EXT_shader_stencil_export",
                              "VK_EXT_shader_demote_to_helper_invocation", "VK_KHR_shader_float16_int8",
                              "VK_EXT_index_type_uint8", "VK_EXT_robustness2", "VK_EXT_provoking_vertex",
                              "VK_EXT_line_rasterization", "VK_KHR_timeline_semaphore", "VK_EXT_external_memory_host"};
        for (unsigned k = 0; k < sizeof want / sizeof *want; k++) {
            int have = 0;
            for (uint32_t e = 0; e < ne; e++) have |= !strcmp(ext[e].extensionName, want[k]);
            printf("  %-44s %s\n", want[k], have ? "yes" : "-");
        }
        printf("  (%u extensions)\n", ne);
        VkPhysicalDeviceFeatures f;
        vkGetPhysicalDeviceFeatures(devs[i], &f);
        printf("  geometryShader %d tessellation %d logicOp %d depthClamp %d fillModeNonSolid %d wideLines %d "
               "textureCompressionBC %d ETC2 %d ASTC %d independentBlend %d dualSrcBlend %d multiViewport %d\n",
               f.geometryShader, f.tessellationShader, f.logicOp, f.depthClamp, f.fillModeNonSolid, f.wideLines,
               f.textureCompressionBC, f.textureCompressionETC2, f.textureCompressionASTC_LDR, f.independentBlend,
               f.dualSrcBlend, f.multiViewport);
        printf("  maxPushConstants %u maxUBO %u maxBoundSets %u maxPerStageSamplers %u maxColorAttachments %u\n",
               p.limits.maxPushConstantsSize, p.limits.maxUniformBufferRange, p.limits.maxBoundDescriptorSets,
               p.limits.maxPerStageDescriptorSamplers, p.limits.maxColorAttachments);
        printf("  shaderInt64 %d robustBufferAccess %d imageCubeArray %d clipDistance %d fragStores %d vtxStores %d "
               "anisotropy %d depthBiasClamp %d sampleRateShading %d largePoints %d\n", f.shaderInt64,
               f.robustBufferAccess, f.imageCubeArray, f.shaderClipDistance, f.fragmentStoresAndAtomics,
               f.vertexPipelineStoresAndAtomics, f.samplerAnisotropy, f.depthBiasClamp, f.sampleRateShading,
               f.largePoints);
        VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &f12};
        VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f11};
        vkGetPhysicalDeviceFeatures2(devs[i], &f2);
        printf("  1.2: scalarBlockLayout %d shaderInt8 %d storage8 %d uniformStorage8 %d float16 %d mirrorClamp %d "
               "timeline %d descIndexing %d bufferDeviceAddress %d hostQueryReset %d | 1.1: storage16 %d\n",
               f12.scalarBlockLayout, f12.shaderInt8, f12.storageBuffer8BitAccess, f12.uniformAndStorageBuffer8BitAccess,
               f12.shaderFloat16, f12.samplerMirrorClampToEdge, f12.timelineSemaphore, f12.descriptorIndexing,
               f12.bufferDeviceAddress, f12.hostQueryReset, f11.storageBuffer16BitAccess);
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(devs[i], &mp);
        for (uint32_t h = 0; h < mp.memoryHeapCount; h++)
            printf("  heap %u: %llu MB%s\n", h, (unsigned long long)(mp.memoryHeaps[h].size >> 20),
                   mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ? " (device local)" : "");
        for (uint32_t e = 0; e < ne; e++) printf("    %s\n", ext[e].extensionName);
    }
    return 0;
}
