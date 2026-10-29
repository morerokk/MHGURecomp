// SMAA 1x, S2x, T2x and 4x ([Anti-aliasing] AntiAliasing = SMAA / SMAAS2x / SMAAT2x / SMAA4x). Jorge Jimenez et al.'s SMAA (third_party/smaa,
// MIT): SMAA.hlsl's GLSL path, compiled with shaderc, and its precomputed area and search textures.
//  - 1x, in place of the game's FXAA pass (exec.c fxaa_replaced): three passes at the target's size: luma edge
//    detection of the game's FXAA input (gamma space, as SMAA wants it) into an RG8 image, the blending weights into an
//    RGBA8 one, then the neighbourhood blending into the game's target. The edges are cleared first: the edge pass
//    discards where there is none.
//  - T2x, on the full-screen image at the start of the HUD (fsr.c: the camera jittered by a quarter pixel one way, then
//    the other, and its motion vectors): the same passes with that frame's subsample indices into one of two history
//    images, the velocity's length packed in alpha (SMAA_REPROJECTION), then SMAA's resolve of it and the previous
//    frame's, reprojected along the motion vectors, back into the image.
//  - 4x = S2x, temporally jittered: the 3D scene renders with MSAA 2x and, where exec.c resolves it (once a frame,
//    before the game's post effects read it), each of its two samples goes through edges and weights on its own
//    (4x's subsample indices for the sample and the jitter), the two blended and averaged into the resolved image.
//    At the start of the HUD the T2x resolve as above, without its own edges (PACK: the picture and the velocity).
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

enum { EDGES, WEIGHTS, BLEND, SEPARATE, BLEND_S2X, BLEND_T2X, PACK, RESOLVE, NPASS };  // (from BLEND_T2X: reprojection)
typedef struct { VkImage image; VmaAllocation mem; VkImageView view; uint32_t w, h; } Img;

// The passes' GLSL: SMAA.hlsl's vertex or pixel half (with a preset, the target's size and the subsample indices from
// the push constants) around a main that feeds it SMAA's texture coordinates of a full-screen triangle. The motion
// vectors (fsr.c) are where a texel was in the previous frame minus where it is, in pixels; SMAA's velocity is the
// other way round, in texture coordinates.
#define VS_HEAD "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n" \
                "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n" \
                "  texcoord = p;\n"
