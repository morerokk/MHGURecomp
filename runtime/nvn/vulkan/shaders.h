// Shader compilation and reflection for the Vulkan backend (shaders.c).
#pragma once
#include <stddef.h>

#include "vk.h"

typedef struct { uint32_t set, binding; VkDescriptorType type; } VkBindingInfo;

void vk_shaders_init(void);
uint32_t *vk_glsl_to_spirv(const char *src, int stage, const char *name, size_t *nwords);  // malloc'd, NULL on error
// ...with -D macros ("NAME" or "NAME=value") and an #include resolver (the text of a name, or NULL); salt stands for
// what the includes contain (the disk cache key); opt 1: spirv-opt at the size level, which removes the OpNames.
// Compute shaders only.
typedef const char *(*VkGlslInclude)(const char *name, size_t *len);
uint32_t *vk_glsl_to_spirv_ex(const char *src, size_t srclen, const char *name, const char *const *defs, int ndefs,
                              VkGlslInclude inc, uint64_t salt, int opt, size_t *nwords);
// An HLSL compute shader (from entry), with -D macros; its t# registers bind at #, its u# at uav_base + #.
uint32_t *vk_hlsl_to_spirv(const char *src, size_t srclen, const char *entry, const char *name, const char *const *defs,
                           int ndefs, int uav_base, size_t *nwords);
VkShaderModule vk_module(const uint32_t *spv, size_t nwords);
// Descriptor bindings declared by a SPIR-V module.
int vk_reflect(const uint32_t *spv, size_t nwords, VkBindingInfo *out, int max);
VkDescriptorSetLayout vk_set_layout(const VkBindingInfo *b, int n, uint32_t set, VkShaderStageFlags stages);
