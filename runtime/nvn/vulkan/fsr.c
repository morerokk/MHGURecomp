// FSR 3.1 upscaling of the game's 3D picture ([Upscaler] FSR = On; third_party/fsr-vulkan: AMD FidelityFX SDK 1.1.4,
// FSR 3.1.4 upscaler, through its portable C wrapper). The game renders at g_render_size (the output size / the
// FSRQuality ratio, config.c); FSR rebuilds the picture at g_fsr_output, and the HUD is drawn at that size on top.
//
// A frame of the game (seen with MHGU_CONTROL "dump", a quest, 1080p): shadow map, the scene's passes into the
// render-size depth target D, post passes (bloom chain, depth of field...), then into H, the full-screen image:
// the post-processed scene (FXAA, bloom added: small full-screen draws that sample other render targets), the HUD
// (sprites from ordinary textures), and last a draw that samples H and writes the window texture P (brightness /
// gamma) that the game presents.
//  - H is learnt each frame: the first texture the last draw before a present sampled (P is the presented texture).
//  - The start of the HUD: the first draw into H that is not a post pass (a draw of <= 6 vertices that samples a
//    render target other than H), or else the draw into a window texture that samples H. There, before that draw:
//      scene frame (D had >= 16 depth-writing draws this frame and the main camera was captured): prep (D -> R32F
//        depth, camera motion vectors from it and the current / previous camera: fsr_prep below), then FSR with H as
//        its colour input, into H' (H's stand-in at the output size);
//      any other frame (menus, loading, a camera without the hook): H scaled to H' (linear), no FSR.
//    From there to the present H, P and the depth target drawn with them are replaced by stand-ins at the output size
//    (vk_fsr_map: as render targets in exec.c begin_pass, viewport and scissor scaled; as textures in texture_info),
//    so the HUD and the final draw happen at full resolution, and the present shows P' (vk_fsr_present).
//  - Jitter (runtime/hle/camera_jitter.c) runs while frames are scene frames; after 3 frames in a row that are not,
//    it stops (menus: no FSR, a steady picture), and starts again with the next scene frame.
//  - The camera of a frame: the capture whose jittered projection the frame's scene draws read from their uniform
//    buffers (found by its m[8] / m[9]); none found (no jitter yet): the newest capture.
//
//   MHGU_FSR_LOG=1     a line per second: frames with FSR / scaled, where the HUD started, the camera match
//   MHGU_FSR_DEBUG=1   FSR's own checks of its inputs (FFX debug checking; messages in the log)
//   MHGU_FSR_MVCHECK=1 (with MHGU_FSR_LOG) how well the camera motion vectors match the picture (mv_check)
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include <ffx_vk_portable.h>

#include "../../camera_jitter.h"
#include "../../kernel.h"
#include "fsr.h"
#include "fsr4.h"
#include "shaders.h"

bool g_fsr_on;
bool g_temporal_on;

static uint32_t rw, rh, ow, oh;  // render size, output size (SMAA T2x: both the render size)
static bool t2x_on, t2x_ready, t2x_failed;  // SMAA T2x runs (fsr.h g_temporal_on); its images and pipelines are made / could not be
static bool logging;

// ================================================================== stand-ins (the output-size images)
typedef struct {
    NvnTexture fake;   // an NvnTexture of its own: exec.c and textures.c use it as any other render target
    uint32_t fmt, w, h;  // what it stands in for: NVN format, size
} Stand;
static Stand st_h, st_p, st_d;  // the full-screen image H, the window texture P, the depth target

static TexBackend *stand_image(NvnTexture *of, uint32_t w, uint32_t h, bool storage) {
    TexBackend *src = of->backend;
    bool depth = src->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                              (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
                              (storage ? VK_IMAGE_USAGE_STORAGE_BIT : 0);
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = depth ? 0 : VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
                            .imageType = VK_IMAGE_TYPE_2D, .format = src->format, .extent = {w, h, 1}, .mipLevels = 1,
                            .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = usage, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    TexBackend *b = calloc(1, sizeof *b);
    if (vmaCreateImage(g_vk.vma, &ci, &ai, &b->image, &b->mem, NULL) != VK_SUCCESS) {
        LOG("[fsr] could not create a %ux%u image (format %d)\n", w, h, src->format);
        free(b);
        return NULL;
    }
    b->format = src->format;
    b->base_type = VK_IMAGE_VIEW_TYPE_2D;
    b->width = w, b->height = h, b->depth = b->layers = b->levels = b->samples = 1;
    b->aspect = src->aspect;
    b->gpu_owned = true;
    VkCommandBuffer cb = vk_upload_cmd();
    vk_image_init_layout(cb, b->image, b->aspect);
    VkImageSubresourceRange rg = {b->aspect, 0, 1, 0, 1};
    if (depth) {
        VkClearDepthStencilValue far = {1.0f, 0};
        vkCmdClearDepthStencilImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, &far, 1, &rg);
    } else {
        VkClearColorValue zero = {{0, 0, 0, 0}};
        vkCmdClearColorImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &rg);
    }
    vk_full_barrier(cb);
    return b;
}

// The stand-in of `of` at w x h (made, or remade when `of` is another kind of texture than before).
static NvnTexture *stand(Stand *s, NvnTexture *of, uint32_t w, uint32_t h, bool storage) {
    if (!of->backend) return NULL;
    if (s->fake.backend && s->fmt == of->format && s->w == of->width && s->h == of->height) return &s->fake;
    if (s->fake.backend) {
        TexBackend *old = s->fake.backend;
        vk_views_forget(old);
        vk_defer_image(old->image, old->mem);
        free(old);
        s->fake.backend = NULL;
    }
    TexBackend *b = stand_image(of, w, h, storage);
    if (!b) return NULL;
    s->fake = *of;
    s->fake.width = w, s->fake.height = h, s->fake.depth = 1, s->fake.levels = 1;
    s->fake.addr = 0;  // (no guest memory: never uploaded)
    s->fake.pool = NULL;
    s->fake.backend = b;
    snprintf(s->fake.label, sizeof s->fake.label, "fsr stand-in");
    s->fmt = of->format, s->w = of->width, s->h = of->height;
    LOG("[fsr] %ux%u stand-in for the %ux%u %s (format %d)\n", w, h, of->width, of->height,
        s == &st_h ? "full-screen image" : s == &st_p ? "window texture" : "depth target", b->format);
    return &s->fake;
}

// ================================================================== prep: depth copy + camera motion vectors
// (render size) depth_out = the scene depth as R32F (FSR never sees the depth / stencil image itself); mv_out = where
// each texel's point was in the previous frame, minus where it is now, in render pixels (FSR's convention: current
// position + motion = previous position), from the unjittered matrices.
// Pixel p: centre p + 0.5; NDC x = 2 (p.x + 0.5) / w - 1, y = 1 - 2 (p.y + 0.5) / h (the translator's clip space has +y
// at the top: exec.c set_viewport); z = the depth value (0 near .. 1 far). The game's matrices are row-vector
// (clip = v * M); reproj = inverse(view * proj) * (view_prev * proj_prev), uploaded row by row, so in GLSL (columns)
// `reproj * c` is the row-vector product c * reproj.
static const char PREP_GLSL[] =
    "#version 450\n"
    "layout(local_size_x = 8, local_size_y = 8) in;\n"
    "layout(set = 0, binding = 0) uniform sampler2D depth_in;\n"
    "layout(set = 0, binding = 1, r32f) uniform writeonly image2D depth_out;\n"
    "layout(set = 0, binding = 2, rg32f) uniform writeonly image2D mv_out;\n"
    "layout(push_constant) uniform Push { mat4 reproj; vec4 flags; } pc;  // flags.x: 1 = motion vectors, 0 = none\n"
    "void main() {\n"
    "    ivec2 p = ivec2(gl_GlobalInvocationID.xy);\n"
    "    ivec2 size = imageSize(depth_out);\n"
    "    if (p.x >= size.x || p.y >= size.y) return;\n"
    "    float d = texelFetch(depth_in, p, 0).r;\n"
    "    imageStore(depth_out, p, vec4(d, 0.0, 0.0, 0.0));\n"
    "    vec2 mv = vec2(0.0);\n"
    "    if (pc.flags.x != 0.0) {\n"
    "        vec2 sz = vec2(size), cur = vec2(p) + 0.5;\n"
    "        vec4 q = pc.reproj * vec4(2.0 * cur.x / sz.x - 1.0, 1.0 - 2.0 * cur.y / sz.y, d, 1.0);\n"
    "        if (q.w > 1e-6) {\n"
    "            vec2 ndc = q.xy / q.w;\n"
    "            mv = vec2((ndc.x + 1.0) * 0.5 * sz.x, (1.0 - ndc.y) * 0.5 * sz.y) - cur;\n"
    "            if (isnan(mv.x) || isinf(mv.x) || isnan(mv.y) || isinf(mv.y)) mv = vec2(0.0);\n"
    "            mv = clamp(mv, vec2(-16384.0), vec2(16384.0));\n"
    "        }\n"
    "    }\n"
    "    imageStore(mv_out, p, vec4(mv, 0.0, 0.0));\n"
    "}\n";

static VkDescriptorSetLayout prep_set_layout;
static VkPipelineLayout prep_layout;
static VkPipeline prep_pipe;
static TexBackend img_depth, img_mv;  // R32F depth, RG32F motion vectors (render size)

static TexBackend img_mvf;     // the motion vectors FSR gets (RG32F): object vectors over the camera's
static TexBackend img_vel;     // object vectors (RGBA16F: rg vector, b 1 = valid)
static TexBackend img_react;   // reactive mask (R32F)
static TexBackend img_sil[2];  // the moving-object silhouette, this frame / the last (R32F)

// ---- finish (compute, render size): the motion vectors FSR gets and its reactive mask.
//  - mv = the object vector where the velocity pass (object motion, below) left a valid one, else the camera's.
//  - reactive, from the game's moving-object mask (the first colour target drawn with the scene depth in a frame:
//    characters, monsters and their cast shadows, in its blue (and green) channel): sil = smoothstep(0.5/255, 0.25,
//    mask). It enters the reactive mask only where the silhouette CHANGED: outside the min / max of the previous
//    frame's silhouette in the 3x3 texels around where this texel was (texel + mv). That is the ghost trail behind a
//    moving object and its new leading edge; a standing object (or one moving less than a texel) lies inside its own
//    box and keeps FSR's full accumulation, so it does not alias more. react = strength x that, capped to vel_cap on
//    texels with valid object vectors (their vectors are right), blended by the share of valid texels in the 3x3 so
//    edges do not switch between the two from frame to frame.
static const char FINISH_GLSL[] =
    "#version 450\n"
    "layout(local_size_x = 8, local_size_y = 8) in;\n"
    "layout(set = 0, binding = 0) uniform sampler2D cam_mv;\n"
    "layout(set = 0, binding = 1) uniform sampler2D vel;\n"
    "layout(set = 0, binding = 2) uniform sampler2D mask;\n"
    "layout(set = 0, binding = 3) uniform sampler2D sil_prev;\n"
    "layout(set = 0, binding = 4, rg32f) uniform writeonly image2D mv_out;\n"
    "layout(set = 0, binding = 5, r32f) uniform writeonly image2D react_out;\n"
    "layout(set = 0, binding = 6, r32f) uniform writeonly image2D sil_out;\n"
    "layout(set = 0, binding = 7) uniform sampler2D tiles;\n"
    "layout(push_constant) uniform Push { float strength; float vel_cap; uint flags; float pad; } pc;\n"
    "// flags: 1 object vectors, 2 mask, 4 previous silhouette valid, 8 animated-surface tiles\n"
    "void main() {\n"
    "    ivec2 p = ivec2(gl_GlobalInvocationID.xy), size = imageSize(mv_out);\n"
    "    if (p.x >= size.x || p.y >= size.y) return;\n"
    "    vec2 mv = texelFetch(cam_mv, p, 0).rg;\n"
    "    float cov = 0.0;\n"
    "    if ((pc.flags & 1u) != 0u) {\n"
    "        vec4 v = texelFetch(vel, p, 0);\n"
    "        if (v.b > 0.5) mv = v.rg;\n"
    "        for (int dy = -1; dy <= 1; dy++)\n"
    "            for (int dx = -1; dx <= 1; dx++)\n"
    "                cov += texelFetch(vel, clamp(p + ivec2(dx, dy), ivec2(0), size - 1), 0).b > 0.5 ? 1.0 : 0.0;\n"
    "    }\n"
    "    imageStore(mv_out, p, vec4(mv, 0.0, 0.0));\n"
    "    float sil = 0.0;\n"
    "    if ((pc.flags & 2u) != 0u) {\n"
    "        ivec2 ms = textureSize(mask, 0);\n"
    "        vec4 m = texelFetch(mask, clamp(ivec2((vec2(p) + 0.5) * vec2(ms) / vec2(size)), ivec2(0), ms - 1), 0);\n"
    "        sil = smoothstep(0.5 / 255.0, 0.25, max(m.g, m.b));\n"
    "    }\n"
    "    imageStore(sil_out, p, vec4(sil, 0.0, 0.0, 0.0));\n"
    "    float r = 0.0;\n"
    "    ivec2 q = ivec2(floor(vec2(p) + 0.5 + mv));\n"
    "    if ((pc.flags & 4u) != 0u && q.x >= 0 && q.y >= 0 && q.x < size.x && q.y < size.y) {\n"
    "        float lo = 1.0, hi = 0.0;\n"
    "        for (int dy = -1; dy <= 1; dy++)\n"
    "            for (int dx = -1; dx <= 1; dx++) {\n"
    "                float s = texelFetch(sil_prev, clamp(q + ivec2(dx, dy), ivec2(0), size - 1), 0).r;\n"
    "                lo = min(lo, s);\n"
    "                hi = max(hi, s);\n"
    "            }\n"
    "        r = max(max(sil - hi, lo - sil), 0.0);\n"
    "    }\n"
    "    float react = pc.strength * r;\n"
    "    react = mix(react, min(react, pc.vel_cap), cov / 9.0);\n"
    "    if ((pc.flags & 8u) != 0u) {  // animated surfaces (ANIM_GLSL)\n"
    "        vec4 t = texelFetch(tiles, min(p / 8, textureSize(tiles, 0) - 1), 0);\n"
    "        if (t.r > 0.2 || t.b > 0.2) react = max(react, 0.95);\n"
    "    }\n"
    "    imageStore(react_out, p, vec4(react, 0.0, 0.0, 0.0));\n"
    "}\n";

