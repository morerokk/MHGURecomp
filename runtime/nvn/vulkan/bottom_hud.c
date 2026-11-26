// From cerroverb's MHGURecomp-aarch64 fork (89c29ab, MIT like the rest of this repo), where a DRM lease puts it on a
// dual-screen handheld's panel under Linux; here the panel is Android's (second_display_android.c).
//
// The second display's picture, made from the game's own HUD. The pieces of a few HUD groups (bottom_hud_group) are
// drawn a second time, as the game drew them, into an image of the panel's size: the same pipelines, descriptor sets
// and buffers, only the viewport and scissor move them to their place on the panel. They leave the top screen, as on
// MHXX.
// That image is copied to host memory in the same submission; the panel thread (second_display.c) takes the newest copy
// the GPU has finished. Nothing on the game's side waits: the extra draws and the copy go into the command buffer the
// frame records anyway, and a slot the panel is reading is skipped.
//   MHGU_BOTTOM_HUD_TEST=<w>x<h>   (test) make the bottom image without a panel, saved as shots/bottom_<n>.png every 2 s
#include "../../config.h"  // (MHGU_DUAL_SCREEN)
#ifdef MHGU_DUAL_SCREEN
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "vk.h"
#include "bottom_hud.h"
#include "../../rt.h"
#include "../../hle/input.h"
#include "../../hle/itembar.h"
#include "second_display.h"

volatile bool g_bottom_hud_on;
extern volatile int g_hudtrace_active;  // (exec.c)

// ---- the MHXX bottom screen: its 320x240 picture, centred on the panel at the largest whole fit, with MHXX's panel
// base: a grid of 4 x 3 cells under the top bar (cell edges measured on MHXX's quest screen, test/mhxx-reference,
// Azahar 2126.2). [Dual Screen] Panels places panels on it, as MHXX's Panel Customize does.
static const int16_t cell_x[5] = {2, 81, 161, 240, 319}, cell_y[4] = {26, 97, 168, 237};

// A part of the game's 1280x720 HUD canvas: its pieces, by layer and place (hudtrace in a quest, 2026-11-20).
typedef struct { float z, x0, y0, x1, y1; } Source;
#define ARTS_ROW(i) {100.0f, 640.0f, 560.0f + 50.0f * (i), 980.0f, 610.0f + 50.0f * (i) + ((i) == 2 ? 5.0f : 0.0f)}
// The panels: name, size in cells, MHXX's dark (Hunter Arts) or light frame, and the canvas parts drawn in them. With
// three parts the panel is split in three, down (arts3v) or across (arts3h).
typedef struct { const char *name; uint8_t cols, rows; bool dark, across; uint8_t nsrc; Source src[3]; } Kind;
enum { K_ARTS3V, K_ARTS3H, K_MAP, K_ITEMBAR, K_STATUS, K_PALICO, K_TARGET, K_DPAD, NKINDS };
static const Kind kinds[NKINDS] = {
    [K_ARTS3V] = {"arts3v", 2, 3, true, false, 3, {ARTS_ROW(0), ARTS_ROW(1), ARTS_ROW(2)}},
    [K_ARTS3H] = {"arts3h", 4, 1, true, true, 3, {ARTS_ROW(0), ARTS_ROW(1), ARTS_ROW(2)}},
    // (the pieces of all of the minimap's views; the panel fits the map picture's outline, find_map, else this frame:
    // the zone view's place)
    [K_MAP] = {"map", 2, 2, false, false, 1, {{105.0f, 900.0f, 60.0f, 1280.0f, 360.0f}}},
    [K_ITEMBAR] = {"itembar", 2, 1, false, false, 1, {{103.0f, 980.0f, 560.0f, 1280.0f, 720.0f}}},
    [K_STATUS] = {"status", 4, 1, false, false, 1, {{103.0f, 0.0f, 0.0f, 760.0f, 95.0f}}},
    [K_PALICO] = {"palico", 4, 1, false, false, 1, {{103.0f, 0.0f, 95.0f, 760.0f, 200.0f}}},
    [K_TARGET] = {"target", 2, 1, false, false, 1, {{100.0f, 1150.0f, 20.0f, 1260.0f, 100.0f}}},
    [K_DPAD] = {"dpad", 2, 2, false, false, 0, {{0}}},
};

// The panels placed, from [Dual Screen] Panels ("name@column,row ..."), read once at the first HUD piece. A panel
// that overlaps one placed before it replaces it, as on MHXX.
#define MAX_PLACED 12
typedef struct { uint8_t kind, col, row; } Placed;
static Placed placed[MAX_PLACED];
static int nplaced = -1;
static char placed_from[sizeof g_config.bottom_panels];  // (the list they come from: a change in the settings menu applies
                                                         // at the next HUD pass)

static bool overlaps(const Placed *a, const Placed *b) {
    const Kind *ka = &kinds[a->kind], *kb = &kinds[b->kind];
    return a->col < b->col + kb->cols && b->col < a->col + ka->cols && a->row < b->row + kb->rows && b->row < a->row + ka->rows;
}

