// GLSL -> SPIR-V (shaderc, Vulkan 1.2 environment), descriptor layouts from the SPIR-V itself, built-in shaders.
// Ported from LAS Recomp's runtime/nvn/vulkan/shaders.c (without its post-processing passes).
#include <pthread.h>
#include <shaderc/shaderc.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "shaders.h"

void vk_shaders_init(void) {  // (compilers are per thread: check shaderc works at all)
    shaderc_compiler_t c = shaderc_compiler_initialize();
    if (!c) FATAL("shaderc_compiler_initialize failed\n");
    shaderc_compiler_release(c);
}

static shaderc_shader_kind kind_of(int stage) {
    return stage == STAGE_VERTEX ? shaderc_vertex_shader : stage == STAGE_FRAGMENT ? shaderc_fragment_shader : shaderc_compute_shader;
}

// SPIR-V disk cache: cache/spirv/<hash of stage + source>.spv, so a known shader costs a file read instead of a
// GLSL compile (the driver's pipeline cache only covers the step after this one).
static uint64_t src_hash(const char *src, size_t len, int stage) {
    uint64_t h = (0xcbf29ce484222325ull ^ (uint64_t)stage) + 3;  // + version of the post-processing (no_contraction)
    for (size_t i = 0; i < len; i++) { h ^= (uint8_t)src[i]; h *= 0x100000001B3ull; }
    return h;
}

static uint32_t *cache_load(uint64_t h, size_t *nwords) {
    char path[64];
    snprintf(path, sizeof path, "cache/spirv/%016llx.spv", (unsigned long long)h);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t *d = n > 20 && n % 4 == 0 ? malloc((size_t)n) : NULL;
    if (d && fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); d = NULL; }
    fclose(f);
    if (d && d[0] != 0x07230203) { free(d); d = NULL; }
    if (d) *nwords = (size_t)n / 4;
    return d;
}

static void cache_store(uint64_t h, const uint32_t *spv, size_t nwords) {
    static int made;
    if (!made) { made = 1; mkdir("cache", 0755); mkdir("cache/spirv", 0755); }
    char path[64], tmp[80];
    snprintf(path, sizeof path, "cache/spirv/%016llx.spv", (unsigned long long)h);
    snprintf(tmp, sizeof tmp, "%s.%llx", path, (unsigned long long)(uintptr_t)&made ^ (unsigned long long)(uintptr_t)spv);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    bool ok = fwrite(spv, 4, nwords, f) == nwords;
    fclose(f);
    if (!ok || rename(tmp, path) != 0) remove(tmp);  // (another thread may have stored it first)
}

// Vertex shaders: every float add/sub/mul/div/fma gets NoContraction, so no compiler fuses or splits them. Depth
// pre-pass and colour pass are different shaders computing the same position, and the colour pass tests EQUAL:
// any difference in fusing leaves holes (foliage flickered black). GLSL's `precise gl_Position` only covers what
// glslang traces back from the position, which the translated register code defeats.
static bool float_arith(const uint32_t *w, uint32_t wc, uint32_t op) {
    switch (op) {
    case 127: case 129: case 131: case 133: case 136:  // FNegate FAdd FSub FMul FDiv
    case 142: case 143: case 144: case 145: case 146: case 148:  // vector/matrix products, Dot
        return true;
    case 12: return wc >= 5 && w[4] == 50;  // ExtInst GLSL.std.450 Fma
    default: return false;
    }
}

static uint32_t *no_contraction(uint32_t *spv, size_t *nwords) {
    enum { OP_DECORATE = 71, DEC_NO_CONTRACTION = 42 };
    size_t n = *nwords, insert = 0, count = 0;
    uint32_t bound = spv[3];
    uint8_t *done = calloc(bound, 1);  // already decorated (glslang does it for what it traces from precise)
    for (size_t i = 5; i < n;) {
        uint32_t wc = spv[i] >> 16, op = spv[i] & 0xFFFF;
        if (!wc) break;
        if (!insert && op >= 19 && op <= 39) insert = i;  // first type declaration: end of the annotations
        if (op == OP_DECORATE && wc >= 3 && spv[i + 2] == DEC_NO_CONTRACTION && spv[i + 1] < bound) done[spv[i + 1]] = 1;
        i += wc;
    }
    for (size_t i = 5; i < n;) {
        uint32_t wc = spv[i] >> 16, op = spv[i] & 0xFFFF;
        if (!wc) break;
        if (float_arith(spv + i, wc, op) && spv[i + 2] < bound && !done[spv[i + 2]]) count++;
        i += wc;
    }
    if (!insert || !count) { free(done); return spv; }
    uint32_t *out = malloc((n + count * 3) * 4);
    memcpy(out, spv, insert * 4);
    size_t o = insert;
    for (size_t i = 5; i < n;) {
        uint32_t wc = spv[i] >> 16, op = spv[i] & 0xFFFF;
        if (!wc) break;
        if (float_arith(spv + i, wc, op) && spv[i + 2] < bound && !done[spv[i + 2]]) {
            done[spv[i + 2]] = 1;
            out[o++] = 3u << 16 | OP_DECORATE;
            out[o++] = spv[i + 2];  // result id
            out[o++] = DEC_NO_CONTRACTION;
        }
        i += wc;
    }
    memcpy(out + o, spv + insert, (n - insert) * 4);
    *nwords = o + (n - insert);
    free(done);
    free(spv);
    return out;
}

