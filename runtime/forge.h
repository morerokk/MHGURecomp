// Between runtime/forge.c (Forge plugins), runtime/forge_imgui.c (their ImGui calls) and runtime/overlay.cpp
// (Forge's ImGui overlay). C and C++.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
struct Cpu;

// forge.c
// printf with the guest's arguments: variadic ones from core register reg (4: none left) on, then the stack at stk
void forge_format(char *out, size_t n, const char *fmt, struct Cpu *c, int reg, uint32_t stk);
// the plugins' ImGui entry points: FORGE_IMGUI_INIT forge_onImGuiInit(ctx, alloc, free), FORGE_IMGUI_RENDER
// forge_onImGuiRender() (in Forge's window), FORGE_IMGUI_FREE_RENDER forge_onImGuiFreeRender() (every frame)
enum { FORGE_IMGUI_INIT, FORGE_IMGUI_RENDER, FORGE_IMGUI_FREE_RENDER };
void forge_plugins_imgui(struct Cpu *c, int which, uint32_t a0, uint32_t a1, uint32_t a2);
bool forge_plugins_have(int which);                  // a started plugin has that entry point
int forge_plugin_titles(const char **out, int max);  // the started plugins' names (their own, else the file's)
void forge_test_hook(uint32_t target);  // (debug) a plugin-style hook on target (MHGU_FORGE_TESTHOOK, "testhook")
// runtime/nvn/objects.c, for Forge 2.0.0's rendering callbacks (guest addresses): nvnDeviceInitialize's device,
// nvnWindowBuilderSetTextures' array of n NVNtexture *, and nvnQueuePresentTexture before it presents (plugins'
// forge_onNvnInit / forge_onNvnReady the first time, forge_onNvnRender every time; r0-r3 of c are used)
void forge_nvn_device(uint32_t device);
void forge_nvn_window_textures(uint32_t textures, int n);
void forge_nvn_present(struct Cpu *c, uint32_t queue, uint32_t window, int index);
// runtime/hle/abort.c: the Forge file call this thread is in, with its plugin and file (NULL: none)
const char *forge_abort_context(void);
int forge_stack_plugins(uint32_t sp, char *line, size_t size);  // " file.nro+off ...": plugins' calls on a guest stack
int forge_log_open_files(const char *tag);  // plugins' files still open (forge_fs_openFile, not closed): logged, the count

// forge_imgui.c, for the overlay (on the game's thread, cpu: its Cpu)
void forge_imgui_init(void *cpu);  // (after the ImGui context exists: plugins get it)
void forge_imgui_run(void *cpu, int which);

#ifdef __cplusplus
}
#endif