static const char *const vs_main[NPASS] = {
    "layout(location = 0) out vec2 texcoord;\n"
    "layout(location = 1) out vec4 offset[3];\n"
    "void main() {\n" VS_HEAD "  SMAAEdgeDetectionVS(texcoord, offset);\n}\n",
    "layout(location = 0) out vec2 texcoord;\n"
    "layout(location = 1) out vec4 offset[3];\n"
    "layout(location = 4) out vec2 pixcoord;\n"
    "void main() {\n" VS_HEAD "  SMAABlendingWeightCalculationVS(texcoord, pixcoord, offset);\n}\n",
    "layout(location = 0) out vec2 texcoord;\n"
    "layout(location = 1) out vec4 offset;\n"
    "void main() {\n" VS_HEAD "  SMAANeighborhoodBlendingVS(texcoord, offset);\n}\n",
    "layout(location = 0) out vec2 texcoord;\n"
    "void main() {\n" VS_HEAD "}\n",
    "layout(location = 0) out vec2 texcoord;\n"
    "layout(location = 1) out vec4 offset;\n"
    "void main() {\n" VS_HEAD "  SMAANeighborhoodBlendingVS(texcoord, offset);\n}\n",
    "layout(location = 0) out vec2 texcoord;\n"
    "layout(location = 1) out vec4 offset;\n"
    "void main() {\n" VS_HEAD "  SMAANeighborhoodBlendingVS(texcoord, offset);\n}\n",
    "layout(location = 0) out vec2 texcoord;\n"
    "void main() {\n" VS_HEAD "}\n",
    "layout(location = 0) out vec2 texcoord;\n"
    "void main() {\n" VS_HEAD "}\n"};
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
    "void main() { o = SMAABlendingWeightCalculationPS(texcoord, pixcoord, offset, edgesTex, areaTex, searchTex, pc.sub); }\n",
    "layout(set = 0, binding = 0) uniform sampler2D colorTex;\n"
    "layout(set = 0, binding = 1) uniform sampler2D blendTex;\n"
    "layout(location = 0) in vec2 texcoord;\n"
    "layout(location = 1) in vec4 offset;\n"
    "layout(location = 0) out vec4 o;\n"
    "void main() { o = SMAANeighborhoodBlendingPS(texcoord, offset, colorTex, blendTex); }\n",
    // S2x: one sample (pc.sub.x) of the multisampled image as an image of its own (SMAASeparatePS)
    "layout(set = 0, binding = 0) uniform sampler2DMS msTex;\n"
    "layout(location = 0) in vec2 texcoord;\n"
    "layout(location = 0) out vec4 o;\n"
    "void main() { o = texelFetch(msTex, ivec2(gl_FragCoord.xy), int(pc.sub.x)); }\n",
    // S2x: each sample blended along its own weights, the two averaged (SMAA.hlsl: the second run blends at 0.5)
    "layout(set = 0, binding = 0) uniform sampler2D colorTex0;\n"
    "layout(set = 0, binding = 1) uniform sampler2D blendTex0;\n"
    "layout(set = 0, binding = 2) uniform sampler2D colorTex1;\n"
    "layout(set = 0, binding = 3) uniform sampler2D blendTex1;\n"
    "layout(location = 0) in vec2 texcoord;\n"
    "layout(location = 1) in vec4 offset;\n"
    "layout(location = 0) out vec4 o;\n"
    "void main() {\n"
    "  o = 0.5 * (SMAANeighborhoodBlendingPS(texcoord, offset, colorTex0, blendTex0) +\n"
    "             SMAANeighborhoodBlendingPS(texcoord, offset, colorTex1, blendTex1));\n"
    "}\n",
    "layout(set = 0, binding = 0) uniform sampler2D colorTex;\n"
    "layout(set = 0, binding = 1) uniform sampler2D blendTex;\n"
    "layout(set = 0, binding = 2) uniform sampler2D velocityTex;\n"
    "layout(location = 0) in vec2 texcoord;\n"
    "layout(location = 1) in vec4 offset;\n"
    "layout(location = 0) out vec4 o;\n"
    "void main() { o = SMAANeighborhoodBlendingPS(texcoord, offset, colorTex, blendTex, velocityTex); }\n",
    // 4x: the picture (S2x already in it) with the velocity's length in alpha, as the blending pass leaves it for the
    // resolve (SMAA_REPROJECTION)
    "layout(set = 0, binding = 0) uniform sampler2D colorTex;\n"
    "layout(set = 0, binding = 1) uniform sampler2D velocityTex;\n"
    "layout(location = 0) in vec2 texcoord;\n"
    "layout(location = 0) out vec4 o;\n"
    "void main() {\n"
    "  vec2 velocity = SMAA_DECODE_VELOCITY(textureLod(velocityTex, texcoord, 0.0));\n"
    "  o = vec4(textureLod(colorTex, texcoord, 0.0).rgb, sqrt(5.0 * length(velocity)));\n"
    "}\n",
    // SMAAResolvePS (SMAA_REPROJECTION), with the previous frame's colour clamped to the range of the current frame's
    // 3x3 neighbourhood: where the motion vectors are wrong (a limb the object vectors miss, the background a moving
    // character uncovers) the old picture is not blended in as a ghost. No history from outside the picture, nor where
    // the reactive mask (fsr.c: where the game's moving-object mask changed since the last frame) says a character moved.
    "layout(set = 0, binding = 0) uniform sampler2D currentColorTex;\n"
    "layout(set = 0, binding = 1) uniform sampler2D previousColorTex;\n"
    "layout(set = 0, binding = 2) uniform sampler2D velocityTex;\n"
    "layout(set = 0, binding = 3) uniform sampler2D reactiveTex;\n"
    "layout(location = 0) in vec2 texcoord;\n"
    "layout(location = 0) out vec4 o;\n"
    "#define NB(x, y) c = textureLodOffset(currentColorTex, texcoord, 0.0, ivec2(x, y)).rgb; lo = min(lo, c); hi = max(hi, c);\n"
    "void main() {\n"
    "  vec2 velocity = -SMAA_DECODE_VELOCITY(textureLod(velocityTex, texcoord, 0.0));\n"
    "  vec2 at = texcoord + velocity;\n"
    "  vec4 current = textureLod(currentColorTex, texcoord, 0.0), previous = textureLod(previousColorTex, at, 0.0);\n"
    "  vec3 lo = current.rgb, hi = current.rgb, c;\n"
    "  NB(-1, -1) NB(0, -1) NB(1, -1) NB(-1, 0) NB(1, 0) NB(-1, 1) NB(0, 1) NB(1, 1)\n"
    "  previous.rgb = clamp(previous.rgb, lo, hi);\n"
    "  float delta = abs(current.a * current.a - previous.a * previous.a) / 5.0;\n"
    "  float weight = 0.5 * saturate(1.0 - sqrt(delta) * SMAA_REPROJECTION_WEIGHT_SCALE);\n"
    "  if (any(lessThan(at, vec2(0.0))) || any(greaterThan(at, vec2(1.0)))) weight = 0.0;\n"
    "  weight *= 1.0 - saturate(textureLod(reactiveTex, texcoord, 0.0).r);\n"
    "  o = vec4(mix(current.rgb, previous.rgb, weight), 1.0);\n"
    "}\n"};
