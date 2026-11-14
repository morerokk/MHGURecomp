/*
 * Copyright (c) 2026 Q2RTX FSR Vulkan contributors
 * SPDX-License-Identifier: MIT
 */

/* The GLSL of AMD's FidelityFX SDK 1.1.4 FSR 3.1 upscaler as data in the executable: the ten pass shaders and the
 * headers they include (the list is what the compile of every pass and permutation requests). The directories come
 * from the Makefile (FFX_GPU_DIR, FFX_PASS_DIR, relative to where the compiler runs). */
#include <string.h>

#include "ffx_fsr3_glsl.h"

#ifdef __APPLE__  // (Mach-O: its own section names, and C symbols carry a leading underscore)
#define EMBED_SECTION "__DATA,__const"
#define EMBED_SYM "_ffx_glsl_"
#else
#define EMBED_SECTION ".rodata"
#define EMBED_SYM "ffx_glsl_"
#endif
#define EMBED(n, dir, file)                                                                                   \
    __asm__(".pushsection " EMBED_SECTION "\n.globl " EMBED_SYM #n "\n" EMBED_SYM #n ":\n.incbin \"" dir "/"  \
            file "\"\n.globl " EMBED_SYM #n "_end\n" EMBED_SYM #n "_end:\n.popsection");                     \
    extern const char ffx_glsl_##n[], ffx_glsl_##n##_end[];
#define ROW(n, dir, file) {file, ffx_glsl_##n, ffx_glsl_##n##_end},

#define HEADERS(X)                                                                                            \
    X(0, FFX_GPU_DIR, "ffx_common_types.h") X(1, FFX_GPU_DIR, "ffx_core.h")                                 \
    X(2, FFX_GPU_DIR, "ffx_core_glsl.h") X(3, FFX_GPU_DIR, "ffx_core_gpu_common.h")                         \
    X(4, FFX_GPU_DIR, "ffx_core_gpu_common_half.h") X(5, FFX_GPU_DIR, "ffx_core_portability.h")             \
    X(6, FFX_GPU_DIR, "fsr1/ffx_fsr1.h") X(7, FFX_GPU_DIR, "spd/ffx_spd.h")                                 \
    X(8, FFX_GPU_DIR, "fsr3upscaler/ffx_fsr3upscaler_accumulate.h")                                         \
    X(9, FFX_GPU_DIR, "fsr3upscaler/ffx_fsr3upscaler_callbacks_glsl.h")                                     \
    X(10, FFX_GPU_DIR, "fsr3upscaler/ffx_fsr3upscaler_common.h")                                            \
    X(11, FFX_GPU_DIR, "fsr3upscaler/ffx_fsr3upscaler_debug_view.h")                                        \
    X(12, FFX_GPU_DIR, "fsr3upscaler/ffx_fsr3upscaler_luma_instability.h")                                  \
    X(13, FFX_GPU_DIR, "fsr3upscaler/ffx_fsr3upscaler_luma_pyramid.h")                                      \
    X(14, FFX_GPU_DIR, "fsr3upscaler/ffx_fsr3upscaler_prepare_inputs.h")                                    \
    X(15, FFX_GPU_DIR, "fsr3upscaler/ffx_fsr3upscaler_prepare_reactivity.h")                                \
    X(16, FFX_GPU_DIR, "fsr3upscaler/ffx_fsr3upscaler_rcas.h")                                              \
    X(17, FFX_GPU_DIR, "fsr3upscaler/ffx_fsr3upscaler_reproject.h")                                         \
    X(18, FFX_GPU_DIR, "fsr3upscaler/ffx_fsr3upscaler_resources.h")                                         \
    X(19, FFX_GPU_DIR, "fsr3upscaler/ffx_fsr3upscaler_sample.h")                                            \
    X(20, FFX_GPU_DIR, "fsr3upscaler/ffx_fsr3upscaler_shading_change.h")                                    \
    X(21, FFX_GPU_DIR, "fsr3upscaler/ffx_fsr3upscaler_shading_change_pyramid.h")                            \
    X(22, FFX_GPU_DIR, "fsr3upscaler/ffx_fsr3upscaler_upsample.h")

#define PASSES(X)                                                                                             \
    X(30, FFX_PASS_DIR, "ffx_fsr3upscaler_accumulate_pass.glsl")                                            \
    X(31, FFX_PASS_DIR, "ffx_fsr3upscaler_autogen_reactive_pass.glsl")                                      \
    X(32, FFX_PASS_DIR, "ffx_fsr3upscaler_debug_view_pass.glsl")                                            \
    X(33, FFX_PASS_DIR, "ffx_fsr3upscaler_luma_instability_pass.glsl")                                      \
    X(34, FFX_PASS_DIR, "ffx_fsr3upscaler_luma_pyramid_pass.glsl")                                          \
    X(35, FFX_PASS_DIR, "ffx_fsr3upscaler_prepare_inputs_pass.glsl")                                        \
    X(36, FFX_PASS_DIR, "ffx_fsr3upscaler_prepare_reactivity_pass.glsl")                                    \
    X(37, FFX_PASS_DIR, "ffx_fsr3upscaler_rcas_pass.glsl")                                                  \
    X(38, FFX_PASS_DIR, "ffx_fsr3upscaler_shading_change_pass.glsl")                                        \
    X(39, FFX_PASS_DIR, "ffx_fsr3upscaler_shading_change_pyramid_pass.glsl")

HEADERS(EMBED)
PASSES(EMBED)

static const struct { const char *name, *begin, *end; } files[] = {HEADERS(ROW) PASSES(ROW)};

const char *ffxFsr3GlslFile(const char *name, size_t *length) {
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    for (int pass = 0; pass < 2; pass++)  // (the exact name, then the file name: headers include their neighbours by it)
        for (size_t i = 0; i < sizeof files / sizeof *files; i++) {
            const char *f = files[i].name, *fb = strrchr(f, '/');
            if (strcmp(pass ? base : name, pass ? (fb ? fb + 1 : f) : f)) continue;
            *length = (size_t)(files[i].end - files[i].begin);
            return files[i].begin;
        }
    return NULL;
}

uint64_t ffxFsr3GlslHash(void) {
    static uint64_t cached;
    if (cached) return cached;
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < sizeof files / sizeof *files; i++) {
        for (const char *p = files[i].name; *p; p++) h = (h ^ (uint8_t)*p) * 0x100000001B3ull;
        for (const char *p = files[i].begin; p < files[i].end; p++) h = (h ^ (uint8_t)*p) * 0x100000001B3ull;
    }
    return cached = h | 1;  // (all threads compute the same value)
}