// Include resolver for the shaders that have headers (fsr3_glsl.c): the file's text or NULL, *len set.
static shaderc_include_result *inc_resolve(void *ud, const char *requested, int type, const char *requesting, size_t depth) {
    (void)type, (void)requesting, (void)depth;
    VkGlslInclude fn = ud;
    shaderc_include_result *r = calloc(1, sizeof *r);
    size_t len = 0;
    const char *text = fn(requested, &len);
    if (!text) { r->content = "file not found"; r->content_length = 14; r->source_name = ""; return r; }  // (empty name: an error)
    r->source_name = requested, r->source_name_length = strlen(requested);
    r->content = text, r->content_length = len;
    return r;
}
static void inc_release(void *ud, shaderc_include_result *r) { (void)ud; free(r); }

// SPIR-V words of src, or NULL (error logged with the source). *nwords set on success. Thread-safe: one shaderc
// compiler per thread (a shared one behind a lock serialised all the pipeline workers).
// No spirv-opt by default: it rewrote the depth pre-pass and colour-pass vertex shaders differently enough that
// their depths stopped matching (EQUAL test: black holes in foliage), and it was most of the compile time; the
// driver optimises anyway. MHGU_VK_OPT=1 turns it back on (experiments).
// vk_glsl_to_spirv_ex adds -D macros and #include (inc); salt stands for what the includes contain, as the disk cache key.
// opt (there only) is 0 for none or 1 for spirv-opt at the size level (which strips the OpNames), whatever MHGU_VK_OPT says.
// hlsl: the source is HLSL, compiled from that entry point, its registers mapped to bindings (t# and u# from 0 and
// hlsl_uav_base on).
static uint32_t *glsl_compile(const char *src, size_t srclen, int stage, const char *name, const char *const *defs, int ndefs,
                              VkGlslInclude inc, uint64_t salt, int opt, const char *hlsl, int hlsl_uav_base, size_t *nwords) {
    uint64_t h = src_hash(src, srclen, stage);
    if (ndefs || inc || hlsl) {  // (plain callers keep their old keys)
        h ^= salt * 0x9E3779B97F4A7C15ull + 1 + (uint64_t)opt;
        for (int i = 0; i < ndefs; i++)
            for (const char *p = defs[i]; ; p++) { h ^= (uint8_t)*p; h *= 0x100000001B3ull; if (!*p) break; }
        for (const char *p = hlsl; p && *p; p++) { h ^= (uint8_t)*p; h *= 0x100000001B3ull; }
        if (hlsl) h = (h ^ (uint64_t)hlsl_uav_base ^ 0x484C534Cull) * 0x100000001B3ull;
    }
    static int nocache = -1;
    if (nocache < 0) nocache = getenv("MHGU_VK_NO_SPIRV_CACHE") != NULL;
    uint32_t *out = nocache ? NULL : cache_load(h, nwords);
    if (out) return out;
    static __thread shaderc_compiler_t comp;
    if (!comp) comp = shaderc_compiler_initialize();
    shaderc_compile_options_t o = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(o, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
    shaderc_compile_options_set_optimization_level(o, ndefs || inc ? opt ? shaderc_optimization_level_size : shaderc_optimization_level_zero
                                                   : getenv("MHGU_VK_OPT") ? shaderc_optimization_level_performance
                                                                           : shaderc_optimization_level_zero);
    for (int i = 0; i < ndefs; i++) {
        const char *eq = strchr(defs[i], '=');
        size_t nl = eq ? (size_t)(eq - defs[i]) : strlen(defs[i]);
        shaderc_compile_options_add_macro_definition(o, defs[i], nl, eq ? eq + 1 : "1", eq ? strlen(eq + 1) : 1);
    }
    if (inc) shaderc_compile_options_set_include_callbacks(o, inc_resolve, inc_release, inc);
    if (hlsl) {
        shaderc_compile_options_set_source_language(o, shaderc_source_language_hlsl);
        shaderc_compile_options_set_hlsl_io_mapping(o, true);
        shaderc_compile_options_set_binding_base_for_stage(o, kind_of(stage), shaderc_uniform_kind_unordered_access_view,
                                                           (uint32_t)hlsl_uav_base);
    }
    shaderc_compilation_result_t r = shaderc_compile_into_spv(comp, src, srclen, kind_of(stage), name, hlsl ? hlsl : "main", o);
    shaderc_compile_options_release(o);
    if (shaderc_result_get_compilation_status(r) != shaderc_compilation_status_success) {
        static int nlog;
        if (nlog++ < 20)
            LOG("[shader] %s: %s compile failed:\n%s\n%s\n", name, hlsl ? "HLSL" : "GLSL", shaderc_result_get_error_message(r),
                ndefs || hlsl ? "" : src);
    } else {
        size_t n = shaderc_result_get_length(r);
        out = malloc(n);
        memcpy(out, shaderc_result_get_bytes(r), n);
        *nwords = n / 4;
        if (stage == STAGE_VERTEX) out = no_contraction(out, nwords);
        if (!nocache) cache_store(h, out, *nwords);
    }
    shaderc_result_release(r);
    return out;
}

uint32_t *vk_glsl_to_spirv(const char *src, int stage, const char *name, size_t *nwords) {
    return glsl_compile(src, strlen(src), stage, name, NULL, 0, NULL, 0, 0, NULL, 0, nwords);
}

uint32_t *vk_glsl_to_spirv_ex(const char *src, size_t srclen, const char *name, const char *const *defs, int ndefs,
                              VkGlslInclude inc, uint64_t salt, int opt, size_t *nwords) {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint32_t *r = glsl_compile(src, srclen, STAGE_COMPUTE, name, defs, ndefs, inc, salt, opt, NULL, 0, nwords);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    LOG("[shader] %s: %s in %.1f ms\n", name, r ? "ready" : "FAILED", (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) * 1e-6);
    return r;
}

uint32_t *vk_hlsl_to_spirv(const char *src, size_t srclen, const char *entry, const char *name, const char *const *defs,
                           int ndefs, int uav_base, size_t *nwords) {
    return glsl_compile(src, srclen, STAGE_COMPUTE, name, defs, ndefs, NULL, 0, 0, entry, uav_base, nwords);
}

VkShaderModule vk_module(const uint32_t *spv, size_t nwords) {
    VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = nwords * 4, .pCode = spv};
    VkShaderModule m;
    return vkCreateShaderModule(g_vk.dev, &ci, NULL, &m) == VK_SUCCESS ? m : VK_NULL_HANDLE;
}