static VkDescriptorSetLayout finish_set_layout;
static VkPipelineLayout finish_layout;
static VkPipeline finish_pipe;

// (debug, MHGU_FSR_MVCHECK=1) how well the motion vectors explain the picture, measured on this frame's input against
// the previous frame's (bilinear): the luma difference at the texel itself (no motion) and at texel + a vector + jd,
// jd = the change of the jitter's picture shift (the inputs are jittered, the vectors are not: the point at p was at
// p + mv + jd in the previous input). Class 0: texels the camera moves (> 1 px): same / camera / FSR's vectors. Class
// 1: texels of the game's moving-object mask: the same three. Class 2: texels with a valid object vector that differs
// from the camera's by > 0.5 px (moving geometry): same / camera / object / object without jd / object with y negated.
// sums: [0..3] class 0, [4..7] class 1, [8..13] class 2 (count first, then the differences x 1000).
static const char MVCHECK_GLSL[] =
    "#version 450\n"
    "layout(local_size_x = 8, local_size_y = 8) in;\n"
    "layout(set = 0, binding = 0) uniform sampler2D cur;\n"
    "layout(set = 0, binding = 1) uniform sampler2D prev;\n"
    "layout(set = 0, binding = 2) uniform sampler2D cam_mv;\n"
    "layout(set = 0, binding = 3) uniform sampler2D fin_mv;\n"
    "layout(set = 0, binding = 4) uniform sampler2D sil;\n"
    "layout(set = 0, binding = 5) uniform sampler2D vel;\n"
    "layout(set = 0, binding = 6) buffer Sums { uint s[16]; } sums;\n"
    "layout(push_constant) uniform Push { vec2 jd; } pc;\n"
    "shared uint acc[16];\n"
    "float c, luma0;\n"
    "vec2 uv, sz;\n"
    "float luma(vec3 v) { return dot(v, vec3(0.299, 0.587, 0.114)); }\n"
    "uint err(vec2 m) { return uint(abs(c - luma(textureLod(prev, uv + m / sz, 0.0).rgb)) * 1000.0); }\n"
    "void main() {\n"
    "    if (gl_LocalInvocationIndex < 16) acc[gl_LocalInvocationIndex] = 0u;\n"
    "    barrier();\n"
    "    ivec2 p = ivec2(gl_GlobalInvocationID.xy), isz = textureSize(cur, 0);\n"
    "    if (p.x < isz.x && p.y < isz.y) {\n"
    "        sz = vec2(isz);\n"
    "        vec2 m0 = texelFetch(cam_mv, p, 0).xy, m1 = texelFetch(fin_mv, p, 0).xy;\n"
    "        vec4 v = texelFetch(vel, p, 0);\n"
    "        uv = (vec2(p) + 0.5) / sz;\n"
    "        c = luma(texelFetch(cur, p, 0).rgb);\n"
    "        uint same = err(vec2(0.0));\n"
    "        if (dot(m0, m0) > 1.0) {\n"
    "            atomicAdd(acc[0], 1u); atomicAdd(acc[1], same); atomicAdd(acc[2], err(m0 + pc.jd)); atomicAdd(acc[3], err(m1 + pc.jd));\n"
    "        }\n"
    "        if (texelFetch(sil, p, 0).r > 0.5) {\n"
    "            atomicAdd(acc[4], 1u); atomicAdd(acc[5], same); atomicAdd(acc[6], err(m0 + pc.jd)); atomicAdd(acc[7], err(m1 + pc.jd));\n"
    "        }\n"
    "        if (v.b > 0.5 && length(v.rg - m0) > 0.5) {\n"
    "            atomicAdd(acc[8], 1u); atomicAdd(acc[9], same); atomicAdd(acc[10], err(m0 + pc.jd));\n"
    "            atomicAdd(acc[11], err(v.rg + pc.jd)); atomicAdd(acc[12], err(v.rg));\n"
    "            atomicAdd(acc[13], err(vec2(v.r, -v.g) + pc.jd));\n"
    "        }\n"
    "    }\n"
    "    barrier();\n"
    "    if (gl_LocalInvocationIndex < 16) atomicAdd(sums.s[gl_LocalInvocationIndex], acc[gl_LocalInvocationIndex]);\n"
    "}\n";

// ---- animated surfaces ([Upscaler] AnimSurf): surfaces whose picture changes while they do not move (banners and
// flags animated through their texture or texture coordinates, flowing water). Neither the geometry nor any motion
// vector shows that change, so FSR keeps blending the old picture into the new one (ghosting). Per 8x8 tile (one
// workgroup, render size), on texels that do not move as objects (no valid object vector, or one within 0.25 px of the
// camera's):
//  - d = the smallest, over a 3x3 search (+-1 px) around where the texel was (texel + vector + jd), of the largest
//    channel difference between the 4x4 mean of the current input and that of the previous input. An edge or thin line
//    that moved by a fraction of a pixel finds itself within the search; a texture that changed does not.
//  - the tile is flagged when the mean d > thr and >= 40% of its evaluated texels (>= 16) have d > thr, and its linear
//    depth range is < 5% of its nearest depth (a flat surface: silhouettes, ropes and rigging fail).
// Tile texel (RGBA8): r = weight now (1 when also flagged within the last 12 frames: a repeating animation; 0.35 the
// first time; 0 not flagged), g = frames since the last flag / 255, b = r of the previous frame.
// The finish pass raises the reactive mask to 0.95 on tiles with r or b > 0.2; after FSR the bypass pass mixes the
// current input (Catmull-Rom, jitter removed) into the output there: 0.85 x r, 0.5 x b (feathered: tiles sampled
// bilinearly). Counters: [0] flagged tiles, [1] evaluated tiles, [2] flags rejected as not flat, [3] full weight.
static const char ANIM_GLSL[] =
    "#version 450\n"
    "layout(local_size_x = 8, local_size_y = 8) in;\n"
    "layout(set = 0, binding = 0) uniform sampler2D cur;\n"
    "layout(set = 0, binding = 1) uniform sampler2D prev;\n"
    "layout(set = 0, binding = 2) uniform sampler2D vel;\n"
    "layout(set = 0, binding = 3) uniform sampler2D cam_mv;\n"
    "layout(set = 0, binding = 4) uniform sampler2D tile_prev;\n"
    "layout(set = 0, binding = 5, rgba8) uniform writeonly image2D tile_out;\n"
    "layout(set = 0, binding = 6) uniform sampler2D depth;\n"
    "layout(set = 0, binding = 7) buffer Stats { uint s[8]; } st;\n"
    "layout(set = 0, binding = 8) uniform sampler2D mask;\n"
    "layout(push_constant) uniform Push { uint flags; float jdx, jdy, thr, p22, p32, pad0, pad1; } pc;\n"
    "// flags: 1 previous input valid, 2 object vectors, 4 counters, 8 moving-object mask (its texels are left out)\n"
    "shared uint t_sum, t_n, t_hot, t_zmin, t_zmax, t_char;\n"
    "vec3 box4(sampler2D t, vec2 c, vec2 tx) {  // 4 bilinear taps at texel corners = the 4x4 mean around c\n"
    "    return 0.25 * (textureLod(t, (c + vec2(-1.0, -1.0)) * tx, 0.0).rgb + textureLod(t, (c + vec2(1.0, -1.0)) * tx, 0.0).rgb +\n"
    "                   textureLod(t, (c + vec2(-1.0, 1.0)) * tx, 0.0).rgb + textureLod(t, (c + vec2(1.0, 1.0)) * tx, 0.0).rgb);\n"
    "}\n"
    "void main() {\n"
    "    if (gl_LocalInvocationIndex == 0u) { t_sum = 0u; t_n = 0u; t_hot = 0u; t_zmin = 0x7f7fffffu; t_zmax = 0u; t_char = 0u; }\n"
    "    barrier();\n"
    "    ivec2 size = textureSize(cur, 0), p = ivec2(gl_GlobalInvocationID.xy);\n"
    "    vec2 tx = 1.0 / vec2(size);\n"
    "    if (p.x < size.x && p.y < size.y) {\n"
    "        ivec2 lp = ivec2(gl_LocalInvocationID.xy);\n"
    "        ivec2 ext = ivec2(lp.x == 0 ? -1 : lp.x == 7 ? 1 : 0, lp.y == 0 ? -1 : lp.y == 7 ? 1 : 0);\n"
    "        float z0 = pc.p32 / (texelFetch(depth, p, 0).r + pc.p22);\n"
    "        float z1 = pc.p32 / (texelFetch(depth, clamp(p + ext, ivec2(0), size - 1), 0).r + pc.p22);\n"
    "        if (z0 > 0.0 && z1 > 0.0) { atomicMin(t_zmin, floatBitsToUint(min(z0, z1))); atomicMax(t_zmax, floatBitsToUint(max(z0, z1))); }\n"
    "        if ((pc.flags & 1u) != 0u) {\n"
    "            vec4 v = texelFetch(vel, p, 0);\n"
    "            vec2 cm = texelFetch(cam_mv, p, 0).rg;\n"
    "            bool obj = (pc.flags & 2u) != 0u && v.b > 0.5;\n"
    "            bool character = false;\n"
    "            if ((pc.flags & 8u) != 0u) {\n"
    "                ivec2 ms = textureSize(mask, 0);\n"
    "                vec4 m = texelFetch(mask, clamp(ivec2((vec2(p) + 0.5) * vec2(ms) / vec2(size)), ivec2(0), ms - 1), 0);\n"
    "                character = max(m.g, m.b) > 0.1;\n"
    "                if (character) atomicAdd(t_char, 1u);\n"
    "            }\n"
    "            if (!character && (!obj || length(v.rg - cm) < 0.25)) {\n"
    "                vec2 c = vec2(p) + 0.5, q = c + (obj ? v.rg : cm) + vec2(pc.jdx, pc.jdy);\n"
    "                if (q.x >= 3.0 && q.y >= 3.0 && q.x <= float(size.x) - 3.0 && q.y <= float(size.y) - 3.0) {\n"
    "                    vec3 a = box4(cur, c, tx);\n"
    "                    float d = 1e9;\n"
    "                    for (int dy = -2; dy <= 2; dy++)  // (+-1 px in half-pixel steps)\n"
    "                        for (int dx = -2; dx <= 2; dx++) {\n"
    "                            vec3 e = abs(a - box4(prev, q + 0.5 * vec2(dx, dy), tx));\n"
    "                            d = min(d, max(e.r, max(e.g, e.b)));\n"
    "                        }\n"
    "                    atomicAdd(t_sum, uint(min(d, 1.0) * 4096.0));\n"
    "                    atomicAdd(t_n, 1u);\n"
    "                    if (d > pc.thr) atomicAdd(t_hot, 1u);\n"
    "                }\n"
    "            }\n"
    "        }\n"
    "    }\n"
    "    barrier();\n"
    "    if (gl_LocalInvocationIndex != 0u) return;\n"
    "    ivec2 tile = ivec2(gl_WorkGroupID.xy);\n"
    "    if (any(greaterThanEqual(tile, imageSize(tile_out)))) return;\n"
    "    bool prev_ok = (pc.flags & 1u) != 0u;\n"
    "    vec4 tp = prev_ok ? texelFetch(tile_prev, tile, 0) : vec4(0.0, 1.0, 0.0, 0.0);\n"
    "    bool evaluated = t_n >= 16u;\n"
    "    bool raw = evaluated && float(t_sum) / 4096.0 / float(t_n) > pc.thr && float(t_hot) >= 0.4 * float(t_n);\n"
    "    float zmin = uintBitsToFloat(t_zmin), zmax = uintBitsToFloat(t_zmax);\n"
    "    bool flat_ = t_zmax != 0u && zmax >= zmin && zmax - zmin < 0.05 * max(zmin, 1e-6);\n"
    "    bool pass = raw && flat_ && t_char == 0u;  // (no tile with a character in it)\n"
    "    float since = floor(tp.g * 255.0 + 0.5), next = min(since + 1.0, 255.0), w = 0.0;\n"
    "    if (pass) { w = prev_ok && since < 12.0 ? 1.0 : 0.35; next = 0.0; }\n"
    "    if ((pc.flags & 4u) != 0u) {\n"
    "        if (pass) atomicAdd(st.s[0], 1u);\n"
    "        if (evaluated) atomicAdd(st.s[1], 1u);\n"
    "        if (raw && !flat_) atomicAdd(st.s[2], 1u);\n"
    "        if (pass && w > 0.5) atomicAdd(st.s[3], 1u);\n"
    "    }\n"
    "    imageStore(tile_out, tile, vec4(w, next / 255.0, tp.r, 1.0));\n"
    "}\n";

