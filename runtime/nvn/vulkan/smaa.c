// SMAA 1x in place of the game's FXAA pass ([Anti-aliasing] AntiAliasing = SMAA; exec.c fxaa_replaced). Jorge
// Jimenez et al.'s SMAA (third_party/smaa, MIT): SMAA.hlsl's GLSL path, compiled with shaderc, and its precomputed
// area and search textures. Three passes at the target's size: luma edge detection of the game's FXAA input (gamma
// space, as SMAA wants it) into an RG8 image, the blending weights into an RGBA8 one, then the neighbourhood blending
// into the game's target. The edges are cleared first: the edge pass discards where there is none.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../rt.h"
#include "shaders.h"
#include "vk.h"
#include "AreaTex.h"
#include "SearchTex.h"

// SMAA.hlsl as data in the executable (the Makefile has third_party/smaa/SMAA.hlsl as a prerequisite)
__asm__(".pushsection .rodata\n.globl smaa_hlsl\nsmaa_hlsl:\n.incbin \"third_party/smaa/SMAA.hlsl\"\n"
        ".globl smaa_hlsl_end\nsmaa_hlsl_end:\n.byte 0\n.popsection");
extern const char smaa_hlsl[], smaa_hlsl_end[];

enum { EDGES, WEIGHTS, BLEND, NPASS };
typedef struct { VkImage image; VmaAllocation mem; VkImageView view; uint32_t w, h; } Img;

// The passes' GLSL: SMAA.hlsl's vertex or pixel half (with a preset, and the target's size from the push constants)
// around a main that feeds it SMAA's texture coordinates of a full-screen triangle.
static const char *const vs_main[NPASS] = {
    "layout(location = 0) out vec2 texcoord;\n"
    "layout(location = 1) out vec4 offset[3];\n"
    "void main() {\n"
    "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
    "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "  texcoord = p;\n"
    "  SMAAEdgeDetectionVS(texcoord, offset);\n"
    "}\n",
    "layout(location = 0) out vec2 texcoord;\n"
    "layout(location = 1) out vec4 offset[3];\n"
    "layout(location = 4) out vec2 pixcoord;\n"
    "void main() {\n"
    "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
    "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "  texcoord = p;\n"
    "  SMAABlendingWeightCalculationVS(texcoord, pixcoord, offset);\n"
    "}\n",
    "layout(location = 0) out vec2 texcoord;\n"
    "layout(location = 1) out vec4 offset;\n"
    "void main() {\n"
    "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
    "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "  texcoord = p;\n"
    "  SMAANeighborhoodBlendingVS(texcoord, offset);\n"
    "}\n"};
static const char *const fs_main[NPASS] = {
    "layout(set = 0, binding = 0) uniform sampler2D colorTex;\n"
    "layout(location = 0) in vec2 texcoord;\n"
    "layout(location = 1) in vec4 offset[3];\n"
    "layout(location = 0) out vec4 o;\n"
    "void main() { o = vec4(SMAALumaEdgeDetectionPS(texcoord, offset, colorTex), 0.0, 0.0); }\n",
    "layout(set = 0, binding = 0) uniform sampler2D edgesTex;\n"
    "layout(set = 0, binding = 1) uniform sampler2D areaTex;\n"
    "layout(set = 0, binding = 2) uniform sampler2D searchTex;\n"
    "layout(location = 0) in vec2 texcoord;\n"
    "layout(location = 1) in vec4 offset[3];\n"
    "layout(location = 4) in vec2 pixcoord;\n"
    "layout(location = 0) out vec4 o;\n"
    "void main() { o = SMAABlendingWeightCalculationPS(texcoord, pixcoord, offset, edgesTex, areaTex, searchTex, vec4(0.0)); }\n",
    "layout(set = 0, binding = 0) uniform sampler2D colorTex;\n"
    "layout(set = 0, binding = 1) uniform sampler2D blendTex;\n"
    "layout(location = 0) in vec2 texcoord;\n"
    "layout(location = 1) in vec4 offset;\n"
    "layout(location = 0) out vec4 o;\n"
    "void main() { o = SMAANeighborhoodBlendingPS(texcoord, offset, colorTex, blendTex); }\n"};
static const char *const presets[4] = {"LOW", "MEDIUM", "HIGH", "ULTRA"};

