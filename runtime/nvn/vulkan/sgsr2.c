// SGSR 2, the mobile 2-pass fragment upscaler ([Upscaler] Method = SGSR2, with FSR = On). fsr.c finds the frame's
// scene, camera, object motion and history resets and prepares its depth and motion-vector images; this file records
// the three Vulkan passes that Qualcomm's shaders need, into the same output texture FSR would have written:
//
//   1. velocity (compute): the port's render-pixel vectors -> SGSR 2's encoded clip-space vectors. Only moving
//      objects get a vector; static pixels get zero and SGSR 2 reprojects their depth with clipToPrevClip.
//   2. convert (fragment, render size): InputColor / InputDepth / InputVelocity -> RGBA16F motion/depth/clip.
//   3. upscale (fragment, output size): that buffer + the current colour + the last history image -> the next
//      RGBA16F history image, clamped into H'. The two history images are ping-ponged; H' is what the game's HUD
//      then draws over.
//
// The GLSL of passes 2 and 3 is Qualcomm's, unchanged (third_party/sgsr2); they are compiled with
// REQUEST_NDC_Y_UP because the game's clip space has +y at the top, as do Vulkan's texture coordinates here.
// The port's depth is 0 near .. 1 far, so the README's reverse-Z edits are not needed.
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "sgsr2.h"
#include "sgsr2_math.h"
#include "shaders.h"
#include "sgsr2_glsl.h"

static uint32_t rw, rh, ow, oh;

// Only moving objects get a vector: Qualcomm's convert shader checks `EncodedVelocity.x > 0` to decide between the
// texture's vector and reprojecting depth, so STATIC pixels must be literal zero, not an encoded zero motion (0.5).
// The port's object vectors are render pixels, +y down; SGSR decodes (ev - 0.5) / 0.2495 into clip-space motion with
// +y up in NDC (REQUEST_NDC_Y_UP).
static const char VEL_GLSL[] =
    "#version 450\n"
    "layout(local_size_x = 8, local_size_y = 8) in;\n"
    "layout(set = 0, binding = 0) uniform sampler2D obj_vel;\n"  // RGBA16F: object pixels, b > 0.5 = valid
    "layout(set = 0, binding = 1, rgba16f) uniform writeonly image2D out_vel;\n"
    "layout(push_constant) uniform Push { vec2 size; uint flags; uint reset; } pc;\n"
    "void main() {\n"
    "    ivec2 p = ivec2(gl_GlobalInvocationID.xy);\n"
    "    if (any(greaterThanEqual(p, imageSize(out_vel)))) return;\n"
    "    vec4 v = texelFetch(obj_vel, p, 0);\n"
    "    if (pc.reset != 0u || (pc.flags & 1u) == 0u || v.b <= 0.5) { imageStore(out_vel, p, vec4(0.0)); return; }\n"
    // The port's mv is (previous - current) in y-down pixels; SGSR's decoded motion is (current - previous) in
    // NDC with +y up, hence both the x flip and the y flip. Clamping keeps the encoding above the zero sentinel.
    "    vec2 ndc = clamp(vec2(-2.0 * v.r / pc.size.x, 2.0 * v.g / pc.size.y), vec2(-1.9), vec2(1.9));\n"
    "    imageStore(out_vel, p, vec4(ndc * 0.2495 + 0.5, 0.0, 0.0));\n"
    "}\n";

// A fullscreen triangle with explicit locations and UVs: v = 0 at the top row (Vulkan image coordinates), so that
// REQUEST_NDC_Y_UP in Qualcomm's shaders sees +y up in clip space, like the game's cameras.
static const char QUAD_VS[] =
    "#version 450\n"
    "layout(location = 0) out vec2 texCoord;\n"
    "void main() {\n"
    "    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "    texCoord = vec2(p.x, 1.0 - p.y);\n"
    "}\n";

// Qualcomm's sample keeps both history images RGBA16F. The port's H' is RGBA8 (the game's LDR picture), so a small
// final pass writes the RGBA16F upscale into it instead of copying between different formats.
static const char COPY_FS[] =
    "#version 450\n"
    "layout(set = 0, binding = 0) uniform sampler2D src;\n"
    "layout(location = 0) out vec4 o;\n"
    "void main() { o = vec4(clamp(texelFetch(src, ivec2(gl_FragCoord.xy), 0).rgb, 0.0, 1.0), 1.0); }\n";