// (animated surfaces) after FSR, at the output size: the current input mixed in on flagged tiles (see above).
static const char BYPASS_GLSL[] =
    "#version 450\n"
    "layout(local_size_x = 8, local_size_y = 8) in;\n"
    "layout(set = 0, binding = 0, rgba8) uniform image2D img;\n"
    "layout(set = 0, binding = 1) uniform sampler2D inp;\n"
    "layout(set = 0, binding = 2) uniform sampler2D tile;\n"
    "layout(push_constant) uniform Push { vec2 shift; float w_now, w_next; } pc;\n"
    "vec3 catmull_rom(vec2 pos) {  // pos in texels (centres at +0.5); 9 bilinear taps\n"
    "    vec2 sz = vec2(textureSize(inp, 0)), tc = pos - 0.5, f = fract(tc), t1 = floor(tc) + 0.5;\n"
    "    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f);\n"
    "    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f), w12 = w1 + w2;\n"
    "    vec2 a = (t1 - 1.0) / sz, b = (t1 + w2 / w12) / sz, d = (t1 + 2.0) / sz;\n"
    "    vec3 r = textureLod(inp, vec2(a.x, a.y), 0.0).rgb * w0.x * w0.y + textureLod(inp, vec2(b.x, a.y), 0.0).rgb * w12.x * w0.y +\n"
    "             textureLod(inp, vec2(d.x, a.y), 0.0).rgb * w3.x * w0.y + textureLod(inp, vec2(a.x, b.y), 0.0).rgb * w0.x * w12.y +\n"
    "             textureLod(inp, vec2(b.x, b.y), 0.0).rgb * w12.x * w12.y + textureLod(inp, vec2(d.x, b.y), 0.0).rgb * w3.x * w12.y +\n"
    "             textureLod(inp, vec2(a.x, d.y), 0.0).rgb * w0.x * w3.y + textureLod(inp, vec2(b.x, d.y), 0.0).rgb * w12.x * w3.y +\n"
    "             textureLod(inp, vec2(d.x, d.y), 0.0).rgb * w3.x * w3.y;\n"
    "    return clamp(r, 0.0, 1.0);\n"
    "}\n"
    "void main() {\n"
    "    ivec2 p = ivec2(gl_GlobalInvocationID.xy), osz = imageSize(img);\n"
    "    if (p.x >= osz.x || p.y >= osz.y) return;\n"
    "    vec2 rpos = (vec2(p) + 0.5) * vec2(textureSize(inp, 0)) / vec2(osz);\n"
    "    vec4 t = textureLod(tile, rpos / (8.0 * vec2(textureSize(tile, 0))), 0.0);\n"
    "    float w = max(pc.w_now * t.r, pc.w_next * t.b);\n"
    "    if (w <= 0.002) return;\n"
    "    vec4 o = imageLoad(img, p);\n"
    "    if (pc.w_now > 1.5) { imageStore(img, p, vec4(mix(o.rgb, vec3(1.0, 0.0, 1.0), 0.5 * min(w / 2.0, 1.0)), o.a)); return; }\n"
    "    imageStore(img, p, vec4(mix(o.rgb, catmull_rom(rpos + pc.shift), w), o.a));\n"
    "}\n";

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

// A compute pipeline from GLSL: bindings of the given types (0, 1, ...), push constants of push bytes.
static bool make_compute(const char *src, const char *name, const VkDescriptorType *types, uint32_t n, uint32_t push,
                         VkDescriptorSetLayout *set_layout, VkPipelineLayout *layout, VkPipeline *pipe) {
    size_t nw = 0;
    uint32_t *spv = vk_glsl_to_spirv(src, STAGE_COMPUTE, name, &nw);
    if (!spv) return false;
    VkShaderModule m = vk_module(spv, nw);
    free(spv);
    VkDescriptorSetLayoutBinding b[12];
    for (uint32_t i = 0; i < n; i++) b[i] = (VkDescriptorSetLayoutBinding){i, types[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = n,
                                          .pBindings = b};
    VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, set_layout));
    VkPushConstantRange r = {VK_SHADER_STAGE_COMPUTE_BIT, 0, push};
    VkPipelineLayoutCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = set_layout, .pushConstantRangeCount = push ? 1 : 0,
                                     .pPushConstantRanges = &r};
    VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &pi, NULL, layout));
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = *layout};
    VkResult res = vkCreateComputePipelines(g_vk.dev, g_vk.pcache, 1, &ci, NULL, pipe);
    vkDestroyShaderModule(g_vk.dev, m, NULL);
    return res == VK_SUCCESS;
}

// A descriptor set of n bindings (0, 1, ...): images (sampled with a nearest sampler, or storage) and buffers.
typedef struct { VkDescriptorType type; TexBackend *img; VkBuffer buf; VkDeviceSize off, size; bool linear; } Bind;
static VkDescriptorSet make_set(VkDescriptorSetLayout layout, const Bind *b, uint32_t n) {
    static const VkComponentMapping id = {0};
    VkDescriptorSet set = vk_alloc_set(layout);
    VkDescriptorImageInfo ii[12];
    VkDescriptorBufferInfo bi[12];
    VkWriteDescriptorSet w[12];
    for (uint32_t i = 0; i < n; i++) {
        w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = i,
                                      .descriptorCount = 1, .descriptorType = b[i].type};
        if (b[i].type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            bi[i] = (VkDescriptorBufferInfo){b[i].buf, b[i].off, b[i].size};
            w[i].pBufferInfo = &bi[i];
        } else {
            TexBackend *t = b[i].img;
            VkImageAspectFlags a = t->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : t->aspect;
            ii[i] = (VkDescriptorImageInfo){b[i].type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ? vk_default_sampler(b[i].linear) : VK_NULL_HANDLE,
                                            vk_image_view(t, VK_IMAGE_VIEW_TYPE_2D, t->format, 0, 1, 0, 1, id, a), VK_IMAGE_LAYOUT_GENERAL};
            w[i].pImageInfo = &ii[i];
        }
    }
    vkUpdateDescriptorSets(g_vk.dev, n, w, 0, NULL);
    return set;
}
#define SAMPLED(t) ((Bind){VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (t)})
#define STORAGE(t) ((Bind){VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, (t)})
#define LINEAR(t) ((Bind){VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (t), .linear = true})

static VkDescriptorSetLayout check_set_layout;
static VkPipelineLayout check_layout;
static VkPipeline check_pipe;
static TexBackend img_prev;  // the previous FSR frame's input (animated surfaces, MHGU_FSR_MVCHECK)
static bool prev_in_ok;      // img_prev holds it
static VkBuffer check_buf;
static VmaAllocation check_mem;
static uint32_t *check_sums;
static uint64_t check_seq;   // the submission that writes check_sums (0: none pending)
static uint64_t check_acc[16];
static bool anim_on;                    // [Upscaler] AnimSurf
static VkDescriptorSetLayout anim_set_layout, bypass_set_layout;
static VkPipelineLayout anim_layout, bypass_layout;
static VkPipeline anim_pipe, bypass_pipe;
static TexBackend img_tile[2];          // animated-surface tiles, this FSR frame / the last
static uint32_t tile_cur;
static bool tile_ok;                    // img_tile[tile_cur ^ 1] is the last FSR frame's
static VkBuffer anim_buf;
static VmaAllocation anim_mem;
static uint32_t *anim_sums;
static uint64_t anim_seq, anim_acc[4], anim_frames;

static bool objvel_init(void);

static bool prep_init(void) {
    static const VkDescriptorType prep_types[] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                                  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
    static const VkDescriptorType finish_types[] = {
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER};
    VkImageUsageFlags u = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!make_compute(PREP_GLSL, "fsr_prep", prep_types, 3, 80, &prep_set_layout, &prep_layout, &prep_pipe) ||
        !make_compute(FINISH_GLSL, "fsr_finish", finish_types, 8, 16, &finish_set_layout, &finish_layout, &finish_pipe) ||
        !own_image(&img_depth, VK_FORMAT_R32_SFLOAT, rw, rh, u) || !own_image(&img_mv, VK_FORMAT_R32G32_SFLOAT, rw, rh, u) ||
        !own_image(&img_mvf, VK_FORMAT_R32G32_SFLOAT, rw, rh, u) || !own_image(&img_react, VK_FORMAT_R32_SFLOAT, rw, rh, u) ||
        !own_image(&img_sil[0], VK_FORMAT_R32_SFLOAT, rw, rh, u) || !own_image(&img_sil[1], VK_FORMAT_R32_SFLOAT, rw, rh, u) ||
        !own_image(&img_vel, VK_FORMAT_R16G16B16A16_SFLOAT, rw, rh, u | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
        return false;
    if (g_fsr_objvel && !objvel_init()) {
        LOG("[fsr] object motion could not be set up: camera motion only\n");
        g_fsr_objvel = false;
    }
    VmaAllocationCreateInfo host = {.usage = VMA_MEMORY_USAGE_AUTO,
                                    .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    bool want_check = getenv("MHGU_FSR_MVCHECK") != NULL;
    if ((want_check || g_config.fsr_animsurf) &&
        !own_image(&img_prev, VK_FORMAT_R8G8B8A8_UNORM, rw, rh, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        want_check = false, g_config.fsr_animsurf = 0;
    if (g_config.fsr_animsurf) {
        static const VkDescriptorType anim_types[] = {
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER};
        static const VkDescriptorType bypass_types[] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER};
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 32,
                                 .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        uint32_t tw = (rw + 7) / 8, th = (rh + 7) / 8;
        anim_on = make_compute(ANIM_GLSL, "fsr_animsurf", anim_types, 9, 32, &anim_set_layout, &anim_layout, &anim_pipe) &&
                  make_compute(BYPASS_GLSL, "fsr_animsurf_bypass", bypass_types, 3, 16, &bypass_set_layout, &bypass_layout, &bypass_pipe) &&
                  own_image(&img_tile[0], VK_FORMAT_R8G8B8A8_UNORM, tw, th, u) && own_image(&img_tile[1], VK_FORMAT_R8G8B8A8_UNORM, tw, th, u) &&
                  vmaCreateBuffer(g_vk.vma, &bi, &host, &anim_buf, &anim_mem, &info) == VK_SUCCESS;
        if (anim_on) anim_sums = info.pMappedData;
        else LOG("[fsr] animated surfaces could not be set up: off\n");
    }
    if (want_check) {
        static const VkDescriptorType check_types[] = {
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 64,
                                 .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        if (!make_compute(MVCHECK_GLSL, "fsr_mvcheck", check_types, 7, 8, &check_set_layout, &check_layout, &check_pipe) ||
            vmaCreateBuffer(g_vk.vma, &bi, &host, &check_buf, &check_mem, &info) != VK_SUCCESS) {
            LOG("[fsr] motion vector check unavailable\n");
            check_pipe = VK_NULL_HANDLE;
        } else {
            check_sums = info.pMappedData;
            LOG("[fsr] motion vector check on (MHGU_FSR_MVCHECK)\n");
        }
    }
    return true;
}

// (MHGU_FSR_MVCHECK) the check of this frame's vectors against the previous frame's input. jd: the jitter's picture
// shift then minus now.
static void mv_check(VkCommandBuffer cb, TexBackend *h, TexBackend *sil, bool have_prev, const float jd[2]) {
    if (!check_pipe || !have_prev || check_seq || h->format != img_prev.format) return;
    vkCmdFillBuffer(cb, check_buf, 0, 64, 0);
    vk_full_barrier(cb);
    Bind b[7] = {SAMPLED(h), LINEAR(&img_prev), SAMPLED(&img_mv), SAMPLED(&img_mvf), SAMPLED(sil), SAMPLED(&img_vel),
                 {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, check_buf, 0, 64}};
    VkDescriptorSet set = make_set(check_set_layout, b, 7);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, check_pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, check_layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(cb, check_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, jd);
    vkCmdDispatch(cb, (rw + 7) / 8, (rh + 7) / 8, 1);
    vk_full_barrier(cb);
    check_seq = vk_pending_seq();
}

// The animated-surface tiles of this frame (img_tile[tile_cur]); false: none (no previous input).
static bool anim_pass(VkCommandBuffer cb, TexBackend *h, TexBackend *mask, bool have_prev, bool objv, const float jd[2],
                      const CamRecord *cam) {
    if (!anim_on || h->format != img_prev.format) return false;
    tile_cur ^= 1;
    bool prev_ok = have_prev && prev_in_ok && tile_ok;
    bool count = logging && !anim_seq;
    if (count) vkCmdFillBuffer(cb, anim_buf, 0, 32, 0), vk_full_barrier(cb);
    Bind b[9] = {LINEAR(h), LINEAR(&img_prev), SAMPLED(&img_vel), SAMPLED(&img_mv), SAMPLED(&img_tile[tile_cur ^ 1]),
                 STORAGE(&img_tile[tile_cur]), SAMPLED(&img_depth), {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, anim_buf, 0, 32},
                 SAMPLED(mask ? mask : &img_depth)};
    VkDescriptorSet set = make_set(anim_set_layout, b, 9);
    struct { uint32_t flags; float jdx, jdy, thr, p22, p32, pad0, pad1; } pc = {
        (prev_ok ? 1u : 0u) | (objv ? 2u : 0u) | (count ? 4u : 0u) | (mask ? 8u : 0u), jd[0], jd[1], 0.03f, cam->proj[10], cam->proj[14], 0, 0};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, anim_pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, anim_layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(cb, anim_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cb, (rw + 7) / 8, (rh + 7) / 8, 1);
    vk_full_barrier(cb);
    if (count) anim_seq = vk_pending_seq(), anim_frames++;  // (anim_frames: frames counted)
    tile_ok = true;
    return true;
}

// After FSR: the current input mixed into the output on animated-surface tiles. shift: this frame's picture shift.
static void anim_bypass(VkCommandBuffer cb, TexBackend *h, TexBackend *out, const float shift[2]) {
    if (out->format != VK_FORMAT_R8G8B8A8_UNORM) return;
    Bind b[3] = {STORAGE(out), LINEAR(h), LINEAR(&img_tile[tile_cur])};
    VkDescriptorSet set = make_set(bypass_set_layout, b, 3);
    static int view = -1;  // (debug) MHGU_FSR_ANIMVIEW=1: the tiles tinted magenta instead
    if (view < 0) view = getenv("MHGU_FSR_ANIMVIEW") != NULL;
    float pc[4] = {shift[0], shift[1], view ? 2.0f : 0.85f, view ? 1.0f : 0.5f};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, bypass_pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, bypass_layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(cb, bypass_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cb, (out->width + 7) / 8, (out->height + 7) / 8, 1);
    vk_full_barrier(cb);
}

// This frame's input kept for the next FSR frame (animated surfaces, motion vector check).
static void prev_store(VkCommandBuffer cb, TexBackend *h) {
    if (!img_prev.image || h->format != img_prev.format) return;
    VkImageCopy c = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     .extent = {rw, rh, 1}};
    vkCmdCopyImage(cb, h->image, VK_IMAGE_LAYOUT_GENERAL, img_prev.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    vk_full_barrier(cb);
    prev_in_ok = true;
}

// row-vector 4x4 helpers (double)
static void mat_mul(const double *a, const double *b, double *o) {  // o = a * b
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            double s = 0;
            for (int k = 0; k < 4; k++) s += a[r * 4 + k] * b[k * 4 + c];
            o[r * 4 + c] = s;
        }
}

static bool mat_inv(const double *m, double *o) {  // Gauss-Jordan with partial pivoting
    double a[4][8];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 8; c++) a[r][c] = c < 4 ? m[r * 4 + c] : (c - 4 == r);
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++) if (fabs(a[r][c]) > fabs(a[p][c])) p = r;
        if (fabs(a[p][c]) < 1e-30) return false;
        for (int k = 0; k < 8; k++) { double t = a[c][k]; a[c][k] = a[p][k]; a[p][k] = t; }
        double d = a[c][c];
        for (int k = 0; k < 8; k++) a[c][k] /= d;
        for (int r = 0; r < 4; r++) {
            if (r == c) continue;
            double f = a[r][c];
            for (int k = 0; k < 8; k++) a[r][k] -= f * a[c][k];
        }
    }
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) o[r * 4 + c] = a[r][c + 4];
    return true;
}