VkShaderModule vk_compile_glsl(const char *src, int stage, const char *name) {
    size_t n;
    uint32_t *spv = vk_glsl_to_spirv(src, stage, name, &n);
    if (!spv) return VK_NULL_HANDLE;
    VkShaderModule m = vk_module(spv, n);
    free(spv);
    return m;
}

// ================================================================== reflection
// Just enough SPIR-V parsing to list the descriptor bindings a module declares (set, binding, type).
enum { OP_TYPE_IMAGE = 25, OP_TYPE_SAMPLER = 26, OP_TYPE_SAMPLED_IMAGE = 27, OP_TYPE_ARRAY = 28, OP_TYPE_RUNTIME_ARRAY = 29,
       OP_TYPE_STRUCT = 30, OP_TYPE_POINTER = 32, OP_VARIABLE = 59, OP_DECORATE = 71 };
enum { DEC_BLOCK = 2, DEC_BUFFER_BLOCK = 3, DEC_BINDING = 33, DEC_SET = 34 };
enum { SC_UNIFORM_CONSTANT = 0, SC_UNIFORM = 2, SC_STORAGE_BUFFER = 12 };

int vk_reflect(const uint32_t *spv, size_t n, VkBindingInfo *out, int max) {
    if (n < 5 || spv[0] != 0x07230203) return 0;
    uint32_t bound = spv[3];
    int32_t *binding = malloc(bound * sizeof *binding), *set = malloc(bound * sizeof *set);
    uint8_t *flags = calloc(bound, 1);    // 1 Block, 2 BufferBlock
    uint32_t *op = calloc(bound, sizeof *op), *ref = calloc(bound, sizeof *ref), *sc = calloc(bound, sizeof *sc);
    for (uint32_t i = 0; i < bound; i++) binding[i] = set[i] = -1;
    for (size_t i = 5; i < n;) {
        uint32_t wc = spv[i] >> 16, o = spv[i] & 0xFFFF;
        if (!wc) break;
        const uint32_t *w = spv + i;
        if (o == OP_DECORATE && wc >= 3 && w[1] < bound) {
            if (w[2] == DEC_BINDING && wc >= 4) binding[w[1]] = (int32_t)w[3];
            else if (w[2] == DEC_SET && wc >= 4) set[w[1]] = (int32_t)w[3];
            else if (w[2] == DEC_BLOCK) flags[w[1]] |= 1;
            else if (w[2] == DEC_BUFFER_BLOCK) flags[w[1]] |= 2;
        } else if ((o == OP_TYPE_IMAGE || o == OP_TYPE_SAMPLER || o == OP_TYPE_SAMPLED_IMAGE || o == OP_TYPE_STRUCT) && wc >= 2 && w[1] < bound) {
            op[w[1]] = o;
        } else if ((o == OP_TYPE_ARRAY || o == OP_TYPE_RUNTIME_ARRAY) && wc >= 3 && w[1] < bound) {
            op[w[1]] = o;
            ref[w[1]] = w[2];
        } else if (o == OP_TYPE_POINTER && wc >= 4 && w[1] < bound) {
            op[w[1]] = o;
            sc[w[1]] = w[2];
            ref[w[1]] = w[3];
        } else if (o == OP_VARIABLE && wc >= 4 && w[2] < bound && w[1] < bound) {
            op[w[2]] = o;
            ref[w[2]] = w[1];  // pointer type
            sc[w[2]] = w[3];
        }
        i += wc;
    }
    int cnt = 0;
    for (uint32_t id = 0; id < bound && cnt < max; id++) {
        if (op[id] != OP_VARIABLE || binding[id] < 0) continue;
        uint32_t t = ref[ref[id]];  // pointee
        while (t < bound && (op[t] == OP_TYPE_ARRAY || op[t] == OP_TYPE_RUNTIME_ARRAY)) t = ref[t];
        VkDescriptorType dt;
        if (t >= bound) continue;
        if (op[t] == OP_TYPE_SAMPLED_IMAGE) dt = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        else if (op[t] == OP_TYPE_IMAGE) dt = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        else if (op[t] == OP_TYPE_SAMPLER) dt = VK_DESCRIPTOR_TYPE_SAMPLER;
        else if (op[t] == OP_TYPE_STRUCT && (sc[id] == SC_STORAGE_BUFFER || (flags[t] & 2))) dt = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        else if (op[t] == OP_TYPE_STRUCT && sc[id] == SC_UNIFORM) dt = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        else continue;
        out[cnt++] = (VkBindingInfo){(uint32_t)(set[id] < 0 ? 0 : set[id]), (uint32_t)binding[id], dt};
    }
    free(binding); free(set); free(flags); free(op); free(ref); free(sc);
    return cnt;
}

