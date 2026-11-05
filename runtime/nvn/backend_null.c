// Null renderer: accepts NVN work and draws nothing (MHGU_NULL_GPU=1, and the build without the Vulkan backend).
// Fences are satisfied at once; presents only count frames.
#include "nvn.h"

#ifdef MHGU_NULL_BACKEND
static uint64_t frames;
void backend_init(void) { LOG("[nvn] null renderer\n"); }
void backend_loading_screen(void) {}
bool shader_prewarm_progress(uint64_t *done, uint64_t *total) {
    if (done) *done = 0;
    if (total) *total = 0;
    return false;
}
void backend_execute(uint64_t handle) { (void)handle; }
void backend_present(NvnTexture *tex) {
    (void)tex;
    g_present_count++;
    if ((++frames & 63) == 0) LOG("[nvn] %llu frames presented\n", (unsigned long long)frames);
    plat_sleep_until_ns(plat_now_ns() + 16666667);
}
void backend_texture_destroyed(NvnTexture *tex) { (void)tex; }
void backend_pool_written(uint64_t addr, uint64_t size) { (void)addr; (void)size; }
void backend_program_created(const NvnProgram *p) { (void)p; }
void backend_photo_requested(void) {}
void renderer_sync(void) {}
uint64_t renderer_fence_seq(void) { return 0; }
void renderer_wait_seq(uint64_t seq) { (void)seq; }
void renderer_flush(void) {}
#endif