static void view_proj(const CamRecord *c, double *vp) {
    double v[16], p[16];
    for (int i = 0; i < 16; i++) v[i] = c->view[i], p[i] = c->proj[i];
    mat_mul(v, p, vp);
}

static void eye_of(const CamRecord *c, double *e) {  // eye = -t R^T (view rows 0-2 = R, row 3 = t)
    const float *m = c->view;
    for (int j = 0; j < 3; j++) e[j] = -(m[12] * m[j * 4] + m[13] * m[j * 4 + 1] + m[14] * m[j * 4 + 2]);
}

// ================================================================== object motion ([Upscaler] ObjectMotion)
// The game skins its characters and monsters on the GPU and has no velocity buffer: with camera vectors alone FSR
// smears moving limbs. Per frame:
//  - capture (exec.c draw() -> vk_fsr_capture): every depth-writing draw into a render-size colour + depth target
//    before the HUD is drawn once more, through a variant of its own vertex shader (the translated GLSL with one store
//    appended: positions[base + gl_VertexIndex] = gl_Position) and rasterizer discard. The positions are the game's
//    own (same source, `invariant gl_Position`), with this frame's camera and jitter in them. Its indices are kept
//    (host memory). Needs vertexPipelineStoresAndAtomics (no transform feedback and no CPU strip
//    expansion: the velocity pass draws the same indices with the same topology and restart).
//  - pairing: a draw is the same draw as one of the previous frame when its vertex program, index buffer, counts,
//    base vertex, topology and vertex buffers match (repeats pair in order); one whose vertex buffers moved (streamed
//    vertex data) pairs by the rest when that is unique in both frames. Both must cover the same gl_VertexIndex range.
//  - velocity pass (at the start of the HUD, before FSR): each paired draw is rasterized at its current positions (no
//    culling, no depth attachment) into img_vel; a fragment keeps mv = (previous position - previous jitter) - (current
//    position - current jitter), render pixels, only where its depth matches the scene's (linear depth within
//    max(2 units, 1%): this surface is what the picture shows) and |mv| <= 64 px.
//  - static draws: on every 4th texel in x and y the velocity pass also notes per draw whether it was seen and whether
//    its vector differs from the camera's by more than 0.25 px. A draw seen for STATIC_FRAMES checked frames without
//    differing is left out of the capture for STATIC_HOLD frames (it gets the camera vectors, right for it), then is
//    checked again. It is woken at once (captured again from that frame on) when its own vertex-stage uniform data
//    changes: a hash of its uniform buffers that are not shared with many draws (the camera's constants are) is kept
//    when it is left out and compared at each of its draws (static_hash). Transforms or bones kept elsewhere (vertex
//    textures, storage buffers) are only seen at the next check, STATIC_HOLD frames later.
//  - blended skinned meshes (depth test without depth writes, a vertex shader indexing a constant buffer with a
//    register: fur shells, hair cards) are captured too; their fragments pass where they lie in front of the scene's
//    depth (Track.front), so their vectors replace the ones of what shows through them.
//  - the finish pass puts the object vectors over the camera's and caps the reactive mask there.
bool g_fsr_objvel;
#define CAP_SLOT (3u << 20)    // positions (vec4) per frame: 48 MB, two frames
#define IDX_SLOT (8u << 20)    // index bytes per frame (host memory), four frames
#define MAX_TRACK 2048         // captured draws per frame
#define STATIC_FRAMES 60      // checked frames without differing from the camera -> left out
#define STATIC_HOLD 240       // .. for that many frames (unless its uniform data changes: static_hash)
#define VEL_MAX_PX 128.0f     // object motion beyond this (render px a frame) is not trusted: camera vectors

static VkBuffer cap_buf, idx_buf, flag_buf, flag_rb;  // flag_rb: flag_buf's copy for the CPU (flag_map)
static VmaAllocation cap_mem, idx_mem, flag_mem, flag_rb_mem;
static uint8_t *idx_map;
static uint32_t *flag_map;
typedef struct {
    uint64_t key, loose;
    int32_t base, vmin, vmax, vofs;  // positions of gl_VertexIndex v at base + v (vmin .. vmax); vertex offset
    uint32_t idx_off, n, first;      // indices at idx_off in idx_buf (itype != 0), their count / vertices from first
    uint8_t itype, topo, restart, used;  // itype 0 none, 1 u16, 2 u32
    uint8_t front;                   // blended (no depth writes): kept where in front of the scene's depth
    uint64_t vs;                     // (log) its vertex program
    uint64_t uhash;                  // uniform_hash at its capture
} Track;
static Track trk[2][MAX_TRACK];
static uint32_t ntrk[2];
static uint64_t trk_frame[2];
static uint64_t cap_frame = 1;       // counts the presents
static uint32_t cap_used, idx_used;
typedef struct { uint64_t key; uint32_t still; uint64_t until, uhash; } StaticEnt;
#define STAT_TAB 16384
static StaticEnt stat_tab[STAT_TAB];
static struct { uint64_t seq; uint32_t n; uint64_t keys[MAX_TRACK], uhash[MAX_TRACK]; } flag_ring[4];
static VkDescriptorSetLayout vel_set_layout;
static VkPipelineLayout vel_layout;
static VkPipeline vel_pipe[3][2];    // triangle list / strip / fan, primitive restart
static VkRenderPass vel_rp;
static uint64_t n_cap, n_pair, n_excluded, n_unpaired;  // (per second, MHGU_FSR_LOG)
// skip reasons (per second): not ready (variant / pipeline building), failed (variant / pipeline), budget (index /
// position memory, > 1M vertices), no range, unpaired: no previous copy / previous copy covers another range
static uint64_t n_sk_build, n_sk_fail, n_sk_budget, n_sk_range, n_up_none, n_up_range, n_woken, n_front, n_cap_skinned;
// UBO addresses bound by the frame's captured draws: those of >= 4 draws in the last frame are shared (camera)
#define UBO_TAB 4096
static struct { uint64_t addr; uint32_t n[2]; } ubo_tab[UBO_TAB];
static char unpaired_note[1024];
static _Atomic uint64_t n_progs_built, n_pipes_built, build_ns;
static uint64_t capture_ns;

static uint64_t mix64(uint64_t h, uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h * 0xC2B2AE3D27D4EB4Full;
}

static StaticEnt *static_ent(uint64_t key, bool add) {
    uint32_t s = (uint32_t)(key ^ (key >> 32)) & (STAT_TAB - 1);
    for (int i = 0; i < 32; i++, s = (s + 1) & (STAT_TAB - 1)) {
        if (stat_tab[s].key == key) return &stat_tab[s];
        if (!stat_tab[s].key) {
            if (!add) return NULL;
            stat_tab[s].key = key;
            return &stat_tab[s];
        }
    }
    return NULL;
}

// The velocity pass: positions from the two frames' captures, the vector in the fragment shader (see above).
static const char VEL_VS[] =
    "#version 450\n"
    "layout(std430, set = 0, binding = 0) readonly buffer Cap { vec4 p[]; } cap;\n"
    "layout(push_constant) uniform Push { ivec4 base; vec4 shift; vec4 lin; vec4 size; } pc;\n"
    "// base: current, previous, draw number, flags; shift: jitter now (xy), before (zw), render px, y down;\n"
    "// lin: m[10], m[14] of the projection (linear depth), max px; size: render size\n"
    "layout(location = 0) out vec4 prev_clip;\n"
    "layout(location = 1) out float bad;\n"
    "vec2 px(vec4 c) { return vec2((c.x / c.w * 0.5 + 0.5) * pc.size.x, (0.5 - c.y / c.w * 0.5) * pc.size.y); }\n"
    "void main() {\n"
    "    vec4 c = cap.p[pc.base.x + gl_VertexIndex], q = cap.p[pc.base.y + gl_VertexIndex];\n"
    "    gl_Position = c;\n"
    "    prev_clip = q;\n"
    "    bad = 0.0;\n"
    "    if (!(c.w > 1e-6) || !(q.w > 1e-6) || any(isnan(q)) || any(isinf(q))) bad = 1.0;\n"
    "    else if (!(length((px(q) - pc.shift.zw) - (px(c) - pc.shift.xy)) <= pc.lin.z)) bad = 1.0;\n"
    "}\n";
static const char VEL_FS[] =
    "#version 450\n"
    "layout(location = 0) in vec4 prev_clip;\n"
    "layout(location = 1) in float bad;\n"
    "layout(set = 0, binding = 1) uniform sampler2D scene_depth;\n"
    "layout(set = 0, binding = 2) uniform sampler2D cam_mv;\n"
    "layout(std430, set = 0, binding = 3) buffer Flags { uint f[]; } flags;\n"
    "layout(push_constant) uniform Push { ivec4 base; vec4 shift; vec4 lin; vec4 size; } pc;\n"
    "layout(location = 0) out vec4 o_vel;\n"
    "float lin(float z) { return pc.lin.y / (z + pc.lin.x); }\n"
    "void main() {\n"
    "    if (bad > 0.0) discard;\n"
    "    ivec2 p = ivec2(gl_FragCoord.xy);\n"
    "    float ls = lin(texelFetch(scene_depth, p, 0).r), lf = lin(gl_FragCoord.z);\n"
    "    float tol = max(2.0, 0.01 * max(lf, 0.0));\n"
    "    if ((pc.base.w & 2) != 0 ? !(lf <= ls + tol) : !(abs(lf - ls) <= tol)) discard;  // (2: blended, in front)\n"
    "    vec2 prev = vec2((prev_clip.x / prev_clip.w * 0.5 + 0.5) * pc.size.x, (0.5 - prev_clip.y / prev_clip.w * 0.5) * pc.size.y);\n"
    "    vec2 mv = (prev - pc.shift.zw) - (gl_FragCoord.xy - pc.shift.xy);\n"
    "    if (any(isnan(mv)) || any(isinf(mv)) || !(length(mv) <= pc.lin.z)) discard;\n"
    "    if ((pc.base.w & 1) != 0 && ((p.x | p.y) & 3) == 0)\n"
    "        atomicOr(flags.f[pc.base.z], length(mv - texelFetch(cam_mv, p, 0).rg) > 0.25 ? 3u : 2u);\n"
    "    o_vel = vec4(mv, 1.0, 0.0);\n"
    "}\n";

static VkShaderModule glsl_module(const char *src, int stage, const char *name) {
    size_t n = 0;
    uint32_t *spv = vk_glsl_to_spirv(src, stage, name, &n);
    if (!spv) return VK_NULL_HANDLE;
    VkShaderModule m = vk_module(spv, n);
    free(spv);
    return m;
}

static bool objvel_init(void) {
    VmaAllocationCreateInfo dev = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VmaAllocationCreateInfo host = {.usage = VMA_MEMORY_USAGE_AUTO,
                                    .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)CAP_SLOT * 2 * 16,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    if (vmaCreateBuffer(g_vk.vma, &bi, &dev, &cap_buf, &cap_mem, NULL) != VK_SUCCESS) return false;
    bi.size = (VkDeviceSize)IDX_SLOT * 4, bi.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (vmaCreateBuffer(g_vk.vma, &bi, &host, &idx_buf, &idx_mem, &info) != VK_SUCCESS) return false;
    idx_map = info.pMappedData;
    // the per-draw notes (static draws): the velocity pass's atomics go to device memory, then a copy brings them to
    // flag_rb for the CPU. (In host memory each atomic crosses the bus: in town, 225 draws, 12 ms of a 15 ms frame on
    // an RTX 3090, against 0.2 ms here.)
    bi.size = (VkDeviceSize)MAX_TRACK * 4 * 4;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vmaCreateBuffer(g_vk.vma, &bi, &dev, &flag_buf, &flag_mem, NULL) != VK_SUCCESS) return false;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vmaCreateBuffer(g_vk.vma, &bi, &host, &flag_rb, &flag_rb_mem, &info) != VK_SUCCESS) return false;
    flag_map = info.pMappedData;
    VkDescriptorSetLayoutBinding b[4] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 4, .pBindings = b};
    VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &vel_set_layout));
    VkPushConstantRange pr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64};
    VkPipelineLayoutCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &vel_set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &pi, NULL, &vel_layout));
    VkShaderModule vs = glsl_module(VEL_VS, STAGE_VERTEX, "fsr_vel_vs"), fs = glsl_module(VEL_FS, STAGE_FRAGMENT, "fsr_vel_fs");
    if (!vs || !fs) return false;
    VkFormat f = img_vel.format;
    vel_rp = vk_render_pass(&f, 1, VK_FORMAT_UNDEFINED, 1);
    VkPipelineShaderStageCreateInfo st[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1,
                                              .pAttachments = &cba};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2,
                                           .pDynamicStates = dyn};
    bool ok = true;
    for (int t = 0; t < 3; t++)
        for (int r = 0; r < 2 && ok; r++) {
            if (t == 0 && r) { vel_pipe[t][r] = vel_pipe[t][0]; continue; }  // (no restart for lists)
            VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                         .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST + t, .primitiveRestartEnable = r};
            VkGraphicsPipelineCreateInfo gi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = st,
                                               .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp,
                                               .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                               .pColorBlendState = &cb, .pDynamicState = &dy, .layout = vel_layout, .renderPass = vel_rp};
            ok = vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &gi, NULL, &vel_pipe[t][r]) == VK_SUCCESS;
        }
    vkDestroyShaderModule(g_vk.dev, vs, NULL);
    vkDestroyShaderModule(g_vk.dev, fs, NULL);
    return ok;
}

// ---- the capture variant of a vertex shader, and its pipelines (built on the shader threads; draws are captured
// once they are ready)
FsrCapProg *vk_fsr_capprog_new(const char *src) {
    FsrCapProg *cp = calloc(1, sizeof *cp);
    cp->src = strdup(src);
    return cp;
}