static VkShaderModule module(int pass, int quality, int stage) {
    static VkShaderModule cache[NPASS][4][2];
    VkShaderModule *m = &cache[pass][quality][stage == STAGE_FRAGMENT];
    if (*m) return *m;
    char head[512], name[64];
    snprintf(head, sizeof head,
             "#version 450\n#define SMAA_GLSL_4\n#define SMAA_PRESET_%s\n#define SMAA_INCLUDE_VS %d\n"
             "#define SMAA_INCLUDE_PS %d\nlayout(push_constant) uniform P { vec4 rt; } pc;\n#define SMAA_RT_METRICS pc.rt\n",
             presets[quality], stage != STAGE_FRAGMENT, stage == STAGE_FRAGMENT);
    const char *body = stage == STAGE_FRAGMENT ? fs_main[pass] : vs_main[pass];
    size_t nh = strlen(head), ns = (size_t)(smaa_hlsl_end - smaa_hlsl), nb = strlen(body);
    char *src = malloc(nh + ns + nb + 2);
    if (!src) return VK_NULL_HANDLE;
    memcpy(src, head, nh);
    memcpy(src + nh, smaa_hlsl, ns);
    src[nh + ns] = '\n';
    memcpy(src + nh + ns + 1, body, nb + 1);
    snprintf(name, sizeof name, "smaa.%d.%s.%s", pass, presets[quality], stage == STAGE_FRAGMENT ? "frag" : "vert");
    *m = vk_compile_glsl(src, stage, name);
    free(src);
    return *m;
}

static VkDescriptorSetLayout set_layout;
static VkPipelineLayout layout;

static VkPipeline pipeline(int pass, int quality, VkRenderPass rp) {
    static struct { int pass, quality; VkRenderPass rp; VkPipeline p; } tab[32];
    static int n;
    for (int i = 0; i < n; i++)
        if (tab[i].pass == pass && tab[i].quality == quality && tab[i].rp == rp) return tab[i].p;
    if (!layout) {
        VkDescriptorSetLayoutBinding b[3];
        for (uint32_t i = 0; i < 3; i++)
            b[i] = (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = b};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &set_layout));
        VkPushConstantRange r = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                         .pSetLayouts = &set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &layout));
    }
    VkShaderModule vs = module(pass, quality, STAGE_VERTEX), fs = module(pass, quality, STAGE_FRAGMENT);
    VkPipeline p = VK_NULL_HANDLE;
    if (vs && fs) {
        VkPipelineShaderStageCreateInfo stages[2] = {
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"}};
        VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                     .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
        VkPipelineViewportStateCreateInfo vps = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1,
                                                 .scissorCount = 1};
        VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                     .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
        VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                   .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
        VkPipelineColorBlendAttachmentState att = {.colorWriteMask = 0xF};
        VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                  .attachmentCount = 1, .pAttachments = &att};
        VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2,
                                               .pDynamicStates = dyn};
        VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2,
                                           .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
                                           .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms,
                                           .pColorBlendState = &cb, .pDynamicState = &dy, .layout = layout, .renderPass = rp};
        if (vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    }
    if (!p) LOG("[vk] SMAA pass %d (%s) pipeline failed\n", pass, presets[quality]);
    if (n < 32) tab[n].pass = pass, tab[n].quality = quality, tab[n].rp = rp, tab[n++].p = p;  // (a failure is not retried)
    return p;
}

// An image of our own in GENERAL layout (data: its pixels, rows of w texels), and its view.
static bool image(Img *m, VkFormat f, uint32_t w, uint32_t h, VkImageUsageFlags usage, const void *data, size_t bytes) {
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(g_vk.vma, &ci, &ai, &m->image, &m->mem, NULL) != VK_SUCCESS) return false;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = m->image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = f, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(g_vk.dev, &vi, NULL, &m->view) != VK_SUCCESS) {
        vk_defer_image(m->image, m->mem);
        return false;
    }
    m->w = w, m->h = h;
    VkCommandBuffer cb = vk_upload_cmd();
    vk_image_init_layout(cb, m->image, VK_IMAGE_ASPECT_COLOR_BIT);
    if (data) {
        VkSpan s = vk_transient_copy(data, bytes, 16);
        VkBufferImageCopy rg = {.bufferOffset = s.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
        vkCmdCopyBufferToImage(cb, s.buf, m->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
    }
    vk_full_barrier(cb);
    return true;
}

// The edges and blending weights of one target size (kept for the session: the game's FXAA pass has one or two).
static struct { Img edges, weights; } sizes[4];

static void pass(VkCommandBuffer cb, VkPipeline p, VkRenderPass rp, VkImageView target, uint32_t w, uint32_t h,
                 const VkDescriptorImageInfo *in, uint32_t nin) {
    VkDescriptorSet set = vk_alloc_set(set_layout);
    VkWriteDescriptorSet wr[3];
    for (uint32_t i = 0; i < nin; i++)
        wr[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = i,
                                       .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                       .pImageInfo = &in[i]};
    vkUpdateDescriptorSets(g_vk.dev, nin, wr, 0, NULL);
    VkRect2D area = {{0, 0}, {w, h}};
    VkRenderPassBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp,
                                .framebuffer = vk_framebuffer(rp, &target, 1, w, h), .renderArea = area};
    float rt[4] = {1.0f / (float)w, 1.0f / (float)h, (float)w, (float)h};
    vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof rt, rt);
    VkViewport vp = {0, 0, (float)w, (float)h, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &area);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRenderPass(cb);
    vk_full_barrier(cb);
}