static void place_panels(const char *list) {
    nplaced = 0;
    snprintf(placed_from, sizeof placed_from, "%s", g_config.bottom_panels);
    char buf[256];
    snprintf(buf, sizeof buf, "%s", list);
    for (char *save = NULL, *tok = strtok_r(buf, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save)) {
        char name[16];
        int col, row, k;
        if (sscanf(tok, "%15[a-z0-9]@%d,%d", name, &col, &row) != 3) { LOG("[bottom-hud] panel '%s': not name@column,row\n", tok); continue; }
        for (k = 0; k < NKINDS && strcmp(name, kinds[k].name); k++) {}
        if (k == NKINDS) { LOG("[bottom-hud] panel '%s': no such panel\n", name); continue; }
        if (col < 0 || row < 0 || col + kinds[k].cols > 4 || row + kinds[k].rows > 3) { LOG("[bottom-hud] panel '%s' does not fit at %d,%d\n", name, col, row); continue; }
        Placed p = {(uint8_t)k, (uint8_t)col, (uint8_t)row};
        int kept = 0;
        for (int i = 0; i < nplaced; i++) if (!overlaps(&placed[i], &p)) placed[kept++] = placed[i];
        nplaced = kept;
        if (nplaced < MAX_PLACED) placed[nplaced++] = p;
    }
    LOG("[bottom-hud] %d panels from \"%s\"\n", nplaced, list);
}

// A placed panel's part (sub 0..2), in 3DS bottom screen pixels.
static void part_rect(int p, int sub, float r[4]) {
    const Kind *k = &kinds[placed[p].kind];
    float x0 = cell_x[placed[p].col], x1 = cell_x[placed[p].col + k->cols], y0 = cell_y[placed[p].row], y1 = cell_y[placed[p].row + k->rows];
    if (k->nsrc == 3 && k->across) {
        float w = (x1 - x0) / 3.0f;
        x0 += w * (float)sub, x1 = x0 + w;
    } else if (k->nsrc == 3) {
        float h = (y1 - y0) / 3.0f;
        y0 += h * (float)sub, y1 = y0 + h;
    }
    r[0] = x0, r[1] = y0, r[2] = x1, r[3] = y1;
}

// The framed boxes of a placed panel: one per part, as MHXX frames each Hunter Art on its own, else the whole panel.
// Box i of the count returned, in 3DS bottom screen pixels.
static int panel_boxes(int p) { return kinds[placed[p].kind].nsrc == 3 ? 3 : 1; }
static void box_rect(int p, int i, float r[4]) {
    const Kind *k = &kinds[placed[p].kind];
    part_rect(p, i, r);
    if (panel_boxes(p) == 1) r[2] = cell_x[placed[p].col + k->cols], r[3] = cell_y[placed[p].row + k->rows];
}

// The 3DS picture on the panel: scale and top left, from the upright size.
static void screen_fit(uint32_t w, uint32_t h, float *scale, float *x0, float *y0) {
    float s = (float)w / 320.0f < (float)h / 240.0f ? (float)w / 320.0f : (float)h / 240.0f;
    *scale = s, *x0 = ((float)w - 320.0f * s) * 0.5f, *y0 = ((float)h - 240.0f * s) * 0.5f;
}

// In a quest: the quest clock (layer 103, its pieces at the top left) was drawn this frame or the last. The panels
// take pieces only then; in town the same layers and places hold other windows, which stay on the top screen.
static uint64_t clock_frame;
static bool in_quest(float z, float x, float y) {
    if (z > 102.5f && z < 103.5f && x < 60.0f && y < 70.0f) clock_frame = g_present_count;
    return clock_frame + 1 >= g_present_count && clock_frame;
}

// A HUD piece's place on the bottom screen: placed panel * 3 + part, or -1 (it stays on the top screen only).
int bottom_hud_group(float z, float x, float y) {
    if (nplaced < 0) place_panels(g_config.bottom_panels[0] ? g_config.bottom_panels : BOTTOM_PANELS_DEFAULT);
    if (!in_quest(z, x, y)) return -1;
    for (int p = 0; p < nplaced; p++) {
        const Kind *k = &kinds[placed[p].kind];
        for (int i = 0; i < k->nsrc; i++) {
            const Source *g = &k->src[i];
            if (z > g->z - 0.5f && z < g->z + 0.5f && x >= g->x0 && x <= g->x1 && y >= g->y0 && y <= g->y1) return p * 3 + i;
        }
    }
    return -1;
}


// The last game frame that drew a panel piece: the panel shows the empty panels again a while after it (back in town)
static _Atomic uint64_t last_piece_frame;
bool bottom_hud_stale(void) {
    uint64_t f = atomic_load(&last_piece_frame);
    return f && g_present_count > f + 30;  // (half a second at 60 fps: a loading screen or a menu in a quest is longer)
}