static const char *const presets[4] = {"LOW", "MEDIUM", "HIGH", "ULTRA"};

static VkShaderModule module(int pass, int quality, int stage) {
    static VkShaderModule cache[NPASS][4][2];
    VkShaderModule *m = &cache[pass][quality][stage == STAGE_FRAGMENT];
    if (*m) return *m;
    char head[768], name[64];
    snprintf(head, sizeof head,
             "#version 450\n#define SMAA_GLSL_4\n#define SMAA_PRESET_%s\n#define SMAA_INCLUDE_VS %d\n"
             "#define SMAA_INCLUDE_PS %d\nlayout(push_constant) uniform P { vec4 rt, sub; } pc;\n#define SMAA_RT_METRICS pc.rt\n%s",
             presets[quality], stage != STAGE_FRAGMENT, stage == STAGE_FRAGMENT,
             pass >= BLEND_T2X ? "#define SMAA_REPROJECTION 1\n#define SMAA_DECODE_VELOCITY(s) (-(s).rg * SMAA_RT_METRICS.xy)\n" : "");
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
    static struct { int pass, quality; VkRenderPass rp; VkPipeline p; } tab[64];
    static int n;
    for (int i = 0; i < n; i++)
        if (tab[i].pass == pass && tab[i].quality == quality && tab[i].rp == rp) return tab[i].p;
    if (!layout) {
        VkDescriptorSetLayoutBinding b[4];
        for (uint32_t i = 0; i < 4; i++)
            b[i] = (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 4, .pBindings = b};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &set_layout));
        VkPushConstantRange r = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
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
    if (n < 64) tab[n].pass = pass, tab[n].quality = quality, tab[n].rp = rp, tab[n++].p = p;  // (a failure is not retried)
    return p;
}

// An image of our own in GENERAL layout (data: its pixels, rows of w texels), and its view.
static bool image(Img *m, VkFormat f, uint32_t w, uint32_t h, VkImageUsageFlags usage, const void *data, size_t bytes) {
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = usage | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
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

// What one target size needs (kept for the session: the game draws these passes at one or two sizes): the edges, the
// blending weights and, for T2x, two frames of history in the target's format (hist_ok: hist[cur ^ 1] holds the last);
// for S2x, the two samples apart (in the target's format) and the second one's blending weights.
typedef struct { Img edges, weights, hist[2], sample[2], weights1; VkFormat hist_fmt, sample_fmt; int cur; bool hist_ok; } Size;
static Size sizes[4];
enum { MODE_1X, MODE_T2X, MODE_S2X };

// subsampleIndices (SMAA.hlsl @SUBSAMPLE_INDICES) of S2x's samples 0 / 1: S2x alone, then 4x's first and second jitter
static const float s2x_sub[3][2][4] = {{{1, 1, 1, 0}, {2, 2, 2, 0}},
                                       {{5, 3, 1, 3}, {4, 6, 2, 3}},
                                       {{3, 5, 1, 4}, {6, 4, 2, 4}}};

// sub: the subsample indices (NULL: none, SMAA 1x; SEPARATE: the sample in x)
static void pass(VkCommandBuffer cb, VkPipeline p, VkRenderPass rp, VkImageView target, uint32_t w, uint32_t h,
                 const VkDescriptorImageInfo *in, uint32_t nin, const float *sub) {
    VkDescriptorSet set = vk_alloc_set(set_layout);
    VkWriteDescriptorSet wr[4];
    for (uint32_t i = 0; i < nin; i++)
        wr[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = i,
                                       .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                       .pImageInfo = &in[i]};
    vkUpdateDescriptorSets(g_vk.dev, nin, wr, 0, NULL);
    VkRect2D area = {{0, 0}, {w, h}};
    VkRenderPassBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp,
                                .framebuffer = vk_framebuffer(rp, &target, 1, w, h), .renderArea = area};
    float pc[8] = {1.0f / (float)w, 1.0f / (float)h, (float)w, (float)h};
    if (sub) memcpy(pc + 4, sub, 4 * sizeof *sub);
    vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc);
    VkViewport vp = {0, 0, (float)w, (float)h, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &area);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRenderPass(cb);
    vk_full_barrier(cb);
}

