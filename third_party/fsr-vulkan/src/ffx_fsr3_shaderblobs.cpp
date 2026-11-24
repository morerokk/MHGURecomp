/*
 * Copyright (c) 2026 Q2RTX FSR Vulkan contributors
 * SPDX-License-Identifier: MIT
 */

// FidelityFX's blob accessor for the FSR 3.1 upscaler (backends/shared/blob_accessors/ffx_fsr3upscaler_shaderblobs.cpp)
// with the shaders compiled from the SDK's GLSL when first asked for, instead of read from pre-built SPIR-V headers.
// The macros and the size optimisation are those of AMD's FidelityFX_SC run (one blob per pass, FFX_HALF and the six
// permutation options). The reflection FfxShaderBlob carries (names, bindings, counts) is read from the SPIR-V: the
// resources the optimised module still uses, named by the same shader compiled without optimisation (which strips names).
#include <FidelityFX/host/ffx_util.h>
#include "ffx_fsr3upscaler_shaderblobs.h"
#include "fsr3upscaler/ffx_fsr3upscaler_private.h"

#include <algorithm>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string.h>
#include <string>
#include <thread>
#include <vector>

#include "ffx_fsr3_glsl.h"

namespace {

struct PassInfo {
    const char* glsl;
    bool        half;  // has an FFX_HALF=1 build (the others ignore FSR3UPSCALER_SHADER_PERMUTATION_ALLOW_FP16)
};

const PassInfo passes[] = {
    {"ffx_fsr3upscaler_prepare_inputs_pass.glsl", false},
    {"ffx_fsr3upscaler_luma_pyramid_pass.glsl", false},
    {"ffx_fsr3upscaler_shading_change_pyramid_pass.glsl", false},
    {"ffx_fsr3upscaler_shading_change_pass.glsl", true},
    {"ffx_fsr3upscaler_prepare_reactivity_pass.glsl", true},
    {"ffx_fsr3upscaler_luma_instability_pass.glsl", false},
    {"ffx_fsr3upscaler_accumulate_pass.glsl", true},
    {"ffx_fsr3upscaler_rcas_pass.glsl", false},
    {"ffx_fsr3upscaler_debug_view_pass.glsl", false},
    {"ffx_fsr3upscaler_autogen_reactive_pass.glsl", true},
};

// index into passes, or -1
int passIndex(FfxFsr3UpscalerPass pass)
{
    switch (pass) {
    case FFX_FSR3UPSCALER_PASS_PREPARE_INPUTS: return 0;
    case FFX_FSR3UPSCALER_PASS_LUMA_PYRAMID: return 1;
    case FFX_FSR3UPSCALER_PASS_SHADING_CHANGE_PYRAMID: return 2;
    case FFX_FSR3UPSCALER_PASS_SHADING_CHANGE: return 3;
    case FFX_FSR3UPSCALER_PASS_PREPARE_REACTIVITY: return 4;
    case FFX_FSR3UPSCALER_PASS_LUMA_INSTABILITY: return 5;
    case FFX_FSR3UPSCALER_PASS_ACCUMULATE:
    case FFX_FSR3UPSCALER_PASS_ACCUMULATE_SHARPEN: return 6;
    case FFX_FSR3UPSCALER_PASS_RCAS: return 7;
    case FFX_FSR3UPSCALER_PASS_DEBUG_VIEW: return 8;
    case FFX_FSR3UPSCALER_PASS_GENERATE_REACTIVE: return 9;
    default: return -1;
    }
}

// One resource kind of a blob: the arrays FfxShaderBlob points at.
struct Group {
    std::vector<std::string>  names;
    std::vector<const char*>  namePtrs;
    std::vector<uint32_t>     bindings, counts, spaces;
    uint32_t count() const { return (uint32_t)bindings.size(); }
};

struct Entry {
    int           state = 0;  // 0 being built, 1 ready, 2 failed
    uint32_t*     spv = nullptr;
    size_t        nwords = 0;
    Group         cbv, srv, uav, sampler;
    FfxShaderBlob blob = {};
};

// What reflection needs of a module: its variables with a descriptor binding.
struct Var {
    uint32_t    set, binding, count;
    int         kind;  // 0 constant buffer, 1 sampled image, 2 storage image, 3 sampler
    bool        used;
    std::string name;
};

bool scan(const uint32_t* spv, size_t nwords, std::vector<Var>& vars)
{
    enum { OP_FUNCTION_CALL = 57, OP_IMAGE_TEXEL_POINTER = 60, OP_LOAD = 61, OP_STORE = 62, OP_ACCESS_CHAIN = 65,
           OP_IN_BOUNDS_ACCESS_CHAIN = 66, OP_NAME = 5, OP_TYPE_IMAGE = 25, OP_TYPE_SAMPLER = 26, OP_TYPE_ARRAY = 28,
           OP_TYPE_STRUCT = 30, OP_TYPE_POINTER = 32, OP_CONSTANT = 43, OP_VARIABLE = 59, OP_DECORATE = 71 };
    enum { DEC_BINDING = 33, DEC_SET = 34 };
    enum { SC_UNIFORM_CONSTANT = 0, SC_UNIFORM = 2 };
    if (nwords < 5 || spv[0] != 0x07230203) return false;
    const uint32_t bound = spv[3];
    std::vector<uint32_t> op(bound), a(bound), b(bound), binding(bound, ~0u), set(bound, 0), value(bound);
    std::vector<std::string> name(bound);
    std::vector<bool> used(bound);
    for (size_t i = 5; i < nwords;) {
        const uint32_t* w = spv + i;
        const uint32_t  wc = w[0] >> 16, o = w[0] & 0xFFFF;
        if (!wc || i + wc > nwords) return false;
        if (o == OP_NAME && wc >= 3 && w[1] < bound) name[w[1]] = (const char*)(w + 2);
        else if (o == OP_DECORATE && wc >= 4 && w[1] < bound && w[2] == DEC_BINDING) binding[w[1]] = w[3];
        else if (o == OP_DECORATE && wc >= 4 && w[1] < bound && w[2] == DEC_SET) set[w[1]] = w[3];
        else if (o == OP_TYPE_IMAGE && wc >= 9 && w[1] < bound) op[w[1]] = o, a[w[1]] = w[7];  // sampled: 1 or 2 (storage)
        else if ((o == OP_TYPE_SAMPLER || o == OP_TYPE_STRUCT) && wc >= 2 && w[1] < bound) op[w[1]] = o;
        else if (o == OP_TYPE_ARRAY && wc >= 4 && w[1] < bound) op[w[1]] = o, a[w[1]] = w[2], b[w[1]] = w[3];  // element, length id
        else if (o == OP_TYPE_POINTER && wc >= 4 && w[1] < bound) op[w[1]] = o, a[w[1]] = w[3];
        else if (o == OP_CONSTANT && wc >= 4 && w[2] < bound) value[w[2]] = w[3];
        else if (o == OP_VARIABLE && wc >= 4 && w[2] < bound) op[w[2]] = o, a[w[2]] = w[1], b[w[2]] = w[3];  // pointer type, class
        else if ((o == OP_LOAD || o == OP_ACCESS_CHAIN || o == OP_IN_BOUNDS_ACCESS_CHAIN || o == OP_IMAGE_TEXEL_POINTER) && wc >= 4 && w[3] < bound)
            used[w[3]] = true;
        else if (o == OP_STORE && wc >= 3 && w[1] < bound) used[w[1]] = true;
        else if (o == OP_FUNCTION_CALL)
            for (uint32_t k = 4; k < wc; k++)
                if (w[k] < bound) used[w[k]] = true;
        i += wc;
    }
    for (uint32_t id = 1; id < bound; id++) {
        if (op[id] != OP_VARIABLE || binding[id] == ~0u) continue;
        uint32_t t = a[a[id]], count = 1;  // pointee
        while (t < bound && op[t] == OP_TYPE_ARRAY) count *= value[b[t]], t = a[t];
        if (t >= bound) return false;
        int kind;
        if (op[t] == OP_TYPE_STRUCT && b[id] == SC_UNIFORM) kind = 0;
        else if (op[t] == OP_TYPE_IMAGE && a[t] == 1 && b[id] == SC_UNIFORM_CONSTANT) kind = 1;
        else if (op[t] == OP_TYPE_IMAGE && a[t] == 2 && b[id] == SC_UNIFORM_CONSTANT) kind = 2;
        else if (op[t] == OP_TYPE_SAMPLER) kind = 3;
        else return false;  // (buffers and acceleration structures: none in these shaders)
        vars.push_back({set[id], binding[id], count, kind, used[id], name[id]});
    }
    return true;
}

// The groups from the optimised module's variables that are used, with the names of the plain one's at those bindings.
bool reflect(Entry& e, const uint32_t* plain, size_t plainWords)
{
    std::vector<Var> vars, named;
    if (!scan(e.spv, e.nwords, vars) || !scan(plain, plainWords, named)) return false;
    std::map<uint64_t, const std::string*> names;
    for (const Var& v : named) names[(uint64_t)v.set << 32 | v.binding] = &v.name;
    std::sort(vars.begin(), vars.end(), [](const Var& x, const Var& y) { return x.set != y.set ? x.set < y.set : x.binding < y.binding; });
    for (const Var& v : vars) {
        auto it = names.find((uint64_t)v.set << 32 | v.binding);
        if (it == names.end() || it->second->empty()) return false;
        if (!v.used) continue;  // (declared only)
        Group& g = v.kind == 0 ? e.cbv : v.kind == 1 ? e.srv : v.kind == 2 ? e.uav : e.sampler;
        g.names.push_back(*it->second);
        g.bindings.push_back(v.binding);
        g.counts.push_back(v.count);
        g.spaces.push_back(v.set);
    }
    for (Group* g : {&e.cbv, &e.srv, &e.uav, &e.sampler})
        for (const std::string& n : g->names) g->namePtrs.push_back(n.c_str());
    return true;
}

void fillBlob(Entry& e)
{
    FfxShaderBlob blob = {
        (const uint8_t*)e.spv, (uint32_t)(e.nwords * 4),
        e.cbv.count(), e.srv.count(), e.uav.count(), 0, 0, e.sampler.count(), 0,
        e.cbv.namePtrs.data(), e.cbv.bindings.data(), e.cbv.counts.data(), e.cbv.spaces.data(),
        e.srv.namePtrs.data(), e.srv.bindings.data(), e.srv.counts.data(), e.srv.spaces.data(),
        e.uav.namePtrs.data(), e.uav.bindings.data(), e.uav.counts.data(), e.uav.spaces.data(),
        nullptr, nullptr, nullptr, nullptr,
        nullptr, nullptr, nullptr, nullptr,
        e.sampler.namePtrs.data(), e.sampler.bindings.data(), e.sampler.counts.data(), e.sampler.spaces.data(),
        nullptr, nullptr, nullptr, nullptr,
    };
    memcpy(&e.blob, &blob, sizeof blob);
}

// (pass shader, FFX_HALF, the six option bits) -> blob; built once, by whoever asks first, the others wait for it.
std::mutex              mutex;
std::condition_variable built;
Entry*                  table[10][2][64];

const Entry* get(FfxFsr3UpscalerPass pass, uint32_t options)
{
    const int       index = passIndex(pass);
    const PassInfo& info = passes[index];
    const int       half = info.half && (options & FSR3UPSCALER_SHADER_PERMUTATION_ALLOW_FP16) ? 1 : 0;
    const uint32_t  key = options & 0x3F;  // wave64 selects the same SPIR-V (only the subgroup size at pipeline creation differs)
    Entry*&         slot = table[index][half][key];
    std::unique_lock<std::mutex> lock(mutex);
    if (slot) {
        Entry* e = slot;
        built.wait(lock, [e] { return e->state != 0; });
        return e->state == 1 ? e : nullptr;
    }
    Entry* e = slot = new Entry;
    lock.unlock();

    static const char* const optionNames[6] = {"REPROJECT_USE_LANCZOS_TYPE", "HDR_COLOR_INPUT", "LOW_RESOLUTION_MOTION_VECTORS",
                                               "JITTERED_MOTION_VECTORS", "INVERTED_DEPTH", "APPLY_SHARPENING"};
    std::vector<std::string> defs = {"FFX_GPU=1", "FFX_GLSL=1", std::string("FFX_HALF=") + (half ? "1" : "0"),
        "FFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0", "FFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0",
        "FFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1", "FFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0",
        "FFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2"};
    for (int i = 0; i < 6; i++)
        defs.push_back(std::string("FFX_FSR3UPSCALER_OPTION_") + optionNames[i] + (key >> i & 1 ? "=1" : "=0"));
    std::vector<const char*> defPtrs;
    for (const std::string& d : defs) defPtrs.push_back(d.c_str());

    size_t      length = 0;
    const char* src = ffxFsr3GlslFile(info.glsl, &length);
    std::string name = std::string("fsr3 ") + info.glsl + (half ? " half" : "") + " options " + std::to_string(key);
    bool        ok = false;
    if (src) {
        auto include = [](const char* n, size_t* len) { return ffxFsr3GlslFile(n, len); };
        size_t plainWords = 0;
        e->spv = vk_glsl_to_spirv_ex(src, length, name.c_str(), defPtrs.data(), (int)defPtrs.size(), include, ffxFsr3GlslHash(), 1, &e->nwords);
        uint32_t* plain = vk_glsl_to_spirv_ex(src, length, (name + " (names)").c_str(), defPtrs.data(), (int)defPtrs.size(), include,
                                              ffxFsr3GlslHash(), 0, &plainWords);
        ok = e->spv && plain && reflect(*e, plain, plainWords);
        if (e->spv && plain && !ok) ffxVkPortableLogf("%s: its resources could not be read from the SPIR-V", name.c_str());
        free(plain);
        if (ok) fillBlob(*e);
    } else {
        ffxVkPortableLogf("%s: no GLSL source", name.c_str());
    }
    lock.lock();
    e->state = ok ? 1 : 2;
    built.notify_all();
    return ok ? e : nullptr;
}

}  // namespace