// ---- the draws of the pass being recorded
#define MAX_DRAWS 256
static BottomDraw draws[MAX_DRAWS];
static int ndraws;

void bottom_hud_record(const BottomDraw *d) {
    if (ndraws < MAX_DRAWS) draws[ndraws++] = *d;
}
void bottom_hud_discard(void) { ndraws = 0; }

// ---- the bottom image: one image per attachment of the game's HUD pass (the pipelines need a pass of the same
// formats), and an RGBA8 image the colour is converted into for the copy
static _Atomic uint32_t want_w, want_h;
static uint32_t img_w, img_h;
static struct { VkFormat format; TexBackend b; VkImageView view; } att[9];
static uint32_t natt;
static TexBackend rgba;
static uint64_t cleared_frame = UINT64_MAX;

void bottom_hud_set_size(uint32_t w, uint32_t h) {
    atomic_store(&want_h, h);
    atomic_store(&want_w, w);
}
void bottom_hud_size(uint32_t *w, uint32_t *h) { *w = atomic_load(&want_w), *h = atomic_load(&want_h); }

static bool make_image(TexBackend *b, VkFormat f, VkImageAspectFlags aspect, VkImageUsageFlags usage) {
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                            .extent = {img_w, img_h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    memset(b, 0, sizeof *b);
    if (vmaCreateImage(g_vk.vma, &ci, &ai, &b->image, &b->mem, NULL) != VK_SUCCESS) return false;
    b->format = f, b->base_type = VK_IMAGE_VIEW_TYPE_2D, b->width = img_w, b->height = img_h;
    b->depth = b->layers = b->levels = b->samples = 1;
    b->aspect = aspect, b->gpu_owned = true;
    vk_image_init_layout(vk_upload_cmd(), b->image, aspect);
    return true;
}

static VkImageAspectFlags depth_aspect(VkFormat f) {
    return f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_X8_D24_UNORM_PACK32 ? VK_IMAGE_ASPECT_DEPTH_BIT
         : f == VK_FORMAT_S8_UINT ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
}

// The attachments for these formats at the panel's size, made once (the HUD pass keeps its formats).
static bool ensure_images(const VkFormat *color, uint32_t ncolor, VkFormat depth) {
    VkFormat want[9];
    uint32_t n = 0;
    for (uint32_t i = 0; i < ncolor; i++) if (color[i] != VK_FORMAT_UNDEFINED) want[n++] = color[i];
    if (depth != VK_FORMAT_UNDEFINED) want[n++] = depth;
    bool same = n == natt && img_w;
    for (uint32_t i = 0; same && i < n; i++) same = att[i].format == want[i];
    if (same) return true;
    if (natt) {
        static bool told;
        if (!told) told = true, LOG("[bottom-hud] a HUD pass of other formats: not drawn\n");
        return false;
    }
    img_w = atomic_load(&want_w), img_h = atomic_load(&want_h);
    static const VkComponentMapping id = {0};
    for (uint32_t i = 0; i < n; i++) {
        bool is_depth = depth != VK_FORMAT_UNDEFINED && i == n - 1;
        VkImageAspectFlags a = is_depth ? depth_aspect(want[i]) : VK_IMAGE_ASPECT_COLOR_BIT;
        VkImageUsageFlags u = is_depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                       : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (!make_image(&att[i].b, want[i], a, u)) { LOG("[bottom-hud] could not make a %ux%u image\n", img_w, img_h); return false; }
        att[i].format = want[i];
        att[i].view = vk_image_view(&att[i].b, VK_IMAGE_VIEW_TYPE_2D, want[i], 0, 1, 0, 1, id, a);
    }
    if (!make_image(&rgba, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT,
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return false;
    natt = n;
    LOG("[bottom-hud] %ux%u, %u attachments, colour format %d\n", img_w, img_h, n, att[0].format);
    return true;
}

// ---- host copies: the panel reads one slot while the game fills another
#define SLOTS 3
#define PENDING UINT64_MAX
static struct { VkBuffer buf; VmaAllocation mem; uint32_t *px; _Atomic uint64_t seq; } slot[SLOTS];
static _Atomic int reading = -1;
static int last_slot;

static bool ensure_slots(void) {
    if (slot[0].px) return true;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)img_w * img_h * 4,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    for (int i = 0; i < SLOTS; i++) {
        VmaAllocationInfo info;
        if (vmaCreateBuffer(g_vk.vma, &bi, &ai, &slot[i].buf, &slot[i].mem, &info) != VK_SUCCESS) {
            LOG("[bottom-hud] no host memory for the copies\n");
            return false;
        }
        slot[i].px = info.pMappedData;
        atomic_store(&slot[i].seq, 0);
    }
    return true;
}

// A slot the panel isn't reading, marked pending before the check (the panel marks reading before it checks the seq:
// one of the two sees the other).
static int take_slot(void) {
    for (int k = 1; k <= SLOTS; k++) {
        int i = (last_slot + k) % SLOTS;
        uint64_t old = atomic_exchange(&slot[i].seq, PENDING);
        if (atomic_load(&reading) != i) return last_slot = i;
        atomic_store(&slot[i].seq, old);
    }
    return -1;
}

static void host_read_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                         .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &b, 0, NULL, 0, NULL);
}