static void capprog_build(void *arg) {
    uint64_t t0 = plat_now_ns();
    FsrCapProg *cp = arg;
    const char *s = cp->src, *m = strstr(s, "void main()"), *e = strrchr(s, '}');
    if (!m || !e || e < m) { atomic_store(&cp->state, 3); return; }
    static const char decl[] =  // (bindings FSR_CAP_OUT_BINDING / FSR_CAP_BASE_BINDING)
        "layout(std430, set = 0, binding = 30) writeonly buffer FsrCapOut { vec4 p[]; } fsr_cap_out;\n"
        "layout(std140, set = 0, binding = 31) uniform FsrCapBase { ivec4 base; } fsr_cap_in;\n";
    static const char store[] = "  fsr_cap_out.p[fsr_cap_in.base.x + gl_VertexIndex] = gl_Position;\n";
    size_t a = (size_t)(m - s), b = (size_t)(e - s), n = strlen(s);
    char *o = malloc(n + sizeof decl + sizeof store);
    memcpy(o, s, a);
    size_t k = a;
    memcpy(o + k, decl, sizeof decl - 1), k += sizeof decl - 1;
    memcpy(o + k, s + a, b - a), k += b - a;
    memcpy(o + k, store, sizeof store - 1), k += sizeof store - 1;
    memcpy(o + k, s + b, n - b + 1);
    size_t nw = 0;
    uint32_t *spv = vk_glsl_to_spirv(o, STAGE_VERTEX, "fsr_capture_vs", &nw);
    free(o);
    free(cp->src);
    cp->src = NULL;
    if (!spv) { atomic_store(&cp->state, 3); return; }
    cp->nbinds = vk_reflect(spv, nw, cp->binds, 96);
    cp->vs = vk_module(spv, nw);
    free(spv);
    cp->set0 = vk_set_layout(cp->binds, cp->nbinds, 0, VK_SHADER_STAGE_VERTEX_BIT);
    VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &cp->set0};
    bool ok = cp->vs && vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &cp->layout) == VK_SUCCESS;
    atomic_fetch_add(&n_progs_built, 1);
    atomic_fetch_add(&build_ns, plat_now_ns() - t0);
    atomic_store(&cp->state, ok ? 2 : 3);
}

typedef struct { uint64_t key; FsrCapProg *cp; FsrCapture c; VkPipeline pipe; _Atomic int state; } CapPipe;
#define CAPPIPE_TAB 8192
static CapPipe *cappipe_tab[CAPPIPE_TAB];

static void cappipe_build(void *arg) {
    CapPipe *p = arg;
    const FsrCapture *c = &p->c;
    VkVertexInputAttributeDescription va[32];
    VkVertexInputBindingDescription vb[17];
    VkVertexInputBindingDivisorDescriptionEXT vd[17];
    uint32_t na = 0, nb = 0, nd = 0;
    for (int i = 0; i < 32; i++)
        if (c->attr_mask & (1u << i)) va[na++] = (VkVertexInputAttributeDescription){(uint32_t)i, c->attr_bind[i], c->attr_fmt[i], c->attr_off[i]};
    for (uint32_t s = 0; s < 17; s++) {
        if (!(c->used_streams & (1u << s))) continue;
        bool inst = c->divisor[s] != 0;
        vb[nb++] = (VkVertexInputBindingDescription){s, c->stride[s], inst ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
        if (inst && c->divisor[s] > 1 && g_vk.divisor) vd[nd++] = (VkVertexInputBindingDivisorDescriptionEXT){s, c->divisor[s]};
    }
    VkPipelineVertexInputDivisorStateCreateInfoEXT dvi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT,
                                                          .vertexBindingDivisorCount = nd, .pVertexBindingDivisors = vd};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .pNext = nd ? &dvi : NULL,
                                               .vertexBindingDescriptionCount = nb, .pVertexBindingDescriptions = vb,
                                               .vertexAttributeDescriptionCount = na, .pVertexAttributeDescriptions = va};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = (VkPrimitiveTopology)c->topology, .primitiveRestartEnable = c->restart};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .rasterizerDiscardEnable = VK_TRUE, .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineShaderStageCreateInfo st = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
                                          .module = p->cp->vs, .pName = "main"};
    VkGraphicsPipelineCreateInfo gi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 1, .pStages = &st,
                                       .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pRasterizationState = &rs,
                                       .layout = p->cp->layout, .renderPass = c->rp};
    bool ok = vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &gi, NULL, &p->pipe) == VK_SUCCESS;
    atomic_fetch_add(&n_pipes_built, 1);
    atomic_store(&p->state, ok ? 2 : 3);
}

static CapPipe *cappipe(const FsrCapture *c, FsrCapProg *cp) {
    uint64_t h = mix64(mix64(mix64((uint64_t)(uintptr_t)cp, (uint64_t)(uintptr_t)c->rp), c->attr_mask), c->used_streams);
    h = mix64(h, (uint64_t)c->topology << 8 | c->restart);
    for (int i = 0; i < 32; i++)
        if (c->attr_mask >> i & 1) h = mix64(h, (uint64_t)c->attr_fmt[i] << 40 ^ (uint64_t)c->attr_off[i] << 8 ^ c->attr_bind[i]);
    for (int s = 0; s < 17; s++)
        if (c->used_streams >> s & 1) h = mix64(h, (uint64_t)c->stride[s] << 32 | c->divisor[s]);
    if (!h) h = 1;
    uint32_t slot = (uint32_t)(h ^ (h >> 32)) & (CAPPIPE_TAB - 1);
    for (int i = 0; i < 64; i++, slot = (slot + 1) & (CAPPIPE_TAB - 1)) {
        CapPipe *p = cappipe_tab[slot];
        if (p && p->key == h) return p;
        if (!p) {
            p = calloc(1, sizeof *p);
            p->key = h, p->cp = cp, p->c = *c;
            cappipe_tab[slot] = p;
            vk_run_job(cappipe_build, p);
            return p;
        }
    }
    return NULL;
}

VkDescriptorBufferInfo vk_fsr_cap_buffer(void) { return (VkDescriptorBufferInfo){cap_buf, 0, VK_WHOLE_SIZE}; }

// ================================================================== the FSR context
static FfxVkPortableUpscaleContext *ctx;
static FfxVkPortableUpscaleCreateInfo ctx_ci;
static bool ctx_failed;

static bool ctx_make(void) {
    if (ctx) return true;
    if (ctx_failed) return false;
    FfxVkPortableDeviceInfo di = {.structSize = sizeof di, .instance = g_vk.inst, .physicalDevice = g_vk.phys,
                                  .device = g_vk.dev, .getDeviceProcAddr = vkGetDeviceProcAddr, .queue = g_vk.queue,
                                  .queueFamilyIndex = g_vk.qfam, .shaderFloat16Enabled = g_vk.fsr_fp16,
                                  .shaderStorageImageWriteWithoutFormatEnabled = g_vk.fsr_storage_wo_format};
    ctx_ci = (FfxVkPortableUpscaleCreateInfo){.structSize = sizeof ctx_ci, .maxRenderSize = {rw, rh}, .maxOutputSize = {ow, oh}};
    // LDR colour (the post-processed picture, RGBA8), depth 0 near .. 1 far, render-size motion vectors without jitter
    if (getenv("MHGU_FSR_DEBUG")) ctx_ci.flags |= FFX_VK_PORTABLE_CONTEXT_DEBUG_CHECKING;
    FfxVkPortableResult r = ffxVkPortableUpscaleContextCreate(&di, &ctx_ci, &ctx);
    if (r != FFX_VK_PORTABLE_OK) {
        LOG("[fsr] FSR 3.1 context could not be created (%d): the picture is scaled instead\n", r);
        ctx_failed = true;
        ctx = NULL;
        return false;
    }
    FfxVkPortableMemoryUsage mu = {.structSize = sizeof mu};
    ffxVkPortableUpscaleContextGetMemoryUsage(ctx, &mu);
    LOG("[fsr] FSR 3.1 context: %ux%u -> %ux%u, %.1f MB of its own\n", rw, rh, ow, oh, mu.totalUsageInBytes / 1048576.0);
    return true;
}

static FfxVkPortableImage ffx_image(TexBackend *b, VkImageUsageFlags usage) {
    FfxVkPortableImage i = {.structSize = sizeof i};
    if (!b) return i;
    i.image = b->image, i.format = b->format, i.extent = (FfxVkPortableExtent2D){b->width, b->height};
    i.mipCount = 1, i.arrayLayers = 1, i.usage = usage, i.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    i.state = FFX_VK_PORTABLE_RESOURCE_STATE_UNORDERED_ACCESS;  // (every image here lives in GENERAL)
    return i;
}

// ================================================================== the frame
static NvnTexture *g_h;                        // the full-screen image (learnt at the presents)
static NvnTexture *last_target, *last_src;     // the frame's last draw: its target, the first texture it sampled
static bool frame_open, redirect, frame_fsr;   // a draw seen since the present; from the start of the HUD on; FSR ran
static bool hud_seen;                           // the start of the HUD was this frame (FSR: redirect too; T2x: not)
static uint32_t h_draws;                       // draws into H so far (post passes)
static struct { NvnTexture *t; uint32_t n; } depths[4];
static NvnTexture *mask_first;                 // the first render-size colour target drawn with depth (frame_mask)
static struct { NvnTexture *t; uint32_t n; } colors[4];  // depth-writing draws per render-size colour target
static bool sil_ok;                            // img_sil[sil_cur ^ 1] holds the last FSR frame's silhouette
static uint32_t sil_cur;
static uint64_t matched_seq;                   // the capture found in the frame's uniform buffers (0: none)
static int match_tries;
static CamRecord prev_cam;                     // the camera of the last FSR frame
static uint64_t prev_fsr_present;              // g_present_count at the last FSR frame
static int not_scene;                          // frames in a row that were not scene frames
static bool jitter_on;
static uint64_t last_dispatch_ns, frame_id;
// per-second log (MHGU_FSR_LOG)
static uint32_t n_fsr4, n_fsr, n_scaled, n_none, n_match, n_nomatch, n_reset, lag_hist[4], n_objv_frames, n_react_frames;
static uint64_t log_t0;
static char why_last[96];

static bool is_rt(NvnTexture *t) { return t && t->backend && ((TexBackend *)t->backend)->gpu_owned; }

static bool presented_texture(NvnTexture *t) { return t && t->backend && ((TexBackend *)t->backend)->presented; }

static void frame_begin(void);

// ---- object motion: capture (exec.c draw()), velocity pass, static draws (see "object motion" above)
static bool capture(const FsrCapture *c, FsrCapProg *cp, VkPipeline *pipe, int32_t *base);
bool vk_fsr_capture(const FsrCapture *c, FsrCapProg *cp, VkPipeline *pipe, int32_t *base) {
    uint64_t t0 = plat_now_ns();
    bool r = capture(c, cp, pipe, base);
    capture_ns += plat_now_ns() - t0;
    return r;
}

// A hash of the draw's own vertex-stage uniform data: its uniform buffers that are not shared with many draws (the
// shared ones hold the camera's constants, which change for static draws too). Registers this frame's buffers.
static uint64_t uniform_hash(const FsrCapture *c) {
    uint32_t cur = cap_frame & 1;
    uint64_t h = 0x84222325cbf29ce4ull;
    for (int u = 0; c->vubuf && u < 18; u++) {
        uint64_t a = c->vubuf[2 * u], n = c->vubuf[2 * u + 1];
        if (!a || !n || a >= GUEST_SPACE || n > GUEST_SPACE - a) continue;
        uint32_t s = (uint32_t)((a * 0x9E3779B97F4A7C15ull) >> 52) & (UBO_TAB - 1);
        int i = 0;
        while (i < 16 && ubo_tab[s].addr && ubo_tab[s].addr != a) s = (s + 1) & (UBO_TAB - 1), i++;
        bool shared = false;
        if (i < 16) {
            if (ubo_tab[s].addr != a) ubo_tab[s].addr = a, ubo_tab[s].n[0] = ubo_tab[s].n[1] = 0;
            ubo_tab[s].n[cur]++;
            shared = ubo_tab[s].n[cur ^ 1] >= 4;
        }
        if (shared) continue;
        if (n > 0x4000) n = 0x4000;
        const uint64_t *w = G2H((uint32_t)a);
        for (uint64_t k = 0; k < n / 8; k++) h = mix64(h, w[k]);
        h = mix64(h, n);
    }
    return h;
}