VkDescriptorSetLayout vk_set_layout(const VkBindingInfo *b, int n, uint32_t set, VkShaderStageFlags stages) {
    VkDescriptorSetLayoutBinding lb[96];
    uint32_t k = 0;
    for (int i = 0; i < n && k < 96; i++)
        if (b[i].set == set) lb[k++] = (VkDescriptorSetLayoutBinding){b[i].binding, b[i].type, 1, stages, NULL};
    VkDescriptorSetLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = k, .pBindings = lb};
    VkDescriptorSetLayout l;
    VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &ci, NULL, &l));
    return l;
}

// ================================================================== built-in shaders
static const char *builtin_src(const char *name, int *stage) {
    if (!strcmp(name, "present.vert")) {
        *stage = STAGE_VERTEX;
        // pv[0].x: flip (lower-left origin), pv[1]: uv crop (origin, size). Fullscreen triangle; Vulkan NDC y points down.
        return "#version 450\n"
               "layout(push_constant) uniform P { vec4 a; vec4 crop; } pc;\n"
               "layout(location = 0) out vec2 uv;\n"
               "void main() {\n"
               "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
               "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
               "  vec2 u = vec2(p.x, pc.a.x > 0.5 ? 1.0 - p.y : p.y);\n"
               "  uv = pc.crop.xy + u * pc.crop.zw;\n"
               "}\n";
    }
    if (!strcmp(name, "present.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(location = 0) in vec2 uv;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() { o = vec4(texture(t, uv).rgb, 1.0); }\n";
    }
    if (!strcmp(name, "clear.vert")) {
        *stage = STAGE_VERTEX;
        return "#version 450\n"
               "layout(push_constant) uniform P { vec4 c; vec4 d; } pc;\n"
               "void main() {\n"
               "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
               "  gl_Position = vec4(p * 2.0 - 1.0, pc.d.x, 1.0);\n"
               "}\n";
    }
    // clear.frag.<f|i|u><index>: writes the push-constant colour to colour target <index>
    if (!strncmp(name, "clear.frag.", 11)) {
        *stage = STAGE_FRAGMENT;
        static __thread char buf[512];
        char k = name[11];
        int idx = atoi(name + 12);
        const char *ty = k == 'i' ? "ivec4" : k == 'u' ? "uvec4" : "vec4";
        const char *cv = k == 'i' ? "floatBitsToInt(pc.c)" : k == 'u' ? "floatBitsToUint(pc.c)" : "pc.c";
        snprintf(buf, sizeof buf,
                 "#version 450\n"
                 "layout(push_constant) uniform P { vec4 c; vec4 d; } pc;\n"
                 "layout(location = %d) out %s o;\n"
                 "void main() { o = %s; }\n", idx, ty, cv);
        return buf;
    }
    if (!strcmp(name, "clear.frag.none")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\nvoid main() {}\n";
    }
    // MSAA depth resolve (exec.c vk_ms_resolve): a fullscreen triangle writing sample 0's depth (and stencil, ".s":
    // VK_EXT_shader_stencil_export), as the single-sample image would have it
    if (!strcmp(name, "resolve.vert")) {
        *stage = STAGE_VERTEX;
        return "#version 450\n"
               "void main() {\n"
               "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
               "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
               "}\n";
    }
    if (!strcmp(name, "fxaa.frag")) {
        *stage = STAGE_FRAGMENT;
        // FXAA 3.11 Quality (Timothy Lottes' algorithm) over the game's FXAA input, which holds its luma in alpha (as the
        // game's own pass reads it). pc.a: sub-pixel blur, edge threshold, edge threshold min, the number of search
        // steps; pc.p: the steps (in pixels) of the quality preset (exec.c fxaa_presets).
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 p[3]; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "float luma(vec2 uv) { return textureLod(t, uv, 0.0).a; }\n"
               "#define AT(x, y) textureLodOffset(t, pos, 0.0, ivec2(x, y)).a\n"
               "void main() {\n"
               "  vec2 rcp = 1.0 / vec2(textureSize(t, 0)), pos = gl_FragCoord.xy * rcp;\n"
               "  vec4 rgbyM = textureLod(t, pos, 0.0);\n"
               "  float lumaM = rgbyM.a, lumaN = AT(0, -1), lumaS = AT(0, 1), lumaW = AT(-1, 0), lumaE = AT(1, 0);\n"
               "  float rangeMax = max(max(max(lumaN, lumaW), max(lumaE, lumaS)), lumaM);\n"
               "  float range = rangeMax - min(min(min(lumaN, lumaW), min(lumaE, lumaS)), lumaM);\n"
               "  if (range < max(pc.a.z, rangeMax * pc.a.y)) { o = rgbyM; return; }\n"
               "  float lumaNW = AT(-1, -1), lumaNE = AT(1, -1), lumaSW = AT(-1, 1), lumaSE = AT(1, 1);\n"
               "  float lumaNS = lumaN + lumaS, lumaWE = lumaW + lumaE;\n"
               "  float edgeHorz = abs(lumaNW + lumaSW - 2.0 * lumaW) + 2.0 * abs(lumaNS - 2.0 * lumaM) + abs(lumaNE + lumaSE - 2.0 * lumaE);\n"
               "  float edgeVert = abs(lumaSW + lumaSE - 2.0 * lumaS) + 2.0 * abs(lumaWE - 2.0 * lumaM) + abs(lumaNW + lumaNE - 2.0 * lumaN);\n"
               "  bool horzSpan = edgeHorz >= edgeVert;\n"
               "  float lengthSign = horzSpan ? rcp.y : rcp.x;\n"
               "  if (!horzSpan) { lumaN = lumaW; lumaS = lumaE; }\n"
               "  float subpixC = clamp(abs(((lumaNS + lumaWE) * 2.0 + lumaNW + lumaSW + lumaNE + lumaSE) / 12.0 - lumaM) / range, 0.0, 1.0);\n"
               "  float gradientN = lumaN - lumaM, gradientS = lumaS - lumaM;\n"
               "  bool pairN = abs(gradientN) >= abs(gradientS);\n"
               "  float gradientScaled = max(abs(gradientN), abs(gradientS)) * 0.25;\n"
               "  if (pairN) lengthSign = -lengthSign;\n"
               "  float lumaHalf = ((pairN ? lumaN : lumaS) + lumaM) * 0.5;\n"
               "  vec2 posB = pos + (horzSpan ? vec2(0.0, lengthSign) : vec2(lengthSign, 0.0)) * 0.5;\n"
               "  vec2 off = horzSpan ? vec2(rcp.x, 0.0) : vec2(0.0, rcp.y);\n"
               "  vec2 posN = posB - off * pc.p[0].x, posP = posB + off * pc.p[0].x;\n"
               "  float endN = luma(posN) - lumaHalf, endP = luma(posP) - lumaHalf;\n"
               "  bool doneN = abs(endN) >= gradientScaled, doneP = abs(endP) >= gradientScaled;\n"
               "  int steps = int(pc.a.w);\n"
               "  for (int i = 1; i < steps && !(doneN && doneP); i++) {  // the last step is taken without a look\n"
               "    float s = pc.p[i >> 2][i & 3];\n"
               "    if (!doneN) posN -= off * s;\n"
               "    if (!doneP) posP += off * s;\n"
               "    if (i == steps - 1) break;\n"
               "    if (!doneN) { endN = luma(posN) - lumaHalf; doneN = abs(endN) >= gradientScaled; }\n"
               "    if (!doneP) { endP = luma(posP) - lumaHalf; doneP = abs(endP) >= gradientScaled; }\n"
               "  }\n"
               "  float dstN = horzSpan ? pos.x - posN.x : pos.y - posN.y, dstP = horzSpan ? posP.x - pos.x : posP.y - pos.y;\n"
               "  bool goodSpan = ((dstN < dstP ? endN : endP) < 0.0) != (lumaM < lumaHalf);\n"
               "  float pixelOffset = goodSpan ? 0.5 - min(dstN, dstP) / (dstN + dstP) : 0.0;\n"
               "  float subpixF = (3.0 - 2.0 * subpixC) * subpixC * subpixC;\n"
               "  float offset = max(pixelOffset, subpixF * subpixF * pc.a.x) * lengthSign;\n"
               "  o = vec4(textureLod(t, pos + (horzSpan ? vec2(0.0, offset) : vec2(offset, 0.0)), 0.0).rgb, lumaM);\n"
               "}\n";
    }
    if (!strcmp(name, "resolve.frag.depth") || !strcmp(name, "resolve.frag.depth.s")) {
        *stage = STAGE_FRAGMENT;
        if (name[18] == '.')
            return "#version 450\n"
                   "#extension GL_ARB_shader_stencil_export : require\n"
                   "layout(set = 0, binding = 0) uniform sampler2DMS d;\n"
                   "layout(set = 0, binding = 1) uniform usampler2DMS s;\n"
                   "void main() {\n"
                   "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
                   "  gl_FragDepth = texelFetch(d, p, 0).r;\n"
                   "  gl_FragStencilRefARB = int(texelFetch(s, p, 0).r);\n"
                   "}\n";
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2DMS d;\n"
               "void main() { gl_FragDepth = texelFetch(d, ivec2(gl_FragCoord.xy), 0).r; }\n";
    }
    return NULL;
}

VkShaderModule vk_builtin_shader(const char *name) {
    static struct { char name[32]; VkShaderModule m; } cache[64];
    static int n;
    for (int i = 0; i < n; i++) if (!strcmp(cache[i].name, name)) return cache[i].m;
    int stage = 0;
    const char *src = builtin_src(name, &stage);
    if (!src) FATAL("no built-in shader %s\n", name);
    VkShaderModule m = vk_compile_glsl(src, stage, name);
    if (!m) FATAL("built-in shader %s failed to compile\n", name);
    if (n < 64) { snprintf(cache[n].name, sizeof cache[n].name, "%s", name); cache[n++].m = m; }
    return m;
}