// Colour attachment 0 as RGBA8 into a free slot, which then carries this submission's number.
static void copy_out(VkCommandBuffer cb, uint64_t seq) {
    int i = take_slot();
    if (i < 0) return;
    VkImage src = att[0].b.image;
    if (att[0].format != VK_FORMAT_R8G8B8A8_UNORM) {
        VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int32_t)img_w, (int32_t)img_h, 1}},
                          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {(int32_t)img_w, (int32_t)img_h, 1}}};
        vkCmdBlitImage(cb, src, VK_IMAGE_LAYOUT_GENERAL, rgba.image, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_NEAREST);
        vk_full_barrier(cb);
        src = rgba.image;
    }
    VkBufferImageCopy rg = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {img_w, img_h, 1}};
    vkCmdCopyImageToBuffer(cb, src, VK_IMAGE_LAYOUT_GENERAL, slot[i].buf, 1, &rg);
    host_read_barrier(cb);
    atomic_store(&slot[i].seq, seq);
}

// ---- the panels' own picture: MHXX's colours for them (sampled from the reference screenshot), drawn as filled
// rectangles until the game's own panel graphics (custom_panel) take their place
typedef struct { float r, g, b; } Colour;
#define RGB(h) {((h) >> 16 & 0xff) / 255.0f, ((h) >> 8 & 0xff) / 255.0f, ((h) & 0xff) / 255.0f}
static _Atomic int touched = -1;  // the panel part under the finger (panel * 3 + part), -1 none: drawn lit, as on MHXX
static const Colour lit = RGB(0xf0c838);  // (a touched panel's frame: MHXX lights the panel under the finger)
static const Colour screen_bg = RGB(0x292d29), art_frame = RGB(0x8e8563), art_fill = RGB(0x6e684d),
                    pad_frame = RGB(0xb7a97b), pad_fill = RGB(0xe6e3c5);

// [Dual Screen] Background: MHXX's dark grey under the panels, or black (an OLED panel's pixels off). With the wallpaper
// the background is left transparent (alpha 0) and the panel thread puts the wallpaper there (second_display.c).
static Colour background(void) { return g_config.bottom_background == BOTTOM_BG_BLACK ? (Colour){0, 0, 0} : screen_bg; }
static float background_alpha(void) { return g_config.bottom_background == BOTTOM_BG_WALLPAPER ? 0.0f : 1.0f; }

static void fill_alpha(VkCommandBuffer cb, float x0, float y0, float x1, float y1, Colour c, float alpha) {
    VkClearAttachment a = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = 0,
                           .clearValue.color = {{c.r, c.g, c.b, alpha}}};
    VkClearRect r = {.rect = {{(int32_t)x0, (int32_t)y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}}, .layerCount = 1};
    vkCmdClearAttachments(cb, 1, &a, 1, &r);
}
static void fill(VkCommandBuffer cb, float x0, float y0, float x1, float y1, Colour c) { fill_alpha(cb, x0, y0, x1, y1, c, 1.0f); }

static void draw_panels(VkCommandBuffer cb) {
    float s, ox, oy;
    screen_fit(img_w, img_h, &s, &ox, &oy);
    fill_alpha(cb, ox, oy, ox + 320.0f * s, oy + 240.0f * s, background(), background_alpha());
    for (int p = 0; p < nplaced; p++) {
        const Kind *k = &kinds[placed[p].kind];
        float r[4], x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        for (int i = 0; i < panel_boxes(p); i++) {
            box_rect(p, i, r);
            float b = 2.0f * s;
            x0 = ox + r[0] * s, y0 = oy + r[1] * s, x1 = ox + r[2] * s - s, y1 = oy + r[3] * s - s;
            fill(cb, x0, y0, x1, y1, k->dark ? art_frame : pad_frame);
            fill(cb, x0 + b, y0 + b, x1 - b, y1 - b, k->dark ? art_fill : pad_fill);
        }
        int t = atomic_load(&touched);
        if (t >= 0 && t / 3 == p) {  // (the touched part: a lit frame round it)
            float r[4];
            part_rect(p, t % 3, r);
            float lx0 = ox + r[0] * s, ly0 = oy + r[1] * s, lx1 = ox + r[2] * s - s, ly1 = oy + r[3] * s - s, w = 2.0f * s;
            fill(cb, lx0, ly0, lx1, ly0 + w, lit), fill(cb, lx0, ly1 - w, lx1, ly1, lit);
            fill(cb, lx0, ly0, lx0 + w, ly1, lit), fill(cb, lx1 - w, ly0, lx1, ly1, lit);
        }
        if (placed[p].kind == K_DPAD) {  // (the D-pad panel's cross, as MHXX draws it there; its graphic is not in MHGU's files)
            float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f, arm = 52.0f * s, half = 14.0f * s;
            fill(cb, cx - arm, cy - half, cx + arm, cy + half, pad_frame);
            fill(cb, cx - half, cy - arm, cx + half, cy + arm, pad_frame);
        }
    }
}

