// Vulkan 1.2 backend for the NVN layer. Baseline: what an AMD GCN card with a 2020 driver offers (see
// docs/STATUS.md): no dynamic rendering, no synchronization2, no extended dynamic state, no push descriptors.
// Files: device.c (instance, device, swapchain, presentation), textures.c (images, change-detected uploads from
// guest memory, views, samplers), shaders.c (GLSL -> SPIR-V, reflection, built-in shaders), exec.c (command lists:
// render passes, draws, clears, copies, compute). Structure and behaviour follow LAS Recomp's Vulkan backend.
//
// Synchronisation model: every image lives in VK_IMAGE_LAYOUT_GENERAL, and a full memory barrier separates render
// passes, copies and dispatches. Render passes are VkRenderPass objects (cached by attachment formats; LOAD / STORE,
// GENERAL in and out) with framebuffers cached by their views. Pipelines bake all state but viewport, scissor, depth
// bias, blend constants and stencil masks / reference. Submissions are numbered; a timeline semaphore signals each
// number, which is what NVN fences (renderer_fence_seq / renderer_wait_seq) wait for.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#if __has_include(<vma/vk_mem_alloc.h>)
#include <vma/vk_mem_alloc.h>
#else
#include <vk_mem_alloc.h>
#endif

#include "../../config.h"
#include "../nvn.h"
#include "../maxwell/shader.h"

#define VK_CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) FATAL("%s failed: %d (%s:%d)\n", #x, r_, __FILE__, __LINE__); } while (0)

typedef struct {
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue queue;
    uint32_t qfam;
    VmaAllocator vma;
    VkPhysicalDeviceProperties props;
    VkPipelineCache pcache;
    VkSemaphore timeline;      // value = last completed submission number
    VkCommandPool pool;        // render thread only
    bool divisor, demote, custom_border, depth_clamp, stencil_export;
    volatile uint32_t msaa;    // samples for the 3D scene's passes (settings MSAA, as far as the GPU allows): 1 = off
    uint32_t msaa_counts;      // the sample counts MSAA can use (bits 2, 4, 8)
    bool fsr_fp16, fsr_storage_wo_format;  // FSR (fsr.c): shaderFloat16 / shaderStorageImageWriteWithoutFormat enabled
    // window
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkFormat sc_format;
    VkExtent2D sc_extent;
    uint32_t sc_count;
    VkImage sc_images[8];
    VkImageView sc_views[8];
} VkG;
extern VkG g_vk;

// One lock for everything that records or submits (the game may submit from more than one thread).
void vk_lock(void);
void vk_unlock(void);

// exit() has begun (set by device.c's atexit handler): driver calls are no longer safe, the process leaves
// through _exit. Late guest threads park at vk_lock and drop their GPU waits.
bool vk_exiting(void);

// ---- submissions (exec.c)
// Work is recorded into the current command buffer; uploads go into a separate one that runs first.
VkCommandBuffer vk_cmd(void);         // current command buffer (outside any render pass unless exec.c began one)
VkCommandBuffer vk_upload_cmd(void);  // upload command buffer of the pending submission
uint64_t vk_pending_seq(void);        // number the pending submission will get
uint64_t vk_completed_seq(void);
void vk_wait_seq(uint64_t seq);
void vk_full_barrier(VkCommandBuffer cb);
void vk_end_pass(void);

// Barriers name every stage and access type explicitly: AMD's 2020 Windows driver for GCN1 (R9 200) does not flush the
// colour-attachment cache for a barrier that only says ALL_COMMANDS / MEMORY_WRITE, so later passes read stale 8x8
// tiles of the image just rendered (the light-shaft "triangle" and blocky bloom shimmer).
#define VK_STAGES_ALL                                                                                                     \
    (VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |     \
     VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | \
     VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |                           \
     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT)
#define VK_ACCESS_ALL_WRITES                                                                                         \
    (VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | \
     VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT)
#define VK_ACCESS_ALL                                                                                                     \
    (VK_ACCESS_ALL_WRITES | VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |                            \
     VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT |               \
     VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT |                       \
     VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_INPUT_ATTACHMENT_READ_BIT)
void vk_submit_extra(VkSemaphore wait, VkSemaphore signal);  // submit (with binary semaphores for presentation)

// Transient GPU-visible memory that lives until the submission it was allocated for completes.
typedef struct { VkBuffer buf; uint64_t off; void *ptr; } VkSpan;
VkSpan vk_transient(uint64_t size, uint64_t align);
VkSpan vk_transient_copy(const void *data, uint64_t size, uint64_t align);
VkSpan vk_readback(uint64_t size);  // host-cached GPU -> CPU staging (read right after the copy completes)
VkDescriptorSet vk_alloc_set(VkDescriptorSetLayout layout);

// Render passes / framebuffers (exec.c): all attachments LOAD / STORE in GENERAL layout (samples > 1, MSAA: in the
// attachment layouts). depth: the depth/stencil format (UNDEFINED: none).
VkRenderPass vk_render_pass(const VkFormat *color, uint32_t ncolor, VkFormat depth, uint32_t samples);
VkFormat vk_depth24_stencil8(void);  // the format DEPTH24_STENCIL8 is made as (D32_SFLOAT_S8_UINT on AMD)
VkFramebuffer vk_framebuffer(VkRenderPass rp, const VkImageView *views, uint32_t n, uint32_t w, uint32_t h);
void vk_framebuffers_forget_view(VkImageView v);  // (textures.c: a view is going away)

