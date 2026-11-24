// The second display's picture from the game's own HUD: bottom_hud.c.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include "../../config.h"  // (MHGU_DUAL_SCREEN: Android builds)

#ifdef MHGU_DUAL_SCREEN
// On while the bottom image is made: the panel is showing (second_display.c) or MHGU_BOTTOM_HUD_TEST is set. All the
// frame path reads, so that with the feature off a draw costs one load and one branch not taken.
extern volatile bool g_bottom_hud_on;

// Where a HUD piece goes on the bottom screen ([Dual Screen] Panels), from its layer and its place on the 1280x720
// canvas (the translation of its model matrix), or -1: it stays on the top screen only.
int bottom_hud_group(float z, float x, float y);

// One HUD draw of a group, as exec.c issued it in the game's pass: drawn again into the bottom image when that pass ends.
// The buffers and descriptor sets are the pass' own (transient memory of the same submission).
typedef struct {
    int group;
    VkPipeline pipe;
    VkPipelineLayout layout;
    VkDescriptorSet sets[2];
    uint32_t vb_mask;  // vertex streams bound (bit per binding)
    VkBuffer vb[17];
    VkDeviceSize vo[17];
    bool indexed;
    VkBuffer ib;
    VkDeviceSize io;
    VkIndexType it;
    uint32_t count, inst, first, base_inst;
    int32_t base_vertex;
    float blend[4];
    float outline[4];  // the piece itself on the canvas (x0, y0, x1, y1, from its vertices; all 0: unknown)
    float clip[4];  // the game's scissor for it, on the 1280x720 canvas (x0, y0, x1, y1): the parts of a big picture
                    // the game keeps out of sight (the zoomed minimap) stay out on the panel too
} BottomDraw;
void bottom_hud_record(const BottomDraw *d);
// At the end of a pass while the bottom screen is off: the pass' recorded draws dropped (their buffers and descriptor
// sets are that submission's own and must never be replayed in a later one).
void bottom_hud_discard(void);

// At the end of the game's pass (outside it): the recorded draws into the bottom image, through a pass of the same
// attachment formats, then a copy into host memory for the panel. seq: the submission this command buffer goes out as.
// True when it drew (the command buffer's bound state is then its own).
bool bottom_hud_flush(VkCommandBuffer cb, VkRenderPass rp, const VkFormat *color, uint32_t ncolor, VkFormat depth,
                      uint32_t samples, uint64_t seq);

// Panel thread: the size of the upright picture (the panel's mode, turned), once; the game makes its images at its
// next flush.
void bottom_hud_set_size(uint32_t w, uint32_t h);
// Panel thread: is upright pixel u, v of a w x h picture on a placed panel (the rest shows the wallpaper)?
bool bottom_hud_covers(uint32_t u, uint32_t v, uint32_t w, uint32_t h);
// Panel thread: no panel piece drawn for a while (out of the quest): time to show the empty panels.
bool bottom_hud_stale(void);
// Panel thread: the bottom screen's background, with the panels empty on it when panels is set, into an RGBA picture of
// w x h (upright), on the CPU.
void bottom_hud_empty_picture(uint32_t *px, uint32_t w, uint32_t h, bool panels);
// The panel's picture size (bottom_hud_set_size's), 0 before the first.
void bottom_hud_size(uint32_t *w, uint32_t *h);
// Test mode (MHGU_BOTTOM_HUD_TEST): on without a panel; false when the variable is not set.
bool bottom_hud_test_init(void);
// Panel thread: the newest finished picture newer than *seq, handed to copy (RGBA8, w x h, upright) while it is held.
// False: none. Never waits.
bool bottom_hud_take(void (*copy)(const uint32_t *rgba, uint32_t w, uint32_t h, void *ctx), void *ctx, uint64_t *seq);
// Touch thread: the finger on the panel, in upright picture pixels (down false: lifted). Holds that panel's action.
void bottom_hud_touch(bool down, uint32_t u, uint32_t v);
// Input loop, each update: the touched panel's buttons added, its right stick in place of the pad's when it has one.
void bottom_hud_touch_input(uint64_t *buttons, int32_t *rx, int32_t *ry);
#endif