// Panel thread: is upright pixel u, v of a w x h picture on a placed panel? Everywhere else the wallpaper shows. (By place,
// not by the picture's alpha: the game's HUD pieces write alpha as they blend, inside the panels too.)
bool bottom_hud_covers(uint32_t u, uint32_t v, uint32_t w, uint32_t h) {
    float s, ox, oy;
    screen_fit(w, h, &s, &ox, &oy);
    float x = ((float)u - ox) / s, y = ((float)v - oy) / s;
    for (int p = 0; p < nplaced; p++) {
        const Kind *k = &kinds[placed[p].kind];
        if (x >= cell_x[placed[p].col] && x < cell_x[placed[p].col + k->cols] && y >= cell_y[placed[p].row] &&
            y < cell_y[placed[p].row + k->rows])
            return true;
    }
    return false;
}

// The panels with nothing in them, drawn by the panel thread into its own RGBA picture (w x h, upright): what the
// bottom screen shows before the game's first picture of a quest. The same rectangles as draw_panels.
static void cpu_fill_alpha(uint32_t *px, uint32_t w, uint32_t h, float x0, float y0, float x1, float y1, Colour c, uint32_t a) {
    uint32_t v = a << 24 | (uint32_t)(c.b * 255.0f) << 16 | (uint32_t)(c.g * 255.0f) << 8 | (uint32_t)(c.r * 255.0f);
    for (uint32_t y = y0 < 0 ? 0 : (uint32_t)y0; y < h && y < (uint32_t)y1; y++)
        for (uint32_t x = x0 < 0 ? 0 : (uint32_t)x0; x < w && x < (uint32_t)x1; x++) px[y * w + x] = v;
}
static void cpu_fill(uint32_t *px, uint32_t w, uint32_t h, float x0, float y0, float x1, float y1, Colour c) {
    cpu_fill_alpha(px, w, h, x0, y0, x1, y1, c, 0xff);
}

void bottom_hud_empty_picture(uint32_t *px, uint32_t w, uint32_t h, bool panels) {
    if (nplaced < 0) place_panels(g_config.bottom_panels[0] ? g_config.bottom_panels : BOTTOM_PANELS_DEFAULT);
    float s, ox, oy;
    screen_fit(w, h, &s, &ox, &oy);
    uint32_t a = background_alpha() > 0 ? 0xff : 0;
    cpu_fill_alpha(px, w, h, 0, 0, (float)w, (float)h, (Colour){0, 0, 0}, a);
    cpu_fill_alpha(px, w, h, ox, oy, ox + 320.0f * s, oy + 240.0f * s, background(), a);
    if (!panels) return;  // (out of a quest: the background alone, so it shows the bottom screen is on)
    for (int p = 0; p < nplaced; p++) {
        const Kind *k = &kinds[placed[p].kind];
        for (int i = 0; i < panel_boxes(p); i++) {
            float r[4];
            box_rect(p, i, r);
            float x0 = ox + r[0] * s, y0 = oy + r[1] * s, x1 = ox + r[2] * s - s, y1 = oy + r[3] * s - s, b = 2.0f * s;
            cpu_fill(px, w, h, x0, y0, x1, y1, k->dark ? art_frame : pad_frame);
            cpu_fill(px, w, h, x0 + b, y0 + b, x1 - b, y1 - b, k->dark ? art_fill : pad_fill);
        }
    }
}

// ---- flush: the panels, the recorded draws, then the copy
// The minimap this pass: the outline of its largest piece, the map picture, as far as the game's scissor shows it. The
// zone view and the whole-area view (Y in the pause menu, after the map is taken) draw the same piece at the same place
// with other vertices, and every area its own size: the panel fits what is drawn, on any map.
static Source map_src;
static bool map_src_found;
static void find_map(const BottomDraw *d, int n) {
    map_src_found = false;
    float best = 0;
    for (int i = 0; i < n; i++) {
        if (placed[d[i].group / 3].kind != K_MAP) continue;
        float x0 = fmaxf(d[i].outline[0], d[i].clip[0]), y0 = fmaxf(d[i].outline[1], d[i].clip[1]);
        float x1 = fminf(d[i].outline[2], d[i].clip[2]), y1 = fminf(d[i].outline[3], d[i].clip[3]);
        float area = (x1 - x0) * (y1 - y0);
        if (x1 > x0 && y1 > y0 && area > best) best = area, map_src = (Source){105.0f, x0, y0, x1, y1}, map_src_found = true;
    }
    // (no map picture: before the map is taken from the supply box the whole-area view draws only its markers, which
    // keep their place in the minimap's own frame)
    if (best < 60.0f * 60.0f) map_src_found = false;
}

