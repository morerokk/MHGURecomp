// The vendored SGSR 2 GLSL (third_party/sgsr2/glsl) as data in the executable.
#pragma once
#include <stddef.h>

const char *sgsr2_glsl_convert(size_t *len);
const char *sgsr2_glsl_upscale(size_t *len);
