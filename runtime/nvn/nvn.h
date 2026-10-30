// NVN implemented on the host. Guest NVN objects hold a pointer to a host object at offset 0 (NVN structures are
// opaque blobs of fixed size to the game, 32- and 64-bit alike); builders and small state objects keep their fields
// in guest memory in our own layout.
//
// Calls come from 32-bit code (AAPCS, VFP hard-float): each NVN function's signature (NVNS) tells the argument
// adapter (nvn_args.c) which arguments are 64-bit (NVNbufferAddress, handles: an even/odd register pair or an
// 8-byte stack slot) and which are floats (s registers). The implementations then see the arguments the way the
// AArch64 ABI would hand them over: integer-class arguments in order as A(i), floating point in order as AF(i).
#pragma once
#include "../hle.h"
#include "tegra.h"

typedef struct {
    uint64_t x[16];  // integer, pointer and 64-bit arguments, in order (A(i))
    float f[8];      // float arguments, in order (AF(i))
} NvnArgs;

// sig: one letter per argument: 'i' 32-bit integer / pointer / enum / size_t, 'l' 64-bit integer, 'f' float.
// NULL: every argument is 32-bit (the common case).
void nvn_args(Cpu *c, const char *sig, NvnArgs *a);

#define NVNS(name, sig)                                                                     \
    static void nvn_impl_##name(Cpu *c, NvnArgs *na_);                                      \
    HLE_FUNC(#name, hle_##name) { NvnArgs na_; nvn_args(c, sig, &na_); nvn_impl_##name(c, &na_); } \
    static void nvn_impl_##name(Cpu *c, NvnArgs *na_)
#define NVN(name) NVNS(name, NULL)
// (debug) in handlers that ignore their call: MHGU_NVN_NOOP_LOG=1 logs the first call of each
#define NVN_NOOP() do { static int seen_; if (!seen_++ && getenv("MHGU_NVN_NOOP_LOG")) LOG("[nvn] ignored: %s\n", __func__ + 9); } while (0)

// argument helpers (inside NVN / NVNS bodies)
#undef ARG
#undef ARGP
#define A(i) (na_->x[(i)])
#define AP(i) ((uint8_t *)G2H((uint32_t)na_->x[(i)]))
#define AF(i) (na_->f[(i)])
#undef RET
#undef RET64
#undef RETF
#define RET(v) (c->r[0] = (uint32_t)(v))
#define RET64(v) do { uint64_t r_ = (uint64_t)(v); c->r[0] = (uint32_t)r_; c->r[1] = (uint32_t)(r_ >> 32); } while (0)
#define RETF(fv_) (c->v.f[0] = (fv_))
// host object pointer stored at offset 0 of a guest NVN object (guest address)
#define HOST(T, guest) ((T *)(uintptr_t)(*(uint64_t *)G2H((uint32_t)(guest))))
#define SET_HOST(guest, p) (*(uint64_t *)G2H((uint32_t)(guest)) = (uint64_t)(uintptr_t)(p))
// a field of a guest structure (guest address + offset)
#define G(T, base, off) (*(T *)((uint8_t *)G2H((uint32_t)(base)) + (off)))

typedef struct NvnPool {
    uint64_t storage, size;  // guest address (== GPU address) and size
    uint32_t flags;
} NvnPool;

typedef struct NvnTexture {
    TexLayout layout;
    uint32_t target, format, flags, width, height, depth, levels, samples, ds_mode;
    uint8_t swizzle[4];
    uint64_t stride;
    uint64_t addr;  // guest address of the texel storage
    NvnPool *pool;
    uint64_t pool_offset;
    void *backend;  // renderer-side texture
    uint64_t debug_id;
    char label[64];
} NvnTexture;

typedef struct {
    uint32_t min_filter, mag_filter, wrap[3], compare_mode, compare_func, reduction;
    float lod_min, lod_max, lod_bias, max_aniso;
    float border[4];
    void *backend;
} NvnSamplerDesc;

typedef struct {
    uint32_t setmask;  // which fields are overridden
    uint16_t base_level, num_levels, min_layer, num_layers;
    uint32_t format, target, ds_mode;
    uint8_t swizzle[4];
} NvnTextureView;  // our layout of the guest NVNtextureView (0x28 bytes)

typedef struct {
    NvnTexture *tex;
    NvnTextureView view;
    bool has_view;
} NvnTexEntry;

typedef struct NvnDescPool {
    uint64_t addr;
    int count;
    void *entries;  // NvnTexEntry[] or NvnSamplerDesc[]
} NvnDescPool;

typedef struct NvnShaderStage {
    uint32_t stage;           // NVNshaderStage
    uint64_t code_addr;       // GPU (guest) address of the code
    const uint8_t *control;   // control section (host pointer)
    uint32_t const_offset, const_size;  // compiler constants (bound as c1), within the code blob
    void *backend;
} NvnShaderStage;

typedef struct NvnProgram {
    NvnShaderStage stages[6];
    uint32_t stage_mask;
    void *backend;
} NvnProgram;

typedef struct NvnWindow {
    uint64_t textures[8];  // guest NVNtexture*
    int num, active, interval, next;
    int crop[4];
} NvnWindow;

typedef struct NvnSync {
    volatile int signaled;
    volatile uint64_t seq;  // GPU submission this fence belongs to (renderer_fence_seq), 0 = none
} NvnSync;

// ---- command recording (stored in guest command memory)
typedef struct {
    uint64_t mem;    // current command memory chunk (guest address)
    uint64_t size;   // chunk size
    uint64_t used;   // bytes used in chunk
    uint64_t start;  // first record of the list being recorded
    uint64_t callback, callback_data;
    bool recording;
    uint64_t guest;  // the guest NVNcommandBuffer*
} NvnCmdBuf;

// One recorded command: header, the call's integer and float arguments (as A(1..7) / AF(0..3)), then copied data.
typedef struct {
    uint32_t op, size;  // size includes the header, multiple of 8
    uint64_t x[7];
    float f[4];
    uint8_t extra[];
} CmdRec;

enum {
    CMD_END = 0, CMD_JUMP, CMD_CALL,
    CMD_BIND_PROGRAM, CMD_BIND_BLEND, CMD_BIND_CHANNEL_MASK, CMD_BIND_COLOR, CMD_BIND_DEPTH_STENCIL,
    CMD_BIND_MULTISAMPLE, CMD_BIND_POLYGON, CMD_BIND_VERTEX_ATTRIB, CMD_BIND_VERTEX_STREAM,
    CMD_BIND_VERTEX_BUFFERS, CMD_BIND_UNIFORM_BUFFERS, CMD_BIND_STORAGE_BUFFERS, CMD_BIND_TEXTURES,
    CMD_BIND_IMAGES, CMD_SET_TEXTURE_POOL, CMD_SET_SAMPLER_POOL, CMD_SET_RENDER_TARGETS,
    CMD_SET_VIEWPORTS, CMD_SET_SCISSORS, CMD_SET_DEPTH_RANGES, CMD_SET_BLEND_COLOR, CMD_SET_STENCIL,
    CMD_SET_PRIMITIVE_RESTART, CMD_SET_ALPHA_REF, CMD_SET_DEPTH_BOUNDS, CMD_SET_POLYGON_OFFSET,
    CMD_SET_LINE_WIDTH, CMD_SET_POINT_SIZE, CMD_SET_SAMPLE_MASK, CMD_SET_RENDER_ENABLE, CMD_SET_DEPTH_CLAMP,
    CMD_CLEAR_COLOR, CMD_CLEAR_DEPTH_STENCIL, CMD_CLEAR_TEXTURE, CMD_CLEAR_BUFFER,
    CMD_DRAW_ARRAYS, CMD_DRAW_ELEMENTS, CMD_DRAW_ARRAYS_INDIRECT, CMD_DRAW_ELEMENTS_INDIRECT,
    CMD_DISPATCH, CMD_DISPATCH_INDIRECT,
    CMD_COPY_BUFFER_TO_BUFFER, CMD_COPY_BUFFER_TO_TEXTURE, CMD_COPY_TEXTURE_TO_BUFFER,
    CMD_COPY_TEXTURE_TO_TEXTURE, CMD_DOWNSAMPLE, CMD_UPDATE_UNIFORM_BUFFER, CMD_FENCE_SYNC, CMD_WAIT_SYNC,
    CMD_BARRIER, CMD_SIGNAL_EVENT, CMD_WAIT_EVENT, CMD_REPORT_COUNTER, CMD_DISCARD_COLOR,
    CMD_DISCARD_DEPTH_STENCIL, CMD_SET_COPY_STRIDES, CMD_DRAW_TEXTURE, CMD_RESOLVE_DEPTH,
    CMD_COUNT
};

// RtRec: CMD_SET_RENDER_TARGETS payload (host texture pointers)
typedef struct {
    uint64_t tex[8];
    NvnTextureView views[8];
    uint8_t has_view[8];
    uint64_t depth;
    NvnTextureView dview;
    uint8_t has_dview;
} RtRec;
// TexRef: copy / clear payloads
typedef struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; } TexRef;
typedef struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; float color[4]; } ClearTexRec;

// renderer backend (runtime/nvn/vulkan)
void backend_init(void);
void backend_quit(void);  // the window is closing: wind the renderer down and leave (returns only without a renderer)
void backend_execute(uint64_t handle);            // run a recorded command list (guest address of its first record)
void backend_present(NvnTexture *tex);            // show a texture in the window
void backend_texture_destroyed(NvnTexture *tex);
void backend_pool_written(uint64_t addr, uint64_t size);
void backend_program_created(const NvnProgram *p);  // nvnProgramSetShaders (the renderer may prepare it ahead of use)
void backend_photo_requested(void);  // the game asked for its quest photo (tools/gen.py: 0xa6f97c): drawn in the next frames
void renderer_sync(void);
uint64_t renderer_fence_seq(void);
void renderer_wait_seq(uint64_t seq);
void renderer_flush(void);