// A Hunter Art's two parts in its row (row 0; each next row 50 lower, hudtrace 2026-11-21): the name bar and the button
// prompt left of it (ZL + the button). In the Art's box the bar goes on top, as wide as the box, and the prompt below it,
// centred, as MHXX lays out its Hunter Arts panels.
static const Source art_bar = {100.0f, 722.0f, 565.0f, 962.0f, 612.0f}, art_prompt = {100.0f, 656.0f, 580.0f, 726.0f, 612.0f};
#define ART_ROW_STEP 50.0f
static void art_layout(const BottomDraw *d, int sub, float r[4], Source *src) {
    // (by the piece's centre: the button reaches 726; a piece of unknown outline stays with the bar)
    bool prompt = d->outline[2] > 0 && (d->outline[0] + d->outline[2]) * 0.5f < art_bar.x0;
    *src = prompt ? art_prompt : art_bar;
    src->y0 += ART_ROW_STEP * (float)sub, src->y1 += ART_ROW_STEP * (float)sub;
    r[2] -= 1.0f, r[3] -= 1.0f;  // (the box as drawn: one pixel short of the next, draw_panels)
    float w = r[2] - r[0], h = r[3] - r[1];
    r[0] += 0.04f * w, r[2] -= 0.04f * w;
    if (prompt) r[1] += 0.64f * h, r[3] -= 0.06f * h;
    else r[1] += 0.08f * h, r[3] = r[1] + 0.54f * h;
}

static void replay(VkCommandBuffer cb, const BottomDraw *d) {
    int p = d->group / 3, sub = d->group % 3;
    const Source *g = &kinds[placed[p].kind].src[sub];
    static const Source map_frame = {105.0f, 1030.0f, 130.0f, 1200.0f, 315.0f};
    if (placed[p].kind == K_MAP) g = map_src_found ? &map_src : &map_frame;
    float s, ox, oy, r[4];
    screen_fit(img_w, img_h, &s, &ox, &oy);
    part_rect(p, sub, r);
    Source art_part;
    if (kinds[placed[p].kind].nsrc == 3) art_layout(d, sub, r, &art_part), g = &art_part;
    float sw = (r[2] - r[0]) * s, sh = (r[3] - r[1]) * s;
    float k = sw / (g->x1 - g->x0) < sh / (g->y1 - g->y0) ? sw / (g->x1 - g->x0) : sh / (g->y1 - g->y0);
    float w = (g->x1 - g->x0) * k, h = (g->y1 - g->y0) * k;
    float left = ox + r[0] * s + (sw - w) * 0.5f, top = oy + r[1] * s + (sh - h) * 0.5f;
    float vx = left - g->x0 * k, vy = top - g->y0 * k;
    // (flipped as exec.c set_viewport does, so the pieces come out as the game's own pass draws them)
    VkViewport vp = {vx, vy + 720.0f * k, 1280.0f * k, -720.0f * k, 0.0f, 1.0f};
    // (the panel part, narrowed to the game's own scissor for the piece)
    float c0 = fmaxf(left, vx + d->clip[0] * k), c1 = fmaxf(top, vy + d->clip[1] * k);
    float c2 = fminf(left + w, vx + d->clip[2] * k), c3 = fminf(top + h, vy + d->clip[3] * k);
    if (c0 < 0) c0 = 0;
    if (c1 < 0) c1 = 0;
    if (c2 <= c0 || c3 <= c1) return;
    VkRect2D sc = {{(int32_t)c0, (int32_t)c1}, {(uint32_t)(c2 - c0), (uint32_t)(c3 - c1)}};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d->pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d->layout, 0, 2, d->sets, 0, NULL);
    for (uint32_t s = 0; s < 17; s++)
        if (d->vb_mask >> s & 1) vkCmdBindVertexBuffers(cb, s, 1, &d->vb[s], &d->vo[s]);
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdSetBlendConstants(cb, d->blend);
    vkCmdSetDepthBias(cb, 0.0f, 0.0f, 0.0f);
    if (d->indexed) {
        vkCmdBindIndexBuffer(cb, d->ib, d->io, d->it);
        vkCmdDrawIndexed(cb, d->count, d->inst, 0, d->base_vertex, d->base_inst);
    } else {
        vkCmdDraw(cb, d->count, d->inst, d->first, d->base_inst);
    }
}