static bool capture(const FsrCapture *c, FsrCapProg *cp, VkPipeline *pipe, int32_t *base) {
    if (!jitter_on || redirect || !c->color0 || c->color0 == g_h || !c->depth || c->depth->width != rw || c->depth->height != rh ||
        c->color0->width != rw || c->color0->height != rh || c->topology > VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN ||
        c->topology < VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        return false;
    if (!c->depth_write && !c->skinned) return false;  // (blended: only skinned meshes)
    if (!frame_open) frame_begin();
    uint32_t slot = cap_frame & 1;
    if (ntrk[slot] >= MAX_TRACK) { n_sk_budget++; return false; }
    uint64_t loose = mix64(mix64(mix64(mix64(c->vs_code, c->iaddr), (uint64_t)c->count << 32 | c->first),
                                 (uint64_t)(uint32_t)c->base_vertex << 8 | c->topology), c->mode);
    uint64_t uh = uniform_hash(c);
    StaticEnt *se = static_ent(loose, false);
    if (se && se->until > cap_frame) {
        if (se->uhash == uh) { n_excluded++; return false; }
        se->until = 0, se->still = 0;  // its own uniform data changed: captured again from now on
        n_woken++;
    }
    int state = atomic_load(&cp->state);
    if (state == 0) {
        int expect = 0;
        if (atomic_compare_exchange_strong(&cp->state, &expect, 1)) vk_run_job(capprog_build, cp);
        n_sk_build++;
        return false;
    }
    if (state != 2) { state == 3 ? n_sk_fail++ : n_sk_build++; return false; }
    CapPipe *pp = cappipe(c, cp);
    int ps = pp ? atomic_load(&pp->state) : 3;
    if (ps != 2) { ps == 3 ? n_sk_fail++ : n_sk_build++; return false; }
    uint64_t key = loose;
    for (int s = 0; s < 16; s++) if (c->used_streams >> s & 1) key = mix64(key, c->vbuf[s]);
    // gl_VertexIndex range, the indices kept for the velocity pass
    Track t = {.key = key, .loose = loose, .vofs = c->base_vertex, .first = c->first, .topo = (uint8_t)c->topology,
               .restart = c->restart, .front = !c->depth_write, .vs = c->vs_code, .uhash = uh};
    if (c->idx) {
        bool u16 = c->itype == VK_INDEX_TYPE_UINT16;
        uint32_t bytes = c->icount * (u16 ? 2 : 4), off = (cap_frame % 4) * IDX_SLOT + idx_used;
        if (!c->icount) { n_sk_range++; return false; }
        if (idx_used + bytes > IDX_SLOT) { n_sk_budget++; return false; }
        const void *src = c->gidx ? c->gidx : c->idx;
        memcpy(idx_map + off, src, bytes);
        uint32_t mn = UINT32_MAX, mx = 0, none = u16 ? 0xFFFFu : 0xFFFFFFFFu;
        for (uint32_t i = 0; i < c->icount; i++) {
            uint32_t v = u16 ? ((const uint16_t *)src)[i] : ((const uint32_t *)src)[i];
            if (c->restart && v == none) continue;
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
        if (mn > mx) { n_sk_range++; return false; }
        t.vmin = (int32_t)mn + c->base_vertex, t.vmax = (int32_t)mx + c->base_vertex;
        t.itype = u16 ? 1 : 2, t.idx_off = off, t.n = c->icount;
        idx_used += (bytes + 3) & ~3u;
    } else {
        t.vmin = (int32_t)c->first, t.vmax = (int32_t)(c->first + c->count) - 1, t.n = c->count;
    }
    if (t.vmin < 0 || t.vmax < t.vmin) { n_sk_range++; return false; }
    uint32_t range = (uint32_t)(t.vmax - t.vmin + 1);
    if (range > (1u << 20) || cap_used + range > CAP_SLOT) { n_sk_budget++; return false; }
    t.base = (int32_t)(slot * CAP_SLOT + cap_used) - t.vmin;
    cap_used += range;
    trk[slot][ntrk[slot]++] = t;
    *pipe = pp->pipe;
    *base = t.base;
    n_cap++;
    n_front += t.front;
    n_cap_skinned += c->skinned;
    return true;
}

// The previous frame's capture of draw t (NULL: none).
static Track *pair_of(const Track *t, uint32_t cs, uint32_t ps) {
    for (uint32_t i = 0; i < ntrk[ps]; i++) {
        Track *p = &trk[ps][i];
        if (!p->used && p->key == t->key) return p;
    }
    Track *one = NULL;
    for (uint32_t i = 0; i < ntrk[ps]; i++) {  // streamed vertex data: by the rest, when unique in both frames
        Track *p = &trk[ps][i];
        if (p->loose != t->loose) continue;
        if (one || p->used) return NULL;
        one = p;
    }
    if (!one) return NULL;
    uint32_t same = 0;
    for (uint32_t i = 0; i < ntrk[cs]; i++) same += trk[cs][i].loose == t->loose;
    return same == 1 ? one : NULL;
}

// The velocity pass into img_vel (cleared first). Returns the number of draws rasterized.
static uint32_t objvel_pass(VkCommandBuffer cb, const CamRecord *cam, const CamRecord *prev) {
    VkClearColorValue zero = {{0, 0, 0, 0}};
    VkImageSubresourceRange rg = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, img_vel.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &rg);
    uint32_t cs = cap_frame & 1, ps = cs ^ 1;
    uint32_t fr = (uint32_t)(cap_frame % 4);
    bool record = !flag_ring[fr].seq;  // (that slot's last readback is done)
    if (record) vkCmdFillBuffer(cb, flag_buf, (VkDeviceSize)fr * MAX_TRACK * 4, MAX_TRACK * 4, 0);
    vk_full_barrier(cb);
    if (!ntrk[cs] || !ntrk[ps] || trk_frame[ps] + 1 != trk_frame[cs]) return 0;
    vmaFlushAllocation(g_vk.vma, idx_mem, (VkDeviceSize)(cap_frame % 4) * IDX_SLOT, idx_used ? idx_used : 4);
    for (uint32_t i = 0; i < ntrk[ps]; i++) trk[ps][i].used = 0;
    Bind b[4] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, cap_buf, 0, VK_WHOLE_SIZE}, SAMPLED(&img_depth), SAMPLED(&img_mv),
                 {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, flag_buf, (VkDeviceSize)fr * MAX_TRACK * 4, MAX_TRACK * 4}};
    VkDescriptorSet set = make_set(vel_set_layout, b, 4);
    static const VkComponentMapping id = {0};
    VkImageView view = vk_image_view(&img_vel, VK_IMAGE_VIEW_TYPE_2D, img_vel.format, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
    VkRenderPassBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = vel_rp,
                                .framebuffer = vk_framebuffer(vel_rp, &view, 1, rw, rh), .renderArea = {{0, 0}, {rw, rh}}};
    vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp = {0, (float)rh, (float)rw, -(float)rh, 0, 1};  // (the translator's clip space: +y at the top)
    VkRect2D sc = {{0, 0}, {rw, rh}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vel_layout, 0, 1, &set, 0, NULL);
    struct { int32_t base[4]; float shift[4], lin[4], size[4]; } pc = {
        .shift = {cam->jitter[0], cam->jitter[1], prev->jitter[0], prev->jitter[1]},
        .lin = {cam->proj[10], cam->proj[14], VEL_MAX_PX, 0}, .size = {(float)rw, (float)rh, 0, 0}};
    VkPipeline bound = VK_NULL_HANDLE;
    uint32_t n = 0;
    for (uint32_t i = 0; i < ntrk[cs]; i++) {
        Track *t = &trk[cs][i], *p = pair_of(t, cs, ps);
        if (!p || p->vmin != t->vmin || p->vmax != t->vmax) {
            n_unpaired++;
            p ? n_up_range++ : n_up_none++;
            size_t l = strlen(unpaired_note);
            if (logging && l < sizeof unpaired_note - 120)
                snprintf(unpaired_note + l, sizeof unpaired_note - l, " [vs %llx n %u v %d..%d %s]", (unsigned long long)t->vs,
                         t->n, t->vmin, t->vmax, p ? "range differs" : "no previous copy");
            continue;
        }
        p->used = 1;
        VkPipeline pipe = vel_pipe[t->topo - VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST][t->restart ? 1 : 0];
        if (pipe != bound) vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, bound = pipe);
        pc.base[0] = t->base, pc.base[1] = p->base, pc.base[2] = (int32_t)n, pc.base[3] = record | (t->front ? 2 : 0);
        vkCmdPushConstants(cb, vel_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
        if (t->itype) {
            vkCmdBindIndexBuffer(cb, idx_buf, t->idx_off, t->itype == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cb, t->n, 1, 0, t->vofs, 0);
        } else {
            vkCmdDraw(cb, t->n, 1, t->first, 0);
        }
        if (record) flag_ring[fr].keys[n] = t->loose, flag_ring[fr].uhash[n] = t->uhash;
        n++;
    }
    vkCmdEndRenderPass(cb);
    vk_full_barrier(cb);
    if (record && n) {  // the notes to flag_rb, read once the GPU is done (objvel_static_update)
        VkBufferCopy c = {(VkDeviceSize)fr * MAX_TRACK * 4, (VkDeviceSize)fr * MAX_TRACK * 4, (VkDeviceSize)n * 4};
        vkCmdCopyBuffer(cb, flag_buf, flag_rb, 1, &c);
        VkMemoryBarrier hb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                              .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, NULL, 0, NULL);
        flag_ring[fr].n = n, flag_ring[fr].seq = vk_pending_seq();
    }
    n_pair += n;
    return n;
}

// The velocity pass's per-draw notes, once the GPU is done with them: draws that stay static are left out a while.
static void objvel_static_update(void) {
    for (int fr = 0; fr < 4; fr++) {
        if (!flag_ring[fr].seq || vk_completed_seq() < flag_ring[fr].seq) continue;
        vmaInvalidateAllocation(g_vk.vma, flag_rb_mem, (VkDeviceSize)fr * MAX_TRACK * 4, MAX_TRACK * 4);
        const uint32_t *f = flag_map + (size_t)fr * MAX_TRACK;
        for (uint32_t i = 0; i < flag_ring[fr].n; i++) {
            if (!(f[i] & 2)) continue;  // not seen (on the sampled texels)
            StaticEnt *e = static_ent(flag_ring[fr].keys[i], true);
            if (!e) continue;
            if (f[i] & 1) e->still = 0;
            else if (++e->still >= STATIC_FRAMES) e->still = 0, e->until = cap_frame + STATIC_HOLD, e->uhash = flag_ring[fr].uhash[i];
        }
        flag_ring[fr].seq = 0;
    }
}

// The scene's depth target: the render-size one with the most depth-writing draws this frame.
static NvnTexture *scene_depth(uint32_t *draws) {
    int best = -1;
    for (int i = 0; i < 4; i++) if (depths[i].t && (best < 0 || depths[i].n > depths[best].n)) best = i;
    *draws = best >= 0 ? depths[best].n : 0;
    return best >= 0 ? depths[best].t : NULL;
}

// The camera capture of this frame (see the top).
static bool frame_camera(CamRecord *c) {
    if (matched_seq && camjit_get(matched_seq, c)) return true;
    return camjit_latest(c) && c->present + 1 >= g_present_count;  // (the camera was computed this frame or the last)
}

// The capture whose jittered m[8] / m[9] a scene draw's vertex uniform buffers hold (row-major: adjacent; column-major:
// 16 bytes apart).
static void camera_match(const FsrDraw *d) {
    if (matched_seq || match_tries >= 8) return;
    match_tries++;
    CamRecord latest;
    if (!camjit_latest(&latest)) return;
    uint32_t k8[8], k9[8];
    uint64_t ks[8];
    int nk = 0;
    for (uint64_t s = latest.seq; s > 0 && s + 8 > latest.seq && nk < 8; s--) {
        CamRecord c;
        if (!camjit_get(s, &c) || (c.jitter[0] == 0 && c.jitter[1] == 0)) continue;
        memcpy(&k8[nk], &c.gpu8, 4), memcpy(&k9[nk], &c.gpu9, 4), ks[nk++] = s;
    }
    if (!nk) return;
    for (int u = 0; u < d->nvubuf; u++) {
        uint64_t a = d->vubuf[2 * u], n = d->vubuf[2 * u + 1];
        if (!a || n < 32 || a >= GUEST_SPACE || n > GUEST_SPACE - a) continue;
        if (n > 0x4000) n = 0x4000;
        const uint32_t *w = G2H((uint32_t)a);
        uint64_t nw = n / 4;
        for (uint64_t i = 0; i < nw; i++)
            for (int k = 0; k < nk; k++) {
                if (w[i] != k8[k]) continue;
                if ((i + 1 < nw && w[i + 1] == k9[k]) || (i + 4 < nw && w[i + 4] == k9[k])) {
                    matched_seq = ks[k];
                    return;
                }
            }
    }
}

static void frame_begin(void) {
    frame_open = true;
    redirect = hud_seen = frame_fsr = false;
    h_draws = 0;
    memset(depths, 0, sizeof depths);
    matched_seq = 0;
    match_tries = 0;
    mask_first = NULL;
    memset(colors, 0, sizeof colors);
    uint32_t s = cap_frame & 1;  // (object motion: this frame's captures)
    ntrk[s] = 0, trk_frame[s] = cap_frame;
    cap_used = idx_used = 0;
    for (int i = 0; i < UBO_TAB; i++) ubo_tab[i].n[s] = 0;
}

// The game's moving-object mask this frame: the first render-size colour target drawn with the scene depth, unless
// it is the target most scene draws went to (then there was no separate mask pass). NULL: none.
static NvnTexture *frame_mask(void) {
    uint32_t nmask = 0, nmax = 0;
    for (int i = 0; i < 4; i++) {
        if (!colors[i].t) continue;
        if (colors[i].t == mask_first) nmask = colors[i].n;
        else if (colors[i].n > nmax) nmax = colors[i].n;
    }
    return mask_first && nmask < nmax ? mask_first : NULL;
}

// H -> H' scaled (frames without FSR)
static void scale_to_stand(NvnTexture *h, NvnTexture *hs) {
    TexBackend *s = vk_tex(h, true), *d = hs->backend;
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkImageBlit r = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int32_t)s->width, (int32_t)s->height, 1}},
                     .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {(int32_t)d->width, (int32_t)d->height, 1}}};
    vkCmdBlitImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &r, VK_FILTER_LINEAR);
    vk_full_barrier(cb);
}