static Img area, search, still;  // the lookup textures; still: one texel of zero motion (a resolve without history)

// The lookup textures, the images of t's size and the passes' pipelines the mode uses.
static Size *prepare(const VkPostAa *t, int quality, int mode, VkPipeline p[NPASS], VkRenderPass rp[NPASS]) {
    bool t2x = mode == MODE_T2X, s2x = mode == MODE_S2X;
    static bool tables_failed;
    static const uint8_t zero[4] = {0};
    if (!area.view && !tables_failed)
        tables_failed = !image(&area, VK_FORMAT_R8G8_UNORM, AREATEX_WIDTH, AREATEX_HEIGHT, 0, areaTexBytes, sizeof areaTexBytes) ||
                        !image(&search, VK_FORMAT_R8_UNORM, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, 0, searchTexBytes, sizeof searchTexBytes) ||
                        !image(&still, VK_FORMAT_R8G8_UNORM, 1, 1, 0, zero, 2);
    if (tables_failed) return NULL;
    int k = 0;
    while (k < 4 && sizes[k].edges.view && (sizes[k].edges.w != t->w || sizes[k].edges.h != t->h)) k++;
    if (k == 4) return NULL;
    Size *z = &sizes[k];
    const VkImageUsageFlags rt = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (!z->edges.view && (!image(&z->edges, VK_FORMAT_R8G8_UNORM, t->w, t->h, rt, NULL, 0) ||
                           !image(&z->weights, VK_FORMAT_R8G8B8A8_UNORM, t->w, t->h, rt, NULL, 0))) {
        LOG("[vk] SMAA: no images for %ux%u\n", t->w, t->h);
        return NULL;
    }
    if (t2x && z->hist[0].view && z->hist_fmt != t->format) z->hist[0].view = z->hist[1].view = VK_NULL_HANDLE;  // (remade below)
    if (t2x && !z->hist[0].view) {
        if (!image(&z->hist[0], t->format, t->w, t->h, rt, NULL, 0) || !image(&z->hist[1], t->format, t->w, t->h, rt, NULL, 0)) {
            LOG("[vk] SMAA T2x: no history images for %ux%u\n", t->w, t->h);
            return NULL;
        }
        z->hist_fmt = t->format, z->hist_ok = false;
    }
    if (s2x && z->sample[0].view && z->sample_fmt != t->format) z->sample[0].view = z->sample[1].view = VK_NULL_HANDLE;
    if (s2x && (!z->weights1.view || !z->sample[0].view)) {
        if ((!z->weights1.view && !image(&z->weights1, VK_FORMAT_R8G8B8A8_UNORM, t->w, t->h, rt, NULL, 0)) ||
            !image(&z->sample[0], t->format, t->w, t->h, rt, NULL, 0) || !image(&z->sample[1], t->format, t->w, t->h, rt, NULL, 0)) {
            LOG("[vk] SMAA S2x: no images for %ux%u\n", t->w, t->h);
            return NULL;
        }
        z->sample_fmt = t->format;
    }
    VkFormat rg8 = VK_FORMAT_R8G8_UNORM, rgba8 = VK_FORMAT_R8G8B8A8_UNORM;
    rp[EDGES] = vk_render_pass(&rg8, 1, VK_FORMAT_UNDEFINED, 1);
    rp[WEIGHTS] = vk_render_pass(&rgba8, 1, VK_FORMAT_UNDEFINED, 1);
    rp[BLEND] = rp[SEPARATE] = rp[BLEND_S2X] = rp[BLEND_T2X] = rp[PACK] = rp[RESOLVE] =
        vk_render_pass(&t->format, 1, VK_FORMAT_UNDEFINED, 1);
    static const bool used[3][NPASS] = {  // [mode][pass]
        {[EDGES] = 1, [WEIGHTS] = 1, [BLEND] = 1},
        {[EDGES] = 1, [WEIGHTS] = 1, [BLEND] = 1, [BLEND_T2X] = 1, [PACK] = 1, [RESOLVE] = 1},
        {[EDGES] = 1, [WEIGHTS] = 1, [SEPARATE] = 1, [BLEND_S2X] = 1}};
    for (int i = 0; i < NPASS; i++)
        if (used[mode][i] && !(p[i] = pipeline(i, quality, rp[i]))) return NULL;
    return z;
}

