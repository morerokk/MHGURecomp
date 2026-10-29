// CMAA2 ([Anti-aliasing] AntiAliasing = CMAA2): Intel's Conservative Morphological Anti-Aliasing 2 (third_party/cmaa2,
// Apache-2.0) in place of the game's FXAA pass (exec.c fxaa_replaced). CMAA2.hlsl's compute kernels, compiled from the
// HLSL by shaderc (shaders.c vk_hlsl_to_spirv), run as Intel's DX12 code runs them (vaCMAA2DX12.cpp), a barrier after
// each: the edges and shape candidates of the input (EdgesColor2x2CS, luma edges), the indirect dispatch's arguments
// (ComputeDispatchArgsCS), the candidates' shapes and blends (ProcessCandidatesCS, indirect), the arguments again, and
// the blends applied (DeferredColorApply2x2CS, indirect). The second argument pass also clears the counters for the next
// frame; the edge pass resets the blend lists' heads.
//  - CMAA2 writes only the pixels it changes, through a storage image. The game's targets are not storage images (that
//    could cost them their compression), so the input is copied into an RGBA8 image of ours, written through an
//    R32_UINT view (CMAA2's untyped store, R8G8B8A8), and that is copied into the game's target.
//  - CMAA2Quality: CMAA2's presets (edge threshold Low 0.15, Medium 0.10, High 0.07, Ultra 0.05); CMAA2ExtraSharpness:
//    its variant that keeps more of text and fine detail.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../rt.h"
#include "shaders.h"
#include "vk.h"

// CMAA2.hlsl as data in the executable (the Makefile has third_party/cmaa2/CMAA2.hlsl as a prerequisite)
__asm__(".pushsection .rodata\n.globl cmaa2_hlsl\ncmaa2_hlsl:\n.incbin \"third_party/cmaa2/CMAA2.hlsl\"\n"
        ".globl cmaa2_hlsl_end\ncmaa2_hlsl_end:\n.byte 0\n.popsection");
extern const char cmaa2_hlsl[], cmaa2_hlsl_end[];

enum { EDGES, ARGS, CANDIDATES, APPLY, NKERNEL };
static const char *const entry[NKERNEL] = {"EdgesColor2x2CS", "ComputeDispatchArgsCS", "ProcessCandidatesCS",
                                           "DeferredColorApply2x2CS"};
// Bindings (shaderc's HLSL register mapping, u# from UAV_BASE): t0 the input; u0 the output, u1 the edges, u2 the shape
// candidates, u3 the blend locations, u4 the blend items, u5 the blend lists' heads, u6 the counters, u7 the indirect
// dispatch's arguments.
#define UAV_BASE 8
enum { B_INPUT = 0, B_OUTPUT = UAV_BASE, B_EDGES, B_CANDIDATES, B_LOCATIONS, B_ITEMS, B_HEADS, B_CONTROL, B_ARGS };

typedef struct { VkImage image; VmaAllocation mem; VkImageView view; } Img;
typedef struct { VkBuffer buf; VmaAllocation mem; VkDeviceSize size; } Buf;

// What one target size needs (made again when it changes): the working image and its R32_UINT view, CMAA2's working
// storage (Intel's sizes: candidates for 1/4 of the pixels, blend items for 1/2, list heads for 1/6).
static struct {
    uint32_t w, h;
    Img work, edges, heads;
    VkImageView work_u32;
    Buf candidates, locations, items, control, args;
} R;
static VkDescriptorSetLayout set_layout;
static VkPipelineLayout layout;
static VkPipeline pipes[4][2][NKERNEL];  // [quality][extra sharpness][kernel]
static bool tried[4][2];

static bool image(Img *m, VkFormat f, uint32_t w, uint32_t h, VkImageUsageFlags usage, VkImageCreateFlags flags) {
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = flags, .imageType = VK_IMAGE_TYPE_2D,
                            .format = f, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = usage | VK_IMAGE_USAGE_STORAGE_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(g_vk.vma, &ci, &ai, &m->image, &m->mem, NULL) != VK_SUCCESS) return false;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = m->image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = f, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(g_vk.dev, &vi, NULL, &m->view) != VK_SUCCESS) {
        vk_defer_image(m->image, m->mem);
        m->image = VK_NULL_HANDLE;
        return false;
    }
    vk_image_init_layout(vk_upload_cmd(), m->image, VK_IMAGE_ASPECT_COLOR_BIT);
    return true;
}

static bool buffer(Buf *b, VkDeviceSize size, VkBufferUsageFlags usage) {
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                             .usage = usage | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    b->size = size;
    return vmaCreateBuffer(g_vk.vma, &bi, &ai, &b->buf, &b->mem, NULL) == VK_SUCCESS;
}

