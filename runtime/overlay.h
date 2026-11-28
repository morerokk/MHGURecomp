// Forge's ImGui overlay (runtime/overlay.cpp): its menu window with the plugins' settings, drawn over the game.
// C and C++.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

// From runtime/forge.c once plugins are loaded: font (a .ttf; NULL: ImGui's own), its size in pixels at 720 lines
// (scaled with the window), the key that opens the menu (SDL scancode = HID usage, as Forge's config has it).
// cjk_font: a mod's .ttf merged into the same atlas for its Chinese glyphs, ahead of the release's mhgurecomp-cjk.otf
// beside the program or in the APK (NULL: the release's alone; neither: the system's once the menu is translated).
void overlay_init(const char *font_path, const char *cjk_font, float font_size, int menu_key);
// Host main thread, before the renderer: ImGui with its own font, for the loading screen until overlay_init.
void overlay_init_early(void);
bool overlay_enabled(void);
bool overlay_menu_open(void);
bool overlay_wants_text(void);  // a text field of the open menu (a Forge plugin's) has the focus
bool overlay_settings_dirty(void);
void overlay_request_close(void); // Close / Alt+F4 routes to controller confirmation inside settings.
void overlay_face_positions(const int positions[4]);
int overlay_face_position(int semantic); // A, B, X, Y -> physical south/east/west/north.
void overlay_toggle_settings(void); // host main thread: F1 (one level back while open; over Forge's menu: closes that)
void overlay_menu_chord(void); // host main thread: Start + Back. As F1, but Forge's menu open by itself closes
// Any thread: a notice shown over the game for a few seconds (title on its own line; the latest replaces any other).
void overlay_notice(const char *title, const char *text);

// Host main thread: an event for ImGui while the menu is open (true: taken, not for the game) - and the menu key.
bool overlay_event(const SDL_Event *e);
// Host main thread, while the menu is open: the controller, which moves through the menu instead of the game.
typedef struct { bool a, b, x, y, up, down, left, right, l, r, plus, minus; float lx, ly; } OverlayPad;
void overlay_pad(const OverlayPad *p);

// The game's thread, once a frame (after the plugins' forge_onUpdate): the overlay's frame, plugins' UI included.
void overlay_frame(void *cpu);

// Render thread, inside the window image's render pass (runtime/nvn/vulkan/device.c, under the Vulkan lock).
typedef struct {
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    uint32_t qfam;
    VkQueue queue;
    VkPipelineCache cache;
    VkRenderPass pass;
    uint32_t width, height;
} OverlayTarget;
void overlay_draw(VkCommandBuffer cb, const OverlayTarget *t);
// Before the game's first frame (device.c's loading screen thread, under the Vulkan lock, inside the window image's
// render pass): the loading indicator alone, on the black window.
void overlay_loading(VkCommandBuffer cb, const OverlayTarget *t);
// (render thread, before the window image's frame begins) false: overlay_draw would draw nothing this frame - no
// overlay window, notice or loading indicator (HDR then draws the game straight into the window image: device.c)
bool overlay_busy(void);

// Forge 2.0.0's forge_graphics_registerTexture / unregisterTexture (any thread): a plugin's NVNtexture (guest address)
// with its NVNtextureView's 0x28 bytes (NULL: none) for ImGui::Image; the ImTextureID the plugin draws it with, 0 when
// the table is full. remove: false for an id that is not registered.
uint64_t overlay_user_texture_add(uint32_t tex, const void *view);
bool overlay_user_texture_remove(uint64_t id);
// device.c, each present, outside the render pass and before overlay_draw: view_of(texture, view or NULL) is the
// registered texture's sampled Vulkan view now (VK_NULL_HANDLE: none)
void overlay_user_textures_prepare(VkImageView (*view_of)(uint32_t tex, const void *view));

#ifdef __cplusplus
}
#endif