// Edges of src and its blending weights into weights (sub: the subsample indices, NULL for 1x).
static void edges_and_weights(VkCommandBuffer cb, Size *z, const VkPipeline *p, const VkRenderPass *rp, VkImageView src,
                              Img *weights, const float *sub) {
    VkClearColorValue zero = {{0, 0, 0, 0}};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, z->edges.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
    vk_full_barrier(cb);
    VkSampler linear = vk_default_sampler(true), point = vk_default_sampler(false);
    VkDescriptorImageInfo color = {linear, src, VK_IMAGE_LAYOUT_GENERAL};
    pass(cb, p[EDGES], rp[EDGES], z->edges.view, z->edges.w, z->edges.h, &color, 1, sub);
    VkDescriptorImageInfo tables[3] = {{linear, z->edges.view, VK_IMAGE_LAYOUT_GENERAL},
                                       {linear, area.view, VK_IMAGE_LAYOUT_GENERAL},
                                       {point, search.view, VK_IMAGE_LAYOUT_GENERAL}};
    pass(cb, p[WEIGHTS], rp[WEIGHTS], weights->view, z->edges.w, z->edges.h, tables, 3, sub);
}

static Size *prepare_all(const VkPostAa *t, int quality, int mode, VkPipeline p[NPASS], VkRenderPass rp[NPASS]) {
    if (quality < SMAA_LOW || quality > SMAA_ULTRA) quality = SMAA_ULTRA;
    return prepare(t, quality, mode, p, rp);
}

bool vk_smaa(const VkPostAa *t, int quality) {
    VkPipeline p[NPASS];
    VkRenderPass rp[NPASS];
    Size *z = prepare_all(t, quality, MODE_1X, p, rp);
    if (!z) return false;
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    edges_and_weights(cb, z, p, rp, t->src, &z->weights, NULL);
    VkSampler linear = vk_default_sampler(true);
    VkDescriptorImageInfo blend[2] = {{linear, t->src, VK_IMAGE_LAYOUT_GENERAL}, {linear, z->weights.view, VK_IMAGE_LAYOUT_GENERAL}};
    pass(cb, p[BLEND], rp[BLEND], t->dst, t->w, t->h, blend, 2, NULL);
    static bool logged;
    if (!logged) logged = true, LOG("[vk] SMAA 1x (%s) draws in place of the game's FXAA, %ux%u\n", presets[quality & 3], t->w, t->h);
    return true;
}