bool vk_smaa(const VkPostAa *t, int quality) {
    static Img area, search;
    static bool tables_failed;
    if (quality < SMAA_LOW || quality > SMAA_ULTRA) quality = SMAA_ULTRA;
    if (!area.view && !tables_failed)
        tables_failed = !image(&area, VK_FORMAT_R8G8_UNORM, AREATEX_WIDTH, AREATEX_HEIGHT, 0, areaTexBytes, sizeof areaTexBytes) ||
                        !image(&search, VK_FORMAT_R8_UNORM, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, 0, searchTexBytes, sizeof searchTexBytes);
    if (tables_failed) return false;
    int k = 0;
    while (k < 4 && sizes[k].edges.view && (sizes[k].edges.w != t->w || sizes[k].edges.h != t->h)) k++;
    if (k == 4) return false;
    if (!sizes[k].edges.view) {
        const VkImageUsageFlags rt = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (!image(&sizes[k].edges, VK_FORMAT_R8G8_UNORM, t->w, t->h, rt, NULL, 0) ||
            !image(&sizes[k].weights, VK_FORMAT_R8G8B8A8_UNORM, t->w, t->h, rt, NULL, 0)) {
            LOG("[vk] SMAA: no images for %ux%u\n", t->w, t->h);
            return false;
        }
    }
    Img *edges = &sizes[k].edges, *weights = &sizes[k].weights;
    VkFormat rg8 = VK_FORMAT_R8G8_UNORM, rgba8 = VK_FORMAT_R8G8B8A8_UNORM;
    VkRenderPass rp[NPASS] = {vk_render_pass(&rg8, 1, VK_FORMAT_UNDEFINED, 1), vk_render_pass(&rgba8, 1, VK_FORMAT_UNDEFINED, 1),
                              vk_render_pass(&t->format, 1, VK_FORMAT_UNDEFINED, 1)};
    VkPipeline p[NPASS];
    for (int i = 0; i < NPASS; i++)
        if (!(p[i] = pipeline(i, quality, rp[i]))) return false;
    VkSampler linear = vk_default_sampler(true), point = vk_default_sampler(false);
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkClearColorValue zero = {{0, 0, 0, 0}};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, edges->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
    vk_full_barrier(cb);
    VkDescriptorImageInfo color = {linear, t->src, VK_IMAGE_LAYOUT_GENERAL};
    pass(cb, p[EDGES], rp[EDGES], edges->view, t->w, t->h, &color, 1);
    VkDescriptorImageInfo tables[3] = {{linear, edges->view, VK_IMAGE_LAYOUT_GENERAL},
                                       {linear, area.view, VK_IMAGE_LAYOUT_GENERAL},
                                       {point, search.view, VK_IMAGE_LAYOUT_GENERAL}};
    pass(cb, p[WEIGHTS], rp[WEIGHTS], weights->view, t->w, t->h, tables, 3);
    VkDescriptorImageInfo blend[2] = {color, {linear, weights->view, VK_IMAGE_LAYOUT_GENERAL}};
    pass(cb, p[BLEND], rp[BLEND], t->dst, t->w, t->h, blend, 2);
    static bool logged;
    if (!logged) logged = true, LOG("[vk] SMAA 1x (%s) draws in place of the game's FXAA, %ux%u\n", presets[quality], t->w, t->h);
    return true;
}
