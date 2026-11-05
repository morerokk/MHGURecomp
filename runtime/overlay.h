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
void overlay_init(const char *font_path, float font_size, int menu_key);
// Host main thread, before the renderer: ImGui with its own font, for the loading screen until overlay_init.
void overlay_init_early(void);
bool overlay_enabled(void);
bool overlay_menu_open(void);
bool overlay_wants_text(void);  // a text field of the open menu (a Forge plugin's) has the focus
bool overlay_settings_dirty(void);
void overlay_request_close(void); // Close / Alt+F4 routes to controller confirmation inside settings.
void overlay_face_positions(const int positions[4]);
int overlay_face_position(int semantic); // A, B, X, Y -> physical south/east/west/north.
void overlay_toggle_settings(void); // host main thread: F1 or Start + Back
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

#ifdef __cplusplus
}
#endif