FfxErrorCode fsr3UpscalerGetPermutationBlobByIndex(
    FfxFsr3UpscalerPass passId,
    uint32_t permutationOptions,
    FfxShaderBlob* outBlob)
{
    // The first request starts the other passes' builds in parallel (the first one's options, sharpening only for its
    // pass: what getPipelinePermutationFlags gives them), so the context creation waits for the slowest one only.
    static std::once_flag once;
    std::call_once(once, [permutationOptions] {
        for (int pass = 0; pass < FFX_FSR3UPSCALER_PASS_TCR_AUTOGENERATE; pass++) {
            uint32_t o = permutationOptions & ~(uint32_t)FSR3UPSCALER_SHADER_PERMUTATION_ENABLE_SHARPENING;
            if (pass == FFX_FSR3UPSCALER_PASS_ACCUMULATE_SHARPEN) o |= FSR3UPSCALER_SHADER_PERMUTATION_ENABLE_SHARPENING;
            std::thread([pass, o] { get((FfxFsr3UpscalerPass)pass, o); }).detach();
        }
    });
    const Entry* e = passIndex(passId) >= 0 ? get(passId, permutationOptions) : nullptr;
    if (!e) {
        memset(outBlob, 0, sizeof(FfxShaderBlob));
        return FFX_ERROR_BACKEND_API_ERROR;
    }
    memcpy(outBlob, &e->blob, sizeof(FfxShaderBlob));
    return FFX_OK;
}

FfxErrorCode fsr3UpscalerIsWave64(uint32_t permutationOptions, bool& isWave64)
{
    isWave64 = FFX_CONTAINS_FLAG(permutationOptions, FSR3UPSCALER_SHADER_PERMUTATION_FORCE_WAVE64);
    return FFX_OK;
}