bool bottom_hud_flush(VkCommandBuffer cb, VkRenderPass rp, const VkFormat *color, uint32_t ncolor, VkFormat depth,
                      uint32_t samples, uint64_t seq) {
    int n = ndraws;
    ndraws = 0;
    if (nplaced >= 0 && strcmp(placed_from, g_config.bottom_panels)) nplaced = -1;  // (the settings menu changed them)
    if (n) atomic_store(&last_piece_frame, g_present_count);
    if (!n || !atomic_load(&want_w)) return false;  // (only a pass that drew panel pieces: other passes, a shadow map's
                                                     // depth-only one among them, are never touched)
    bool first = cleared_frame != g_present_count;  // (the first of this frame's HUD passes starts the picture)
    if (samples != 1) {
        static bool told;
        if (!told) told = true, LOG("[bottom-hud] the HUD pass is multisampled: not drawn\n");
        return false;
    }
    if (!ensure_images(color, ncolor, depth) || !ensure_slots()) return false;
    vk_full_barrier(cb);
    if (first) {
        cleared_frame = g_present_count;
        VkClearColorValue black = {{0, 0, 0, background_alpha()}};
        VkImageSubresourceRange rg = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, att[0].b.image, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &rg);
        vk_full_barrier(cb);
    }
    VkImageView views[9];
    for (uint32_t i = 0; i < natt; i++) views[i] = att[i].view;
    VkRenderPassBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp,
                                .framebuffer = vk_framebuffer(rp, views, natt, img_w, img_h),
                                .renderArea = {{0, 0}, {img_w, img_h}}};
    vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    if (first) draw_panels(cb);
    if (g_hudtrace_active)  // (debug, control file "hudtrace": the panel pieces of this pass and the game's scissor of each)
        for (int i = 0; i < n; i++)
            LOG("[bottom-hud] piece %d (panel %d part %d) count %u outline %.0f,%.0f..%.0f,%.0f clip %.0f,%.0f..%.0f,%.0f\n", i,
                draws[i].group / 3, draws[i].group % 3, draws[i].count, draws[i].outline[0], draws[i].outline[1],
                draws[i].outline[2], draws[i].outline[3], draws[i].clip[0], draws[i].clip[1], draws[i].clip[2], draws[i].clip[3]);
    find_map(draws, n);
    for (int i = 0; i < n; i++) replay(cb, &draws[i]);
    vkCmdEndRenderPass(cb);
    vk_full_barrier(cb);
    copy_out(cb, seq);
    return true;
}

// ---- panel side
bool bottom_hud_take(void (*copy)(const uint32_t *rgba, uint32_t w, uint32_t h, void *ctx), void *ctx, uint64_t *seq) {
    if (!slot[SLOTS - 1].px) return false;
    // (the timeline's value, or on Vulkan 1.1 devices the per-submission fences' count: exec.c, under its own lock)
    uint64_t done = vk_completed_seq(), best_seq = 0;
    int best = -1;
    for (int i = 0; i < SLOTS; i++) {
        uint64_t s = atomic_load(&slot[i].seq);
        if (s != PENDING && s <= done && s > *seq && s > best_seq) best = i, best_seq = s;
    }
    if (best < 0) return false;
    atomic_store(&reading, best);
    if (atomic_load(&slot[best].seq) != best_seq) {
        atomic_store(&reading, -1);
        return false;
    }
    vmaInvalidateAllocation(g_vk.vma, slot[best].mem, 0, VK_WHOLE_SIZE);
    copy(slot[best].px, img_w, img_h, ctx);
    atomic_store(&reading, -1);
    *seq = best_seq;
    return true;
}

// ---- touch: a finger on a panel holds what the game takes for that panel's action on a pad. MHXX's taps start a
// Hunter Art, use or change the item, switch the target camera's monster and turn the camera (D-pad panel); MHGU does
// them with the controller, so the panels press the same: ZL + X / A / B for the three Arts (as the game's own Arts list
// shows them), Y to use the item (its sides turn the bar through hle/itembar.c), the right stick's click for the target camera (its icon
// shows R), the right stick toward the finger for the D-pad panel.
static _Atomic uint64_t touch_buttons;

static _Atomic int32_t touch_rx, touch_ry;