static const char *const NDC_Y_UP[] = {"REQUEST_NDC_Y_UP=1"};

static VkDescriptorSetLayout vel_set, convert_set, upscale_set, copy_set;
static VkPipelineLayout vel_layout, convert_layout, upscale_layout, copy_layout;
static VkPipeline vel_pipe, convert_pipe, upscale_pipe, copy_pipe;
static VkShaderModule quad_vs, copy_fs;
static VkRenderPass rp_convert, rp_upscale, rp_copy;
static VkFormat copy_fmt = VK_FORMAT_UNDEFINED;
static TexBackend img_vel_enc, img_mda, img_hist[2];  // encoded velocity (render), motion/depth/clip (render), history (output)
static VkBuffer ubo;
static VmaAllocation ubo_mem;
static uint32_t hist_cur;
static bool same_valid;
static int same_frames;

static bool own_image(TexBackend *b, VkFormat f, uint32_t w, uint32_t h, VkImageUsageFlags usage) {
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(g_vk.vma, &ci, &ai, &b->image, &b->mem, NULL) != VK_SUCCESS) return false;
    b->format = f, b->base_type = VK_IMAGE_VIEW_TYPE_2D, b->width = w, b->height = h;
    b->depth = b->layers = b->levels = b->samples = 1;
    b->aspect = VK_IMAGE_ASPECT_COLOR_BIT, b->gpu_owned = true;
    VkCommandBuffer cb = vk_upload_cmd();
    vk_image_init_layout(cb, b->image, b->aspect);
    VkClearColorValue zero = {{0, 0, 0, 0}};
    VkImageSubresourceRange rg = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) vkCmdClearColorImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &rg);
    vk_full_barrier(cb);
    return true;
}

static VkShaderModule module(const char *src, size_t len, int stage, const char *name, bool ndc) {
    size_t nw = 0;
    uint32_t *spv = vk_glsl_to_spirv_stage_ex(src, len, stage, name, ndc ? NDC_Y_UP : NULL, ndc ? 1 : 0, 0, 0, &nw);
    if (!spv) return VK_NULL_HANDLE;
    VkShaderModule m = vk_module(spv, nw);
    free(spv);
    return m;
}

// One descriptor-set layout, with the bindings of a pass and a UBO at 0 where the shaders declare one.
static VkDescriptorSetLayout set_layout(const VkDescriptorType *types, int n) {
    VkDescriptorSetLayoutBinding b[8];
    for (int i = 0; i < n; i++)
        b[i] = (VkDescriptorSetLayoutBinding){.binding = i, .descriptorType = types[i], .descriptorCount = 1,
                                              .stageFlags = types[i] == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                                                ? VK_SHADER_STAGE_FRAGMENT_BIT
                                                                : VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = n,
                                          .pBindings = b};
    VkDescriptorSetLayout l;
    return vkCreateDescriptorSetLayout(g_vk.dev, &ci, NULL, &l) == VK_SUCCESS ? l : VK_NULL_HANDLE;
}

static bool make_layouts(void) {
    static const VkDescriptorType vel_types[] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
    static const VkDescriptorType conv_types[] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER};
    static const VkDescriptorType up_types[] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER};
    static const VkDescriptorType copy_types[] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER};
    vel_set = set_layout(vel_types, 2);
    convert_set = set_layout(conv_types, 3);
    upscale_set = set_layout(up_types, 4);
    copy_set = set_layout(copy_types, 1);
    if (!vel_set || !convert_set || !upscale_set || !copy_set) return false;
    VkPushConstantRange vr = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = 16};
    VkPipelineLayoutCreateInfo vli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                      .pSetLayouts = &vel_set, .pushConstantRangeCount = 1, .pPushConstantRanges = &vr};
    if (vkCreatePipelineLayout(g_vk.dev, &vli, NULL, &vel_layout) != VK_SUCCESS) return false;
    // The UBO is a descriptor, not a push constant: both graphics pipelines use only set 0.
    VkPipelineLayoutCreateInfo gli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1};
    gli.pSetLayouts = &convert_set;
    if (vkCreatePipelineLayout(g_vk.dev, &gli, NULL, &convert_layout) != VK_SUCCESS) return false;
    gli.pSetLayouts = &upscale_set;
    if (vkCreatePipelineLayout(g_vk.dev, &gli, NULL, &upscale_layout) != VK_SUCCESS) return false;
    gli.pSetLayouts = &copy_set;
    return vkCreatePipelineLayout(g_vk.dev, &gli, NULL, &copy_layout) == VK_SUCCESS;
}