bool vk_smaa_s2x(const VkPostAa *t, VkImageView ms, int quality, int phase) {
    VkPipeline p[NPASS];
    VkRenderPass rp[NPASS];
    Size *z = prepare_all(t, quality, MODE_S2X, p, rp);
    if (!z) return false;
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkSampler linear = vk_default_sampler(true), point = vk_default_sampler(false);
    for (int k = 0; k < 2; k++) {
        VkDescriptorImageInfo in = {point, ms, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        float which[4] = {(float)k};
        pass(cb, p[SEPARATE], rp[SEPARATE], z->sample[k].view, t->w, t->h, &in, 1, which);
    }
    const float (*sub)[4] = s2x_sub[phase < 0 ? 0 : 1 + (phase & 1)];
    edges_and_weights(cb, z, p, rp, z->sample[0].view, &z->weights, sub[0]);
    edges_and_weights(cb, z, p, rp, z->sample[1].view, &z->weights1, sub[1]);
    VkDescriptorImageInfo blend[4] = {{linear, z->sample[0].view, VK_IMAGE_LAYOUT_GENERAL},
                                      {linear, z->weights.view, VK_IMAGE_LAYOUT_GENERAL},
                                      {linear, z->sample[1].view, VK_IMAGE_LAYOUT_GENERAL},
                                      {linear, z->weights1.view, VK_IMAGE_LAYOUT_GENERAL}};
    pass(cb, p[BLEND_S2X], rp[BLEND_S2X], t->dst, t->w, t->h, blend, 4, NULL);
    static bool logged;
    if (!logged) logged = true, LOG("[vk] SMAA S2x (%s) resolves the 3D scene, %ux%u\n", presets[quality & 3], t->w, t->h);
    return true;
}

bool vk_smaa_t2x(const VkPostAa *t, int quality, VkImageView velocity, VkImageView reactive, int phase, bool history,
                 bool spatial) {
    VkPipeline p[NPASS];
    VkRenderPass rp[NPASS];
    Size *z = prepare_all(t, quality, MODE_T2X, p, rp);
    if (!z) return false;
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkSampler linear = vk_default_sampler(true), point = vk_default_sampler(false);
    Img *cur = &z->hist[z->cur], *prev = &z->hist[z->cur ^ 1];
    if (phase < 0 && !spatial) {  // (4x without its jitter: S2x is all there is)
        z->hist_ok = false;
        return true;
    }
    if (phase < 0) {  // no jitter, no vectors this frame: SMAA 1x of the image, through the history image (in place)
        edges_and_weights(cb, z, p, rp, t->src, &z->weights, NULL);
        VkDescriptorImageInfo blend[2] = {{linear, t->src, VK_IMAGE_LAYOUT_GENERAL}, {linear, z->weights.view, VK_IMAGE_LAYOUT_GENERAL}};
        pass(cb, p[BLEND], rp[BLEND], cur->view, t->w, t->h, blend, 2, NULL);
        VkImageCopy c = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                         .extent = {t->w, t->h, 1}};
        vkCmdCopyImage(cb, cur->image, VK_IMAGE_LAYOUT_GENERAL, t->dst_image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        vk_full_barrier(cb);
        z->hist_ok = false;
        return true;
    }
    // T2x: (1, 1, 1, 0) for the first jitter, (2, 2, 2, 0) the second
    float s = (float)(phase + 1), sub[4] = {s, s, s, 0.0f};
    if (spatial) {
        edges_and_weights(cb, z, p, rp, t->src, &z->weights, sub);
        VkDescriptorImageInfo blend[3] = {{linear, t->src, VK_IMAGE_LAYOUT_GENERAL},
                                          {linear, z->weights.view, VK_IMAGE_LAYOUT_GENERAL},
                                          {linear, velocity, VK_IMAGE_LAYOUT_GENERAL}};
        pass(cb, p[BLEND_T2X], rp[BLEND_T2X], cur->view, t->w, t->h, blend, 3, sub);
    } else {  // 4x: the S2x of the scene (vk_smaa_s2x) is in the picture already
        VkDescriptorImageInfo in[2] = {{linear, t->src, VK_IMAGE_LAYOUT_GENERAL}, {point, velocity, VK_IMAGE_LAYOUT_GENERAL}};
        pass(cb, p[PACK], rp[PACK], cur->view, t->w, t->h, in, 2, NULL);
    }
    bool use_prev = history && z->hist_ok;  // (else the resolve blends this frame with itself: just this frame)
    VkDescriptorImageInfo resolve[4] = {{point, cur->view, VK_IMAGE_LAYOUT_GENERAL},
                                        {linear, use_prev ? prev->view : cur->view, VK_IMAGE_LAYOUT_GENERAL},
                                        {point, use_prev ? velocity : still.view, VK_IMAGE_LAYOUT_GENERAL},
                                        {linear, reactive ? reactive : still.view, VK_IMAGE_LAYOUT_GENERAL}};
    pass(cb, p[RESOLVE], rp[RESOLVE], t->dst, t->w, t->h, resolve, 4, sub);
    z->cur ^= 1;
    z->hist_ok = true;
    static bool logged[2];
    if (!logged[spatial]) logged[spatial] = true, LOG("[vk] SMAA %s (%s) on the full-screen image, %ux%u\n",
                                                      spatial ? "T2x" : "4x's temporal half", presets[quality & 3], t->w, t->h);
    return true;
}