void bottom_hud_touch(bool down, uint32_t u, uint32_t v) {
    static bool was_down;  // (a finger that stays down turns the item bar once)
    uint64_t b = 0;
    int32_t rx = 0, ry = 0;
    atomic_store(&touched, -1);
    if (down && atomic_load(&want_w) && nplaced > 0) {
        float s, ox, oy;
        screen_fit(atomic_load(&want_w), atomic_load(&want_h), &s, &ox, &oy);
        float x = ((float)u - ox) / s, y = ((float)v - oy) / s;  // (3DS bottom screen pixels)
        for (int p = 0; p < nplaced; p++) {
            const Kind *k = &kinds[placed[p].kind];
            float x0 = cell_x[placed[p].col], x1 = cell_x[placed[p].col + k->cols], y0 = cell_y[placed[p].row], y1 = cell_y[placed[p].row + k->rows];
            if (x < x0 || x >= x1 || y < y0 || y >= y1) continue;
            float fx = (x - x0) / (x1 - x0), fy = (y - y0) / (y1 - y0);
            int part = k->nsrc == 3 ? (int)((k->across ? fx : fy) * 3.0f) : 0;
            atomic_store(&touched, p * 3 + (part > 2 ? 2 : part));
            static const int art_button[3] = {BTN_X, BTN_A, BTN_B};
            switch (placed[p].kind) {
            case K_ARTS3V: b = 1ull << BTN_ZL | 1ull << art_button[fy < 1.0f / 3 ? 0 : fy < 2.0f / 3 ? 1 : 2]; break;
            case K_ARTS3H: b = 1ull << BTN_ZL | 1ull << art_button[fx < 1.0f / 3 ? 0 : fx < 2.0f / 3 ? 1 : 2]; break;
            case K_ITEMBAR:
                // (the sides turn the bar one item per touch, through the wheel's path: hle/itembar.c, with the game's
                // own checks; the middle uses the item, Y as on a pad)
                if (fx < 0.3f || fx > 0.7f) {
                    if (!was_down) itembar_scroll(fx < 0.3f ? -1 : 1);
                } else {
                    b = 1ull << BTN_Y;
                }
                break;
            case K_TARGET: b = 1ull << BTN_RSTICK; break;
            case K_MAP: b = 1ull << BTN_LSTICK; break;  // (MHXX's map panel switches the zoomed and the full map: MHGU's
                                                        // left stick click does the same)
            case K_DPAD: {
                float dx = fx - 0.5f, dy = 0.5f - fy, len = sqrtf(dx * dx + dy * dy);
                if (len > 0.05f) rx = (int32_t)(dx / len * 32767.0f), ry = (int32_t)(dy / len * 32767.0f);
                break;
            }
            default: break;  // (status, palico: shown only)
            }
        }
    }
    was_down = down;
    atomic_store(&touch_buttons, b);
    atomic_store(&touch_rx, rx);
    atomic_store(&touch_ry, ry);
}

void bottom_hud_touch_input(uint64_t *buttons, int32_t *rx, int32_t *ry) {
    *buttons |= atomic_load(&touch_buttons);
    int32_t x = atomic_load(&touch_rx), y = atomic_load(&touch_ry);
    if (x || y) *rx = x, *ry = y;
}

// ---- test mode: no panel, the picture saved every 2 s
static const uint32_t *test_wallpaper;
static bool ctx_in_quest;  // (the picture being saved is a quest picture: its panels kept)
static void save_copy(const uint32_t *rgba, uint32_t w, uint32_t h, void *ctx) {
    uint32_t *px = malloc((size_t)w * h * 4);  // (with the wallpaper where the picture is transparent, as the panel shows it)
    if (!px) return;
    for (uint32_t v = 0; v < h; v++)
        for (uint32_t u = 0; u < w; u++) {
            size_t i = (size_t)v * w + u;
            px[i] = test_wallpaper && (!ctx_in_quest || !bottom_hud_covers(u, v, w, h)) ? test_wallpaper[i] : rgba[i] | 0xff000000u;
        }
    vk_save_png((const char *)ctx, (const uint8_t *)px, w, h);
    free(px);
}

static void *test_thread(void *arg) {
    (void)arg;
    uint64_t seq = 0;
    uint32_t w = atomic_load(&want_w), h = atomic_load(&want_h);
    bool changed;
    test_wallpaper = second_display_wallpaper(w, h, &changed);
    uint32_t *empty = malloc((size_t)w * h * 4);  // (what the panel shows before a quest: saved once)
    if (empty) {
        bottom_hud_empty_picture(empty, w, h, false);
        ctx_in_quest = false;
        save_copy(empty, w, h, "shots/bottom_empty.png");
        free(empty);
    }
    for (int n = 0;; ) {
        struct timespec t = {2, 0};
        nanosleep(&t, NULL);
        char path[64];
        snprintf(path, sizeof path, "shots/bottom_%03d.png", n);
        test_wallpaper = second_display_wallpaper(w, h, &changed);  // (as the panel thread: each in turn when chosen)
        ctx_in_quest = true;
        if (bottom_hud_take(save_copy, path, &seq)) n++;
        else if ((seq && bottom_hud_stale()) || (changed && !seq)) {
            // (as the panel thread: the empty panels once out of the quest, and again when the background changes)
            uint32_t w = atomic_load(&want_w), h = atomic_load(&want_h);
            uint32_t *px = malloc((size_t)w * h * 4);
            if (px) {
                bottom_hud_empty_picture(px, w, h, false);
                ctx_in_quest = false;
                save_copy(px, w, h, path);
                free(px);
                n++;
            }
        }
    }
    return NULL;
}

bool bottom_hud_test_init(void) {
    const char *e = getenv("MHGU_BOTTOM_HUD_TEST");
    uint32_t w, h;
    if (!e || sscanf(e, "%ux%u", &w, &h) != 2 || !w || !h) return false;
    bottom_hud_set_size(w, h);
    g_bottom_hud_on = true;
    pthread_t th;
    pthread_create(&th, NULL, test_thread, NULL);
    pthread_detach(th);
    LOG("[bottom-hud] test: %ux%u without a panel, saved in shots/\n", w, h);
    return true;
}
#endif
