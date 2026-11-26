/*
 * The GLSL of Qualcomm's SGSR 2 (the two 2-pass-fs shaders) as data in the
 * executable. SGSR2_GLSL_DIR comes from the Makefile; the files are kept
 * verbatim next to this source and compiled with shaderc when first used.
 */
#include <string.h>

#include "sgsr2_glsl.h"

#define EMBED(n, file)                                                                                             \
    __asm__(".pushsection .rodata\n.globl sgsr2_glsl_" #n "\nsgsr2_glsl_" #n ":\n.incbin \"" SGSR2_GLSL_DIR "/" file \
            "\"\n.globl sgsr2_glsl_" #n "_end\nsgsr2_glsl_" #n "_end:\n.popsection");                              \
    extern const char sgsr2_glsl_##n[], sgsr2_glsl_##n##_end[];

EMBED(convert, "sgsr2_convert.fs")
EMBED(upscale, "sgsr2_upscale.fs")

const char *sgsr2_glsl_convert(size_t *len) { return *len = (size_t)(sgsr2_glsl_convert_end - sgsr2_glsl_convert), sgsr2_glsl_convert; }
const char *sgsr2_glsl_upscale(size_t *len) { return *len = (size_t)(sgsr2_glsl_upscale_end - sgsr2_glsl_upscale), sgsr2_glsl_upscale; }