// The frame's motion vectors for FSR and SMAA T2x (img_mvf, render size, from the camera of the last such frame):
// prep (the scene depth, the camera's vectors), object motion, animated surfaces (with_anim: FSR's) and finish (and
// the reactive mask). out->reset: no last frame to continue (a cut, a gap, another camera). false: not possible.
typedef struct { bool reset, objv, anim, react; } Vectors;
static bool frame_vectors(VkCommandBuffer cb, TexBackend *hb, TexBackend *db, NvnTexture *mask, const CamRecord *cam,
                          bool with_anim, Vectors *out) {
    // camera motion vectors from the previous frame's camera; history reset at a cut
    bool reset = !prev_cam.seq || prev_fsr_present + 1 < g_present_count || prev_cam.cam != cam->cam;
    double e0[3], e1[3];
    eye_of(cam, e1);
    if (!reset) {
        eye_of(&prev_cam, e0);
        double dx = e1[0] - e0[0], dy = e1[1] - e0[1], dz = e1[2] - e0[2];
        if (dx * dx + dy * dy + dz * dz > 300.0 * 300.0) reset = true;  // (a cut: world units are centimetres)
    }
    float push[20] = {0};
    if (!reset) {
        double vp_c[16], vp_p[16], inv[16], x[16];
        view_proj(cam, vp_c);
        view_proj(&prev_cam, vp_p);
        if (mat_inv(vp_c, inv)) {
            mat_mul(inv, vp_p, x);
            for (int i = 0; i < 16; i++) push[i] = (float)x[i];
            push[16] = 1.0f;
        } else {
            reset = true;
        }
    }
    // 1. prep
    static const VkComponentMapping id = {0};
    VkDescriptorSet set = vk_alloc_set(prep_set_layout);
    VkDescriptorImageInfo ii[3] = {
        {vk_default_sampler(false), vk_image_view(db, VK_IMAGE_VIEW_TYPE_2D, db->format, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_DEPTH_BIT),
         VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, vk_image_view(&img_depth, VK_IMAGE_VIEW_TYPE_2D, img_depth.format, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT),
         VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, vk_image_view(&img_mv, VK_IMAGE_VIEW_TYPE_2D, img_mv.format, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT),
         VK_IMAGE_LAYOUT_GENERAL}};
    if (!ii[0].imageView || !ii[1].imageView || !ii[2].imageView) return false;
    VkWriteDescriptorSet w[3];
    for (int i = 0; i < 3; i++)
        w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = (uint32_t)i,
                                      .descriptorCount = 1,
                                      .descriptorType = i ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                      .pImageInfo = &ii[i]};
    vkUpdateDescriptorSets(g_vk.dev, 3, w, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, prep_pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, prep_layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(cb, prep_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
    vkCmdDispatch(cb, (rw + 7) / 8, (rh + 7) / 8, 1);
    vk_full_barrier(cb);
    // 1b. object motion: the velocity pass (object motion above)
    bool objv = g_fsr_objvel && !reset && objvel_pass(cb, cam, &prev_cam) > 0;
    // 1c. animated surfaces (ANIM_GLSL)
    float jd[2] = {reset ? 0.0f : prev_cam.jitter[0] - cam->jitter[0], reset ? 0.0f : prev_cam.jitter[1] - cam->jitter[1]};
    TexBackend *mb = mask ? vk_tex(mask, true) : NULL;
    if (mb && (mb->aspect != VK_IMAGE_ASPECT_COLOR_BIT || mb->samples != 1)) mb = NULL;
    bool anim = with_anim && anim_pass(cb, hb, mb, !reset, objv, jd, cam);
    // 1d. finish: the vectors FSR gets (object over camera) and the reactive mask
    sil_cur ^= 1;
    struct { float strength, vel_cap; uint32_t flags; float pad; } fp = {
        g_config.fsr_reactive, 0.1f, (objv ? 1u : 0u) | (mb ? 2u : 0u) | (sil_ok && !reset ? 4u : 0u) | (anim ? 8u : 0u), 0};
    Bind fb[8] = {SAMPLED(&img_mv), SAMPLED(&img_vel), SAMPLED(mb ? mb : &img_depth), SAMPLED(&img_sil[sil_cur ^ 1]),
                  STORAGE(&img_mvf), STORAGE(&img_react), STORAGE(&img_sil[sil_cur]), SAMPLED(anim ? &img_tile[tile_cur] : &img_depth)};
    VkDescriptorSet fset = make_set(finish_set_layout, fb, 8);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, finish_pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, finish_layout, 0, 1, &fset, 0, NULL);
    vkCmdPushConstants(cb, finish_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof fp, &fp);
    vkCmdDispatch(cb, (rw + 7) / 8, (rh + 7) / 8, 1);
    vk_full_barrier(cb);
    sil_ok = mb != NULL;
    bool react = (mb && g_config.fsr_reactive > 0.0f) || anim;
    if (objv) n_objv_frames++;
    if (react) n_react_frames++;
    mv_check(cb, hb, &img_sil[sil_cur], !reset && prev_in_ok, jd);  // ((debug) MHGU_FSR_MVCHECK)
    *out = (Vectors){reset, objv, anim, react};
    return true;
}

// prep + FSR: H -> H'. false: not done (H' untouched).
static bool use_fsr4;  // [Upscaler] Upscaler = FSR4 and the driver's FSR 4 started (fsr4.c); false after it fails: FSR 3.1