static bool make_velocity_pipe(void) {
    size_t nw = 0;
    uint32_t *spv = vk_glsl_to_spirv(VEL_GLSL, STAGE_COMPUTE, "sgsr2_velocity", &nw);
    if (!spv) return false;
    VkShaderModule m = vk_module(spv, nw);
    free(spv);
    if (!m) return false;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = vel_layout};
    VkResult r = vkCreateComputePipelines(g_vk.dev, g_vk.pcache, 1, &ci, NULL, &vel_pipe);
    vkDestroyShaderModule(g_vk.dev, m, NULL);
    return r == VK_SUCCESS;
}

static VkPipeline graphics_pipe(VkRenderPass rp, VkPipelineLayout layout, VkShaderModule fs) {
    VkPipelineShaderStageCreateInfo st[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = quad_vs, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = fs, .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                            .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = 1, .pAttachments = &cba};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2,
                                       .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
                                       .pViewportState = &vs, .pRasterizationState = &rs, .pMultisampleState = &ms,
                                       .pDepthStencilState = &ds, .pColorBlendState = &cb, .pDynamicState = &dy,
                                       .layout = layout, .renderPass = rp};
    VkPipeline p;
    return vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &ci, NULL, &p) == VK_SUCCESS ? p : VK_NULL_HANDLE;
}

bool vk_sgsr2_init(uint32_t w, uint32_t h, uint32_t wo, uint32_t ho) {
    rw = w, rh = h, ow = wo, oh = ho;
    if (!make_layouts()) { LOG("[sgsr2] descriptor layouts failed\n"); return false; }
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = sizeof(Sgsr2Params),
                             .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateBuffer(g_vk.vma, &bi, &ai, &ubo, &ubo_mem, NULL) != VK_SUCCESS) {
        LOG("[sgsr2] the uniform buffer could not be made\n");
        return false;
    }
    VkImageUsageFlags img_use = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    bool ok = own_image(&img_vel_enc, VK_FORMAT_R16G16B16A16_SFLOAT, rw, rh, img_use) &&
              own_image(&img_mda, VK_FORMAT_R16G16B16A16_SFLOAT, rw, rh,
                        img_use | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) &&
              own_image(&img_hist[0], VK_FORMAT_R16G16B16A16_SFLOAT, ow, oh,
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) &&
              own_image(&img_hist[1], VK_FORMAT_R16G16B16A16_SFLOAT, ow, oh,
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    if (!ok) { LOG("[sgsr2] an image could not be made\n"); return false; }
    size_t clen, ulen;
    const char *cglsl = sgsr2_glsl_convert(&clen), *uglsl = sgsr2_glsl_upscale(&ulen);
    VkShaderModule cm = module(cglsl, clen, STAGE_FRAGMENT, "sgsr2_convert.frag", true);
    VkShaderModule um = module(uglsl, ulen, STAGE_FRAGMENT, "sgsr2_upscale.frag", true);
    quad_vs = module(QUAD_VS, strlen(QUAD_VS), STAGE_VERTEX, "sgsr2_fullscreen.vert", false);
    copy_fs = module(COPY_FS, strlen(COPY_FS), STAGE_FRAGMENT, "sgsr2_copy.frag", false);
    VkFormat mf = img_mda.format, hf = img_hist[0].format;
    rp_convert = vk_render_pass(&mf, 1, VK_FORMAT_UNDEFINED, 1);
    rp_upscale = vk_render_pass(&hf, 1, VK_FORMAT_UNDEFINED, 1);
    bool pipes = cm && um && quad_vs && copy_fs && make_velocity_pipe() &&
                 (convert_pipe = graphics_pipe(rp_convert, convert_layout, cm)) &&
                 (upscale_pipe = graphics_pipe(rp_upscale, upscale_layout, um));
    if (cm) vkDestroyShaderModule(g_vk.dev, cm, NULL);
    if (um) vkDestroyShaderModule(g_vk.dev, um, NULL);
    if (!pipes) { LOG("[sgsr2] a shader or pipeline failed to build\n"); return false; }
    LOG("[sgsr2] on: %ux%u -> %ux%u, two fragment passes from Qualcomm SGSR 2\n", rw, rh, ow, oh);
    return true;
}