// ---- textures (textures.c)
typedef struct TexBackend {
    VkImage image;
    VmaAllocation mem;
    VkFormat format;
    VkImageViewType base_type;
    uint32_t width, height, depth, layers, levels, samples;
    VkImageAspectFlags aspect;
    bool gpu_owned, uploaded, dynamic;
    uint64_t data_hash, sample_hash, checked_frame, full_frame;
    uint32_t wgen;       // page write tracking generation at the last upload check
    bool presented;
    void *pending_rgba;  // host-produced pixels waiting to be uploaded on the render thread
    uint32_t pending_w, pending_h, pending_stride;
    uint8_t astc_bw, astc_bh;  // ASTC without GPU support: decoded to RGBA8 on upload (runtime/nvn/astc.c)
    bool astc_srgb;
    struct TexBackend *ms;  // MSAA: the multisampled image this one is rendered through (textures.c vk_tex_ms)
    bool ms_valid;          // ms holds this image's contents (its last write was a pass through ms)
    bool ms_dirty;          // ms is newer than this image: resolved when this one is next read (exec.c vk_ms_resolve)
    VkFormat ms_fmt;        // the format ms is rendered as
    bool ms_sampled;        // (of a multisampled image) shaders may read it too: SMAA S2x / 4x's S2x (smaa.c)
} TexBackend;

TexBackend *vk_tex(NvnTexture *t, bool for_render);  // backend image (uploads CPU contents unless for_render)
VkFormat vk_format(uint32_t nvn_format);
bool vk_format_alpha_one(uint32_t nvn_format);
bool renderer_format_is_depth(uint32_t f);
bool renderer_format_has_stencil(uint32_t f);
void renderer_mark_gpu_owned(NvnTexture *t);
void renderer_mark_dynamic(NvnTexture *t);  // its CPU-written contents change often: checked every frame
// View of t (optionally through an NVN texture view) for rendering or sampling.
VkImageView vk_view(NvnTexture *t, const NvnTextureView *v, bool has_view, bool render, int want_textype);
VkImageView vk_image_view(TexBackend *b, VkImageViewType type, VkFormat fmt, uint32_t lv0, uint32_t nlv, uint32_t ly0,
                          uint32_t nly, VkComponentMapping sw, VkImageAspectFlags aspect);
VkImageView vk_dummy_view(int textype);
TexBackend *vk_tex_ms(TexBackend *b, uint32_t samples);  // b's multisampled companion (created on first use)
TexBackend *vk_tex_ms_readable(TexBackend *m);  // a sampled copy for its resolve
void vk_ms_resolve(TexBackend *b);  // (exec.c) b up to date with its multisampled companion before b is read
VkSampler vk_sampler(NvnSamplerDesc *d);
VkSampler vk_default_sampler(bool linear);
void vk_image_init_layout(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect);
// Destruction once every submission that may use the object has completed.
void vk_defer_image(VkImage img, VmaAllocation mem);
void vk_defer_view(VkImageView v);
void vk_defer_framebuffer(VkFramebuffer f);
void vk_defer_buffer(VkBuffer b, VmaAllocation mem);
void vk_collect_dead(void);
void vk_views_forget(TexBackend *b);  // drop (deferred) the cached views of an image being replaced
uint64_t vk_frame_counter(void);
// Guest memory [addr, addr + size) (guest addresses) changed since generation `since`? 0 no, 1 yes, -1 unknown.
int vk_pages_changed(uint64_t addr, uint64_t size, uint32_t since, uint32_t *now);

// ---- shaders (shaders.c)
VkShaderModule vk_compile_glsl(const char *src, int stage, const char *name);  // NULL on failure (logged)
VkShaderModule vk_builtin_shader(const char *name);  // internal shaders (clear, present)

// ---- anti-aliasing in place of the game's FXAA pass (exec.c fxaa_replaced): the input that pass samples and the
// colour target it draws into (format, size w x h), both in GENERAL layout; recorded outside a render pass
// (src_image, level: the input's image and the level of the target dst views, for copies)
typedef struct { VkImageView src, dst; VkImage dst_image, src_image; VkFormat format; uint32_t w, h, level; } VkPostAa;
bool vk_smaa(const VkPostAa *t, int quality);  // SMAA 1x, SMAA_* preset (smaa.c); false: not drawn
bool vk_cmaa2(const VkPostAa *t, int quality, bool extra_sharp);  // CMAA2, CMAA2_* preset (cmaa2.c); false: not drawn
// SMAA T2x in place (t->src and t->dst the same image; fsr.c at the start of the HUD): velocity = the motion vectors
// (previous position - current, in pixels); phase: the frame's jitter (camjit_t2x_phase), -1 for none (then SMAA 1x);
// reactive (or VK_NULL_HANDLE): where history is dropped (0..1, fsr.c's reactive mask); history false: the last frame
// does not continue into this one (a cut); spatial false: SMAA 4x, S2x is in the picture already (no edges here, and
// nothing at all without a jitter). false: not drawn.
bool vk_smaa_t2x(const VkPostAa *t, int quality, VkImageView velocity, VkImageView reactive, int phase, bool history,
                 bool spatial);
// SMAA S2x (4x's spatial half) as the resolve of a multisampled image (exec.c vk_ms_resolve): ms = a view of the 2x
// image in SHADER_READ_ONLY_OPTIMAL layout, t->dst = the resolved image (t->src unused); phase: the frame's 4x jitter
// (camjit_t2x_phase), -1 for none (S2x's own subsample indices). false: not drawn (resolve it as usual).
bool vk_smaa_s2x(const VkPostAa *t, VkImageView ms, int quality, int phase);

// ---- device / window (device.c)
void vk_save_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h);