static bool run_fsr(NvnTexture *h, NvnTexture *hs, NvnTexture *depth, NvnTexture *mask, const CamRecord *cam) {
    if (use_fsr4 && !vk_fsr4_ready()) use_fsr4 = false;  // (it failed: fsr4.c logged why)
    if (!use_fsr4 && !ctx_make()) return false;
    TexBackend *hb = vk_tex(h, true), *db = vk_tex(depth, true), *ob = hs->backend;
    if (hb->width != rw || hb->height != rh || db->width != rw || db->height != rh) {
        static int nlog;
        if (nlog++ < 20) LOG("[fsr] not upscaled: the picture (%ux%u) or depth (%ux%u) is not %ux%u\n", hb->width, hb->height,
                             db->width, db->height, rw, rh);
        return false;
    }
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    Vectors v;
    if (!frame_vectors(cb, hb, db, mask, cam, true, &v)) return false;
    bool reset = v.reset, anim = v.anim, react = v.react;
    // 2. FSR
    uint64_t now = plat_now_ns();
    float dt = last_dispatch_ns ? (float)((now - last_dispatch_ns) / 1e6) : 16.7f;
    if (dt < 1.0f) dt = 1.0f;
    if (dt > 100.0f) dt = 100.0f;
    last_dispatch_ns = now;
    bool reset_in = reset;
    if (use_fsr4) {  // FSR 4: the same inputs, recorded by the driver's provider
        float n = cam->near, f = cam->far;
        if (!(n > 0.0f) || !(f > n)) n = 8.0f, f = 50000.0f;
        Fsr4Dispatch d4 = {.cb = cb, .color = {hb->image, hb->format, hb->width, hb->height},
                           .depth = {img_depth.image, img_depth.format, img_depth.width, img_depth.height},
                           .motion = {img_mvf.image, img_mvf.format, img_mvf.width, img_mvf.height},
                           .output = {ob->image, ob->format, ob->width, ob->height},
                           .jitter = {cam->jitter[0], cam->jitter[1]}, .render = {rw, rh}, .out = {ow, oh}, .frame_ms = dt,
                           .cam_near = n, .cam_far = f, .fovy = cam->fovy > 0.05f && cam->fovy < 3.0f ? cam->fovy : 1.0f,
                           .view_to_meters = 0.01f, .sharpness = g_config.fsr_sharpness, .sharpen = g_config.fsr_sharpness > 0.0f,
                           .reset = reset};
        if (react) d4.reactive = (Fsr4Image){img_react.image, img_react.format, img_react.width, img_react.height};
        if (vk_fsr4_dispatch(&d4)) {
            n_fsr4++;
            goto dispatched;
        }
        use_fsr4 = false;  // failed (logged): FSR 3.1 for this frame, with a fresh history
        reset = true;
        if (!ctx_make()) return false;
    }
    FfxVkPortableUpscaleDispatchInfo di = {.structSize = sizeof di, .commandBuffer = cb};
    di.color = ffx_image(hb, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    di.depth = ffx_image(&img_depth, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT);
    di.motionVectors = ffx_image(&img_mvf, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT);
    di.exposure = ffx_image(NULL, 0);
    di.reactiveMask = ffx_image(react ? &img_react : NULL, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT);
    di.transparencyAndCompositionMask = ffx_image(NULL, 0);
    di.output = ffx_image(ob, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    static float jsign[2] = {0, 0};  // (debug) MHGU_FSR_JITTER_SIGN=sx,sy: other sign conventions, to check this one
    if (!jsign[0]) {
        const char *e = getenv("MHGU_FSR_JITTER_SIGN");
        if (!e || sscanf(e, "%f,%f", &jsign[0], &jsign[1]) != 2) jsign[0] = jsign[1] = 1.0f;
    }
    di.jitterOffset = (FfxVkPortableFloat2){jsign[0] * cam->jitter[0], jsign[1] * cam->jitter[1]};  // (the picture shift)
    di.motionVectorScale = (FfxVkPortableFloat2){1.0f, 1.0f};
    di.renderSize = (FfxVkPortableExtent2D){rw, rh};
    di.outputSize = (FfxVkPortableExtent2D){ow, oh};
    di.frameTimeMilliseconds = dt;
    di.preExposure = 1.0f;
    float n = cam->near, f = cam->far;
    if (!(n > 0.0f) || !(f > n)) n = 8.0f, f = 50000.0f;
    di.cameraNear = n, di.cameraFar = f;
    di.cameraVerticalFovRadians = cam->fovy > 0.05f && cam->fovy < 3.0f ? cam->fovy : 1.0f;
    di.viewSpaceToMeters = 0.01f;
    di.sharpness = g_config.fsr_sharpness;
    di.enableSharpening = g_config.fsr_sharpness > 0.0f;
    di.reset = reset;
    di.frameId = ++frame_id;
    uint64_t issues = ffxVkPortableValidateUpscaleDispatchInfo(&ctx_ci, &di);
    if (issues) {
        static int nlog;
        for (uint64_t bit = 1; bit && nlog < 20; bit <<= 1)
            if (issues & bit) { nlog++; LOG("[fsr] dispatch rejected: %s\n", ffxVkPortableValidationIssueName(bit)); }
        return false;
    }
    FfxVkPortableResult r = ffxVkPortableUpscaleContextRecordDispatch(ctx, &di);
    if (r != FFX_VK_PORTABLE_OK) {
        static int nlog;
        if (nlog++ < 20) LOG("[fsr] dispatch failed: %d\n", r);
        return false;
    }
dispatched:
    vk_full_barrier(cb);
    if (anim) anim_bypass(cb, hb, ob, cam->jitter);  // (animated surfaces: the current input there)
    prev_store(cb, hb);
    if (reset || reset_in) n_reset++;
    prev_cam = *cam;
    prev_fsr_present = g_present_count;
    return true;
}

// ---- SMAA T2x ([Anti-aliasing] AntiAliasing = SMAAT2x; smaa.c): at the start of the HUD, on H in place (no stand-ins:
// the picture keeps its size), with this frame's jitter and the motion vectors as for FSR.
static bool t2x_target(NvnTexture *h, VkPostAa *t) {
    TexBackend *b = vk_tex(h, true);
    vk_ms_resolve(b);
    t->src = vk_view(h, NULL, false, false, TEXTYPE_2D);
    t->dst = vk_view(h, NULL, false, true, TEXTYPE_2D);
    t->dst_image = b->image, t->format = b->format, t->w = b->width, t->h = b->height;
    return t->src && t->dst && b->samples == 1;
}

static bool run_t2x(NvnTexture *h, NvnTexture *depth, NvnTexture *mask, const CamRecord *cam) {
    TexBackend *hb = vk_tex(h, true), *db = vk_tex(depth, true);
    if (hb->width != rw || hb->height != rh || db->width != rw || db->height != rh) {
        static int nlog;
        if (nlog++ < 20) LOG("[t2x] not this frame: the picture (%ux%u) or depth (%ux%u) is not %ux%u\n", hb->width, hb->height,
                             db->width, db->height, rw, rh);
        return false;
    }
    VkPostAa t;
    if (!t2x_target(h, &t)) return false;
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    Vectors v;
    if (!frame_vectors(cb, hb, db, mask, cam, false, &v)) return false;
    if (check_pipe) prev_store(cb, hb);  // ((debug) MHGU_FSR_MVCHECK: this frame's input, before T2x writes over it)
    static const VkComponentMapping id = {0};
    VkImageView mv = vk_image_view(&img_mvf, VK_IMAGE_VIEW_TYPE_2D, img_mvf.format, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageView react = v.react ? vk_image_view(&img_react, VK_IMAGE_VIEW_TYPE_2D, img_react.format, 0, 1, 0, 1, id,
                                                VK_IMAGE_ASPECT_COLOR_BIT) : VK_NULL_HANDLE;
    if (!mv || !vk_smaa_t2x(&t, config_live_smaa_quality(), mv, react, camjit_t2x_phase(cam->jitter), !v.reset)) return false;
    hb->ms_valid = false;
    if (v.reset) n_reset++;
    prev_cam = *cam;
    prev_fsr_present = g_present_count;
    return true;
}

static void t2x_hud_start(void) {
    uint32_t dn = 0;
    NvnTexture *depth = scene_depth(&dn);
    CamRecord cam;
    bool have_cam = frame_camera(&cam);
    frame_fsr = depth && dn >= 16 && have_cam && run_t2x(g_h, depth, frame_mask(), &cam);
    VkPostAa t;
    if (!frame_fsr && t2x_target(g_h, &t)) vk_smaa_t2x(&t, config_live_smaa_quality(), VK_NULL_HANDLE, VK_NULL_HANDLE, -1, false);  // (SMAA 1x)
    static int last = -1, nlog;
    if ((int)frame_fsr != last && nlog++ < 500)
        LOG("[t2x] frame %llu: %s\n", (unsigned long long)g_present_count + 1,
            frame_fsr ? "SMAA T2x" : !depth || dn < 16 ? "SMAA 1x: no 3D scene this frame" : "SMAA 1x: the main camera was not computed");
    last = frame_fsr;
}

// The start of the HUD: FSR (or scaling) into H', then H, P, D are replaced from here on (SMAA T2x: in H, nothing replaced).
static void hud_start(const char *why) {
    vk_end_pass();
    if (!g_fsr_on) {
        hud_seen = true;
        t2x_hud_start();
        if (logging) snprintf(why_last, sizeof why_last, "%s, %u post draws before it", why, h_draws);
        return;
    }
    NvnTexture *hs = stand(&st_h, g_h, ow, oh, true);
    if (!hs) return;
    redirect = hud_seen = true;
    uint32_t dn = 0;
    NvnTexture *depth = scene_depth(&dn);
    CamRecord cam;
    bool have_cam = frame_camera(&cam);
    bool scene = depth && dn >= 16 && have_cam;
    frame_fsr = scene && run_fsr(g_h, hs, depth, frame_mask(), &cam);
    if (!frame_fsr) scale_to_stand(g_h, hs);
    {  // every change between FSR and plain scaling, with the reason (always logged; at most 500 lines)
        static int last = -1, nlog;
        const char *why_not = frame_fsr ? NULL : !depth || dn < 16 ? "no 3D scene this frame (fewer than 16 depth-writing draws)"
                            : !have_cam ? "the main camera was not computed (event camera, menu)"
                                        : "FSR did not run (see the lines above)";
        if ((int)frame_fsr != last && nlog++ < 500)
            LOG("[fsr] frame %llu: %s%s\n", (unsigned long long)g_present_count + 1, frame_fsr ? "FSR upscaling" : "plain scaling: ",
                why_not ? why_not : "");
        last = frame_fsr;
    }
    if (st_d.fake.backend) {  // the HUD's depth target starts empty (the game clears its own when it needs one)
        TexBackend *b = st_d.fake.backend;
        VkClearDepthStencilValue far = {1.0f, 0};
        VkImageSubresourceRange rg = {b->aspect, 0, 1, 0, 1};
        vkCmdClearDepthStencilImage(vk_cmd(), b->image, VK_IMAGE_LAYOUT_GENERAL, &far, 1, &rg);
        vk_full_barrier(vk_cmd());
    }
    if (logging) snprintf(why_last, sizeof why_last, "%s, %u post draws before it, depth draws %u", why, h_draws, dn);
}

void vk_fsr_draw(const FsrDraw *d) {
    if (!frame_open) frame_begin();
    last_target = d->color0;
    last_src = d->ntex ? d->tex[0] : NULL;
    if (d->depth && d->depth_write && d->depth->width == rw && d->depth->height == rh) {
        int i = 0;
        while (i < 4 && depths[i].t && depths[i].t != d->depth) i++;
        if (i < 4) depths[i].t = d->depth, depths[i].n++;
        if (jitter_on) camera_match(d);
        if (!redirect && d->color0 && d->color0 != g_h && d->color0->width == rw && d->color0->height == rh) {
            if (!mask_first) mask_first = d->color0;  // (the moving-object mask: frame_mask)
            int k = 0;
            while (k < 4 && colors[k].t && colors[k].t != d->color0) k++;
            if (k < 4) colors[k].t = d->color0, colors[k].n++;
        }
    }
    if (hud_seen || !g_h || !d->color0) return;
    if (d->color0 == g_h) {
        bool post = d->count <= 6;
        bool reads_rt = false;
        for (int i = 0; i < d->ntex; i++) reads_rt |= d->tex[i] != g_h && is_rt(d->tex[i]);
        if (post && reads_rt) { h_draws++; return; }
        hud_start("first HUD draw");
        return;
    }
    if (presented_texture(d->color0)) {
        bool reads_h = false;
        for (int i = 0; i < d->ntex; i++) reads_h |= d->tex[i] == g_h;
        if (reads_h) hud_start("final draw (no HUD)");
    }
}

NvnTexture *vk_fsr_map(NvnTexture *t, bool depth) {
    if (!redirect || !t) return t;
    if (t == g_h) return &st_h.fake;
    if (depth) {
        if (t->backend && t->width == rw && t->height == rh) {
            NvnTexture *s = stand(&st_d, t, ow, oh, false);
            return s ? s : t;
        }
        return t;
    }
    if (presented_texture(t) && t->width == rw && t->height == rh) {
        NvnTexture *s = stand(&st_p, t, ow, oh, false);
        return s ? s : t;
    }
    return t;
}

// SMAA T2x's images and pipelines (the motion vectors' as for FSR, at the render size), made the first time it is picked.
static bool t2x_init(void) {
    rw = ow = g_render_size[0], rh = oh = g_render_size[1];
    logging = getenv("MHGU_FSR_LOG") != NULL;
    VkPhysicalDeviceFeatures f;
    vkGetPhysicalDeviceFeatures(g_vk.phys, &f);
    g_fsr_objvel = f.vertexPipelineStoresAndAtomics && f.fragmentStoresAndAtomics;  // (device.c enables both where it can)
    if (!prep_init()) {
        LOG("[t2x] the motion vectors' shaders could not be built: SMAA 1x instead\n");
        g_fsr_objvel = false;
        return false;
    }
    LOG("[t2x] SMAA T2x ready at %ux%u, object motion %s\n", rw, rh, g_fsr_objvel ? "on" : "off (camera motion only)");
    return true;
}

// Without FSR, once a frame: SMAA T2x starts (from the next frame on) or stops as [Anti-aliasing] AntiAliasing says
// (live: the settings menu). true while it runs.
static bool t2x_present(void) {
    bool want = config_live_aa() == AA_SMAA_T2X;
    if (want && !t2x_ready && !t2x_failed) t2x_ready = !(t2x_failed = !t2x_init());
    want = want && t2x_ready;
    if (want == t2x_on) return want;
    t2x_on = g_temporal_on = want;
    camjit_t2x(want);
    if (want) {
        not_scene = 0;
    } else {  // (the next T2x frame starts afresh: no history, no jitter until it runs)
        camjit_set(false, rw, rh, 2);
        jitter_on = false;
        prev_cam.seq = 0;
        frame_open = redirect = hud_seen = frame_fsr = false;
        last_target = last_src = NULL;
    }
    LOG("[t2x] SMAA T2x %s\n", want ? "on" : "off");
    return want;
}

NvnTexture *vk_fsr_present(NvnTexture *t) {
    if (!g_fsr_on && !t2x_present()) return t;
    NvnTexture *show = t;
    if (redirect) {
        if (t == g_h) show = &st_h.fake;
        else if (st_p.fake.backend && presented_texture(t) && t->width == rw && t->height == rh) show = &st_p.fake;
    }
    // learn H: what the frame's last draw (the one into the presented texture) sampled
    if (last_target == t && last_src && last_src != t && last_src->width == rw && last_src->height == rh && last_src->backend &&
        !(((TexBackend *)last_src->backend)->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) {
        if (g_h != last_src) LOG("[fsr] full-screen image: %p %ux%u (nvn format %#x)\n", (void *)last_src, last_src->width,
                                 last_src->height, last_src->format);
        g_h = last_src;
    }
    // jitter: on while frames are scene frames, off after 3 that are not
    if (frame_fsr) not_scene = 0;
    else if (not_scene < 1000) not_scene++;
    bool want = not_scene < 3;
    if (want != jitter_on) {
        jitter_on = want;
        uint32_t phases = (uint32_t)(8.0f * ((float)ow / rw) * ((float)ow / rw));
        camjit_set(want, rw, rh, phases < 1 ? 1 : phases);
        if (logging) LOG("[fsr] camera jitter %s\n", want ? "on" : "off");
    }
    if (frame_fsr) n_fsr++;
    else if (redirect) n_scaled++;
    else n_none++;
    if (frame_fsr) {
        if (matched_seq) {
            n_match++;
            CamRecord c;
            if (camjit_latest(&c)) { uint64_t lag = c.seq - matched_seq; lag_hist[lag < 3 ? lag : 3]++; }
        } else {
            n_nomatch++;
        }
    }
    uint64_t now = plat_now_ns();
    if (!log_t0) log_t0 = now;
    if (check_seq && vk_completed_seq() >= check_seq) {  // (MHGU_FSR_MVCHECK)
        vmaInvalidateAllocation(g_vk.vma, check_mem, 0, 64);
        for (int i = 0; i < 16; i++) check_acc[i] += check_sums[i];
        check_seq = 0;
    }
    if (anim_seq && vk_completed_seq() >= anim_seq) {  // (animated-surface counters, MHGU_FSR_LOG)
        vmaInvalidateAllocation(g_vk.vma, anim_mem, 0, 32);
        for (int i = 0; i < 4; i++) anim_acc[i] += anim_sums[i];
        anim_seq = 0;
    }
    if (g_fsr_objvel) objvel_static_update();
    cap_frame++;
    if (logging && now - log_t0 >= 1000000000ull) {
        LOG("[fsr] 1s: FSR %u, scaled %u, neither %u | history resets %u | camera matched %u (newest %u, 1 back %u, 2 back %u, "
            "more %u), not %u | object motion %u frames, reactive %u | last HUD start: %s\n",
            n_fsr, n_scaled, n_none, n_reset, n_match, lag_hist[0], lag_hist[1], lag_hist[2], lag_hist[3], n_nomatch,
            n_objv_frames, n_react_frames, why_last);
        LOG("[fsr] 1s: upscaler %s, FSR 4 frames %u\n", use_fsr4 ? vk_fsr4_name() : "FSR 3.1", n_fsr4);
        n_fsr4 = 0;
        if (g_fsr_objvel && n_fsr) {
            uint32_t nstatic = 0;
            for (int i = 0; i < STAT_TAB; i++) nstatic += stat_tab[i].key && stat_tab[i].until > cap_frame;
            LOG("[fsr] object motion: %.1f draws captured a frame, %.1f paired and rasterized, %.1f unpaired, %.1f left out as "
                "static (%u static draws now) | capture CPU %.2f ms a frame | variants built %llu (%.0f ms), pipelines %llu\n",
                (double)n_cap / n_fsr, (double)n_pair / n_fsr, (double)n_unpaired / n_fsr, (double)n_excluded / n_fsr, nstatic,
                capture_ns / 1e6 / n_fsr, (unsigned long long)atomic_exchange(&n_progs_built, 0),
                atomic_exchange(&build_ns, 0) / 1e6, (unsigned long long)atomic_exchange(&n_pipes_built, 0));
            double f = n_fsr;
            LOG("[fsr] object motion a frame: captured %.1f (skinned %.1f, blended %.1f) | not captured: static %.1f, woken "
                "%.2f, variant / pipeline not built yet %.2f, failed %.2f, memory %.2f, no index range %.2f | unpaired: no "
                "previous copy %.2f, other vertex range %.2f%s%s\n",
                n_cap / f, n_cap_skinned / f, n_front / f, n_excluded / f, n_woken / f, n_sk_build / f, n_sk_fail / f,
                n_sk_budget / f, n_sk_range / f, n_up_none / f, n_up_range / f, unpaired_note[0] ? " | some:" : "", unpaired_note);
        }
        unpaired_note[0] = 0;
        capture_ns = 0;
        n_cap = n_pair = n_unpaired = n_excluded = 0;
        n_sk_build = n_sk_fail = n_sk_budget = n_sk_range = n_up_none = n_up_range = n_woken = n_front = n_cap_skinned = 0;
        static const char *const cls[2] = {"camera-moved texels", "moving-object mask texels"};
        for (int c = 0; c < 2; c++)
            if (check_acc[4 * c])
                LOG("[fsr] motion vector check, %s: %llu, mean luma difference to the previous frame: same texel %.4f, along "
                    "the camera vectors %.4f, along FSR's vectors %.4f\n", cls[c], (unsigned long long)check_acc[4 * c],
                    check_acc[4 * c + 1] / 1000.0 / check_acc[4 * c], check_acc[4 * c + 2] / 1000.0 / check_acc[4 * c],
                    check_acc[4 * c + 3] / 1000.0 / check_acc[4 * c]);
        if (check_acc[8]) {
            double n = (double)check_acc[8] * 1000.0;
            LOG("[fsr] motion vector check, moving geometry (valid object vector, > 0.5 px from the camera's): %llu, mean "
                "luma difference: same texel %.4f, camera vector %.4f, object vector %.4f, object vector without the jitter "
                "change %.4f, object vector with y negated %.4f\n", (unsigned long long)check_acc[8], check_acc[9] / n,
                check_acc[10] / n, check_acc[11] / n, check_acc[12] / n, check_acc[13] / n);
        }
        memset(check_acc, 0, sizeof check_acc);
        if (anim_on && anim_frames) {
            LOG("[fsr] animated surfaces: %llu frames, %.1f tiles flagged a frame (%.1f repeating), %.1f evaluated, %.1f "
                "rejected as not flat\n", (unsigned long long)anim_frames, anim_acc[0] / (double)anim_frames,
                anim_acc[3] / (double)anim_frames, anim_acc[1] / (double)anim_frames, anim_acc[2] / (double)anim_frames);
            memset(anim_acc, 0, sizeof anim_acc);
            anim_frames = 0;
        }
        n_fsr = n_scaled = n_none = n_match = n_nomatch = n_reset = n_objv_frames = n_react_frames = 0;
        memset(lag_hist, 0, sizeof lag_hist);
        log_t0 = now;
    }
    frame_open = false;
    redirect = hud_seen = false;
    frame_fsr = false;
    last_target = last_src = NULL;
    return show;
}

void vk_fsr_init(void) {
    if (!g_config.fsr || !g_fsr_output[0]) return;
    rw = g_render_size[0], rh = g_render_size[1], ow = g_fsr_output[0], oh = g_fsr_output[1];
    logging = getenv("MHGU_FSR_LOG") != NULL;
    FfxVkPortableDeviceCapabilities caps = {.structSize = sizeof caps};
    if (ffxVkPortableQueryDeviceCapabilities(g_vk.phys, &caps) != FFX_VK_PORTABLE_OK || !caps.fsr3ComputePrerequisites ||
        !g_vk.fsr_storage_wo_format) {
        LOG("[fsr] this GPU lacks what FSR 3.1 needs (subgroup operations, storage formats, storage writes without a "
            "format): FSR off, the picture is scaled from %ux%u\n", rw, rh);
        return;
    }
    if (g_config.fsr_objmotion) {  // (device.c enables both where the GPU has them)
        VkPhysicalDeviceFeatures f;
        vkGetPhysicalDeviceFeatures(g_vk.phys, &f);
        g_fsr_objvel = f.vertexPipelineStoresAndAtomics && f.fragmentStoresAndAtomics;
        if (!g_fsr_objvel)
            LOG("[fsr] object motion needs vertexPipelineStoresAndAtomics and fragmentStoresAndAtomics, which this GPU lacks: "
                "camera motion only (the reactive mask still covers moving objects)\n");
    }
    if (!prep_init()) { LOG("[fsr] the prep shader could not be built: FSR off\n"); g_fsr_objvel = false; return; }
    g_fsr_on = g_temporal_on = true;
    camjit_set(false, rw, rh, 8);
    if (g_config.fsr_upscaler == FSR_UP_4) {
        use_fsr4 = vk_fsr4_init();
        LOG("[fsr] Upscaler = FSR4: %s\n", use_fsr4 ? vk_fsr4_name() : "not available, FSR 3.1 is used");
    }
    LOG("[fsr] on (%s): %ux%u -> %ux%u, sharpness %.2f, fp16 %d, object motion %s, reactive %.2f\n",
        use_fsr4 ? vk_fsr4_name() : "FSR 3.1", rw, rh, ow, oh, g_config.fsr_sharpness, g_vk.fsr_fp16, g_fsr_objvel ? "on" : "off",
        g_config.fsr_reactive);
}