// The descriptor sets are made each frame; vk_alloc_set recycles them (as in fsr.c).
static void write_image(VkDescriptorSet set, uint32_t binding, TexBackend *t, bool storage, bool linear) {
    static const VkComponentMapping id = {0};
    VkDescriptorImageInfo ii = {storage ? VK_NULL_HANDLE : vk_default_sampler(linear),
                                vk_image_view(t, VK_IMAGE_VIEW_TYPE_2D, t->format, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT),
                                VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = binding,
                              .descriptorCount = 1,
                              .descriptorType = storage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                        : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                              .pImageInfo = &ii};
    vkUpdateDescriptorSets(g_vk.dev, 1, &w, 0, NULL);
}

static void bind_quad(VkCommandBuffer cb, VkPipeline pipe, VkPipelineLayout layout, VkDescriptorSet set, uint32_t w, uint32_t h) {
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, NULL);
    VkViewport vp = {0, 0, (float)w, (float)h, 0, 1};
    VkRect2D sc = {{0, 0}, {w, h}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdDraw(cb, 3, 1, 0, 0);
}

bool vk_sgsr2_dispatch(VkCommandBuffer cb, TexBackend *color, TexBackend *depth, TexBackend *objvel, TexBackend *out,
                       const float clip[16], const CamRecord *cam, bool objvel_on, bool same, bool reset) {
    if (!vel_pipe) return false;
    if (!same_valid) same_frames = 0;
    same_frames = same ? same_frames + 1 : 0;
    same_valid = true;
    Sgsr2Params p;
    sgsr2_params(&p, clip, rw, rh, ow, oh, cam->jitter, cam->fovy, same, reset, same_frames);
    vkCmdUpdateBuffer(cb, ubo, 0, sizeof p, &p);  // the data travels with the command buffer: no CPU/GPU overwrite race
    vk_full_barrier(cb);
    // 1. encode the vectors of moving objects (the prep pass overwrites the camera's where an object vector is valid)
    {
        VkDescriptorSet set = vk_alloc_set(vel_set);
        write_image(set, 0, objvel, false, false);
        write_image(set, 1, &img_vel_enc, true, false);
        struct { float size[2]; uint32_t flags, reset; } pc = {{rw, rh}, objvel_on ? 1u : 0u, reset ? 1u : 0u};
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, vel_pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, vel_layout, 0, 1, &set, 0, NULL);
        vkCmdPushConstants(cb, vel_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
        vkCmdDispatch(cb, (rw + 7) / 8, (rh + 7) / 8, 1);
    }
    vk_full_barrier(cb);
    // 2. convert: depth is sampled as a depth image, the encoded velocity as RGBA16F
    {
        static const VkComponentMapping id = {0};
        VkDescriptorSet set = vk_alloc_set(convert_set);
        VkDescriptorBufferInfo buf = {ubo, 0, sizeof p};
        VkWriteDescriptorSet w[3] = {
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &buf},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 2, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER}};
        VkDescriptorImageInfo di[2] = {
            {vk_default_sampler(false),
             vk_image_view(depth, VK_IMAGE_VIEW_TYPE_2D, depth->format, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_DEPTH_BIT),
             VK_IMAGE_LAYOUT_GENERAL},
            {vk_default_sampler(false), vk_image_view(&img_vel_enc, VK_IMAGE_VIEW_TYPE_2D, img_vel_enc.format, 0, 1, 0, 1, id,
                                                      VK_IMAGE_ASPECT_COLOR_BIT),
             VK_IMAGE_LAYOUT_GENERAL}};
        w[1].pImageInfo = &di[0], w[2].pImageInfo = &di[1];
        vkUpdateDescriptorSets(g_vk.dev, 3, w, 0, NULL);
        VkImageView view = vk_image_view(&img_mda, VK_IMAGE_VIEW_TYPE_2D, img_mda.format, 0, 1, 0, 1, id,
                                         VK_IMAGE_ASPECT_COLOR_BIT);
        if (!view || !di[0].imageView) return false;
        VkRenderPassBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp_convert,
                                    .framebuffer = vk_framebuffer(rp_convert, &view, 1, rw, rh),
                                    .renderArea = {{0, 0}, {rw, rh}}};
        vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
        bind_quad(cb, convert_pipe, convert_layout, set, rw, rh);
        vkCmdEndRenderPass(cb);
    }
    vk_full_barrier(cb);
    // 3. upscale into the next RGBA16F history image, reading the previous one
    uint32_t next = hist_cur ^ 1;
    {
        static const VkComponentMapping id = {0};
        VkDescriptorSet set = vk_alloc_set(upscale_set);
        VkDescriptorBufferInfo buf = {ubo, 0, sizeof p};
        VkWriteDescriptorSet w[4] = {
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &buf},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 2, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 3, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER}};
        VkDescriptorImageInfo di[3] = {
            {vk_default_sampler(true),  // PrevOutput: bilinear at the reprojected UV
             vk_image_view(&img_hist[hist_cur], VK_IMAGE_VIEW_TYPE_2D, img_hist[0].format, 0, 1, 0, 1, id,
                           VK_IMAGE_ASPECT_COLOR_BIT),
             VK_IMAGE_LAYOUT_GENERAL},
            {vk_default_sampler(true), vk_image_view(&img_mda, VK_IMAGE_VIEW_TYPE_2D, img_mda.format, 0, 1, 0, 1, id,
                                                      VK_IMAGE_ASPECT_COLOR_BIT),
             VK_IMAGE_LAYOUT_GENERAL},
            {vk_default_sampler(false), vk_image_view(color, VK_IMAGE_VIEW_TYPE_2D, color->format, 0, 1, 0, 1, id,
                                                      VK_IMAGE_ASPECT_COLOR_BIT),
             VK_IMAGE_LAYOUT_GENERAL}};
        w[1].pImageInfo = &di[0], w[2].pImageInfo = &di[1], w[3].pImageInfo = &di[2];
        vkUpdateDescriptorSets(g_vk.dev, 4, w, 0, NULL);
        VkImageView view = vk_image_view(&img_hist[next], VK_IMAGE_VIEW_TYPE_2D, img_hist[0].format, 0, 1, 0, 1, id,
                                         VK_IMAGE_ASPECT_COLOR_BIT);
        if (!view || !di[0].imageView || !di[2].imageView) return false;
        VkRenderPassBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp_upscale,
                                    .framebuffer = vk_framebuffer(rp_upscale, &view, 1, ow, oh),
                                    .renderArea = {{0, 0}, {ow, oh}}};
        vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
        bind_quad(cb, upscale_pipe, upscale_layout, set, ow, oh);
        vkCmdEndRenderPass(cb);
    }
    vk_full_barrier(cb);
    // 4. write the clamped LDR result into H' (the game's stand-in at the output size)
    if (copy_fmt != out->format) {  // the picture's format can change with the window (rebuild as hdr.c does)
        copy_fmt = out->format;
        rp_copy = vk_render_pass(&copy_fmt, 1, VK_FORMAT_UNDEFINED, 1);
        copy_pipe = graphics_pipe(rp_copy, copy_layout, copy_fs);
    }
    if (!copy_pipe) return false;
    {
        static const VkComponentMapping id = {0};
        VkDescriptorSet set = vk_alloc_set(copy_set);
        write_image(set, 0, &img_hist[next], false, false);
        VkImageView view = vk_image_view(out, VK_IMAGE_VIEW_TYPE_2D, out->format, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
        if (!view) return false;
        VkRenderPassBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp_copy,
                                    .framebuffer = vk_framebuffer(rp_copy, &view, 1, ow, oh),
                                    .renderArea = {{0, 0}, {ow, oh}}};
        vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
        bind_quad(cb, copy_pipe, copy_layout, set, ow, oh);
        vkCmdEndRenderPass(cb);
    }
    hist_cur = next;
    vk_full_barrier(cb);
    return true;
}
