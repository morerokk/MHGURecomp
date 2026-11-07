// Native HDR output ([HDR] HDR = On): runtime/nvn/vulkan/hdr.c. The swapchain side (HDR10 / scRGB window images, the
// composition image the game and the overlay are drawn into) lives in device.c.
#pragma once
#include "fsr.h"
#include "vk.h"

enum { HDR_OUT_NONE, HDR_OUT_PQ, HDR_OUT_SCRGB };  // the window image: SDR, HDR10 (BT.2020 PQ), scRGB (linear FP16)
enum { HDR_PASS_NONE, HDR_PASS_GRADE, HDR_PASS_BLOOM, HDR_PASS_HUD };  // ShaderKey.hdr: the game's passes drawn as HDR variants
#define HDR_PARAMS_BINDING 23                            // (fragment set) the HDR variants' settings (vk_hdr_params)

extern int g_hdr_out;  // HDR_OUT_*: what the window image is now (device.c create_swapchain)

static inline bool vk_hdr_tracking(void) { return g_hdr_out != HDR_OUT_NONE; }
// exec.c draw(), before the draw: HDR_PASS_* when it is the colour grading or the bloom add (draw it through the HDR
// variant of its program), else 0. fs_hash: its fragment program's code hash.
int vk_hdr_draw(const FsrDraw *d, uint64_t fs_hash);
bool vk_hdr_rt_float(NvnTexture *t);              // exec.c begin_pass: render t as RGBA16F (the post chain, the scene)
char *vk_hdr_wrap_fs(const char *src, int pass);  // exec.c compile_program: the HDR variant's source (malloc'd)
char *vk_hdr_guard_fs(const char *src);           // exec.c compile_program: other passes, outputs as RGBA8 bounds them (no NaN)
#define HDR_NPARAMS 24
void vk_hdr_params(float out[HDR_NPARAMS]);       // exec.c stage_set: binding HDR_PARAMS_BINDING's contents
void vk_hdr_present(NvnTexture *presented);                 // device.c backend_present: learns the full-screen image
bool vk_hdr_presented_bt2020(void);
float vk_hdr_gamma(void);  // the intermediate encoding's gamma (0: sRGB)  // exec.c vk_present_draw: the presented picture is in BT.2020 (a graded frame)
void vk_hdr_variant_used(int pass, bool ready, int state);  // (exec.c draw: logged when it changes)
// device.c, inside the HDR render pass on the window image: the composition (the game's picture in the intermediate
// encoding + the overlay) -> HDR10 / scRGB.
void vk_hdr_encode(VkCommandBuffer cb, VkRenderPass rp, TexBackend *compose);
void vk_hdr_metadata_values(float *peak_nits, float *frame_avg_nits);
// device.c backend_present, outside a render pass: the HDR analysis of the presented picture ([HDR] Analysis: hdr_stats.h)
void vk_hdr_analyze(VkCommandBuffer cb, TexBackend *presented);  // VK_EXT_hdr_metadata, from the settings now