static void release(void) {
    Img *imgs[] = {&R.work, &R.edges, &R.heads};
    for (int i = 0; i < 3; i++)
        if (imgs[i]->image) vk_defer_view(imgs[i]->view), vk_defer_image(imgs[i]->image, imgs[i]->mem);
    if (R.work_u32) vk_defer_view(R.work_u32);
    Buf *bufs[] = {&R.candidates, &R.locations, &R.items, &R.control, &R.args};
    for (int i = 0; i < 5; i++)
        if (bufs[i]->buf) vk_defer_buffer(bufs[i]->buf, bufs[i]->mem);
    memset(&R, 0, sizeof R);
}

static bool resources(uint32_t w, uint32_t h) {
    if (R.w == w && R.h == h) return true;
    release();
    uint64_t px = (uint64_t)w * h;
    bool ok = image(&R.work, VK_FORMAT_R8G8B8A8_UNORM, w, h, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) &&
              image(&R.edges, VK_FORMAT_R32_UINT, (w + 1) / 2, h, 0, 0) &&  // (two pixels' 4-bit edges a texel)
              image(&R.heads, VK_FORMAT_R32_UINT, (w + 1) / 2, (h + 1) / 2, 0, 0) &&
              buffer(&R.candidates, px / 4 * 4 + 4, 0) && buffer(&R.items, px / 2 * 8 + 8, 0) &&
              buffer(&R.locations, (px + 3) / 6 * 4 + 4, 0) && buffer(&R.control, 16 * 4, 0) &&
              buffer(&R.args, 4 * 4, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
    if (ok) {
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = R.work.image,
                                    .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R32_UINT,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        ok = vkCreateImageView(g_vk.dev, &vi, NULL, &R.work_u32) == VK_SUCCESS;
    }
    if (!ok) {
        LOG("[vk] CMAA2: no working storage for %ux%u\n", w, h);
        release();
        return false;
    }
    VkCommandBuffer cb = vk_upload_cmd();
    vkCmdFillBuffer(cb, R.control.buf, 0, VK_WHOLE_SIZE, 0);  // (the counters start at 0; CMAA2 clears them after)
    vk_full_barrier(cb);
    R.w = w, R.h = h;
    return true;
}

// The four kernels of a preset (compiled the first time it is picked; the SPIR-V is cached on disk like the rest).
static const VkPipeline *pipelines(int quality, bool sharp) {
    VkPipeline *p = pipes[quality][sharp];
    if (tried[quality][sharp]) return p[NKERNEL - 1] ? p : NULL;
    tried[quality][sharp] = true;
    if (!layout) {
        static const VkDescriptorType type[] = {
            [B_INPUT] = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,       [B_OUTPUT] = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            [B_EDGES] = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,       [B_CANDIDATES] = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            [B_LOCATIONS] = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  [B_ITEMS] = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            [B_HEADS] = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,       [B_CONTROL] = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            [B_ARGS] = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
        VkDescriptorSetLayoutBinding b[9];
        uint32_t n = 0;
        for (uint32_t i = 0; i <= B_ARGS; i++)
            if (i == B_INPUT || i >= B_OUTPUT) b[n++] = (VkDescriptorSetLayoutBinding){i, type[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = n, .pBindings = b};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &set_layout));
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                         .pSetLayouts = &set_layout};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &layout));
    }
    char q[40], s[40];
    snprintf(q, sizeof q, "CMAA2_STATIC_QUALITY_PRESET=%d", quality);
    snprintf(s, sizeof s, "CMAA2_EXTRA_SHARPNESS=%d", sharp);
    // untyped stores into R32_UINT, packed as R8G8B8A8 (the input is gamma-space RGBA8 already: no sRGB conversion)
    const char *defs[] = {q, s, "CMAA2_UAV_STORE_TYPED=0", "CMAA2_UAV_STORE_UNTYPED_FORMAT=1", "CMAA2_UAV_STORE_CONVERT_TO_SRGB=0",
                          "CMAA2_UAV_STORE_TYPED_UNORM_FLOAT=1", "CMAA2_EDGE_DETECTION_LUMA_PATH=1"};
    for (int k = 0; k < NKERNEL; k++) {
        char name[64];
        snprintf(name, sizeof name, "cmaa2.%s.q%d%s", entry[k], quality, sharp ? ".sharp" : "");
        size_t nw;
        uint32_t *spv = vk_hlsl_to_spirv(cmaa2_hlsl, (size_t)(cmaa2_hlsl_end - cmaa2_hlsl), entry[k], name, defs,
                                         (int)(sizeof defs / sizeof *defs), UAV_BASE, &nw);
        VkShaderModule m = spv ? vk_module(spv, nw) : VK_NULL_HANDLE;
        free(spv);
        if (m) {
            VkComputePipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                              .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                        .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m,
                                                        .pName = entry[k]},  // (the SPIR-V keeps the HLSL entry's name)
                                              .layout = layout};
            VkResult r = vkCreateComputePipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p[k]);
            if (r != VK_SUCCESS) p[k] = VK_NULL_HANDLE, LOG("[vk] CMAA2: %s pipeline failed (%d)\n", entry[k], r);
            vkDestroyShaderModule(g_vk.dev, m, NULL);
        }
        if (!p[k]) {
            LOG("[vk] CMAA2: %s (preset %d%s) could not be built\n", entry[k], quality, sharp ? ", extra sharpness" : "");
            p[NKERNEL - 1] = VK_NULL_HANDLE;
            return NULL;
        }
    }
    return p;
}

