// Forge's ImGui overlay (runtime/overlay.cpp): its menu window with the plugins' settings, drawn over the game - and
// the runtime's own status line (overlay_status), with or without Forge. C and C++.
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
bool overlay_enabled(void);
bool overlay_menu_open(void);
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

// Any thread: a line of text in the window's lower left corner, over the game (full screen too), such as how far the
// shader warm-up is; NULL or "": none. It starts ImGui without Forge when no plugin did.
void overlay_status(const char *text);
// The game's thread, as it presents a frame (runtime/nvn/vulkan/device.c, before taking the Vulkan lock): the overlay's
// frame when no Forge plugin hooks the game's frame update (forge.c calls overlay_frame then).
void overlay_tick(void);

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

#ifdef __cplusplus
}
#endif