static void copy(VkCommandBuffer cb, VkImage from, VkImage to, uint32_t to_level, uint32_t w, uint32_t h) {
    VkImageCopy c = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, to_level, 0, 1},
                     .extent = {w, h, 1}};
    vkCmdCopyImage(cb, from, VK_IMAGE_LAYOUT_GENERAL, to, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    vk_full_barrier(cb);
}

bool vk_cmaa2(const VkPostAa *t, int quality, bool extra_sharp) {
    if (t->format != VK_FORMAT_R8G8B8A8_UNORM || !t->src_image) return false;  // (what the game's FXAA pass draws into)
    if (quality < CMAA2_LOW || quality > CMAA2_ULTRA) quality = CMAA2_HIGH;
    const VkPipeline *p = pipelines(quality, extra_sharp);
    if (!p || !resources(t->w, t->h)) return false;
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    copy(cb, t->src_image, R.work.image, 0, t->w, t->h);  // (the pixels CMAA2 leaves alone)
    VkDescriptorSet set = vk_alloc_set(set_layout);
    VkDescriptorImageInfo ii[3] = {{VK_NULL_HANDLE, t->src, VK_IMAGE_LAYOUT_GENERAL},
                                   {VK_NULL_HANDLE, R.work_u32, VK_IMAGE_LAYOUT_GENERAL},
                                   {VK_NULL_HANDLE, R.edges.view, VK_IMAGE_LAYOUT_GENERAL}};
    VkDescriptorImageInfo heads = {VK_NULL_HANDLE, R.heads.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo bi[5] = {{R.candidates.buf, 0, VK_WHOLE_SIZE}, {R.locations.buf, 0, VK_WHOLE_SIZE},
                                    {R.items.buf, 0, VK_WHOLE_SIZE}, {R.control.buf, 0, VK_WHOLE_SIZE},
                                    {R.args.buf, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet w[9];
    for (int i = 0; i < 9; i++) w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .descriptorCount = 1};
    w[0].dstBinding = B_INPUT, w[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, w[0].pImageInfo = &ii[0];
    w[1].dstBinding = B_OUTPUT, w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, w[1].pImageInfo = &ii[1];
    w[2].dstBinding = B_EDGES, w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, w[2].pImageInfo = &ii[2];
    w[3].dstBinding = B_HEADS, w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, w[3].pImageInfo = &heads;
    static const uint32_t bufbind[5] = {B_CANDIDATES, B_LOCATIONS, B_ITEMS, B_CONTROL, B_ARGS};
    for (int i = 0; i < 5; i++)
        w[4 + i].dstBinding = bufbind[i], w[4 + i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, w[4 + i].pBufferInfo = &bi[i];
    vkUpdateDescriptorSets(g_vk.dev, 9, w, 0, NULL);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
    // edges: 16x16 threads a group, each group's output 14x14 2x2 blocks (CMAA2_CS_OUTPUT_KERNEL_SIZE)
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p[EDGES]);
    vkCmdDispatch(cb, (t->w + 28 - 1) / 28, (t->h + 28 - 1) / 28, 1);
    vk_full_barrier(cb);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p[ARGS]);
    vkCmdDispatch(cb, 2, 1, 1);  // (2, 1, 1): the candidates' arguments
    vk_full_barrier(cb);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p[CANDIDATES]);
    vkCmdDispatchIndirect(cb, R.args.buf, 0);
    vk_full_barrier(cb);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p[ARGS]);
    vkCmdDispatch(cb, 1, 2, 1);  // (1, 2, 1): the blends' arguments, and the counters cleared
    vk_full_barrier(cb);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p[APPLY]);
    vkCmdDispatchIndirect(cb, R.args.buf, 0);
    vk_full_barrier(cb);
    copy(cb, R.work.image, t->dst_image, t->level, t->w, t->h);
    static bool logged;
    static const char *const names[4] = {"low", "medium", "high", "ultra"};
    if (!logged) logged = true, LOG("[vk] CMAA2 (%s%s) draws in place of the game's FXAA, %ux%u\n", names[quality],
                                    extra_sharp ? ", extra sharpness" : "", t->w, t->h);
    return true;
}
