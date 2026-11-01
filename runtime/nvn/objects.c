// NVN objects, builders, and state objects (everything except command recording). Port of LAS Recomp's
// runtime/nvn/objects.c to the 32-bit game: arguments through the adapter (nvn.h), guest pointers through G2H,
// 64-bit results (handles, GPU addresses) in r0:r1.
#include <stdlib.h>
#include <string.h>

#include "../kernel.h"
#include "nvn.h"

static uint64_t next_debug_id = 1;

// ================================================================== device
static uint32_t device_integer(uint32_t which) {
    static const uint32_t v[] = {
        [0] = 53, [1] = 207,           // API version
        [2] = 14, [3] = 65536, [4] = 256, [5] = 8, [6] = 16, [7] = 4, [8] = 16, [9] = 32,
        [10] = 16, [11] = 4, [12] = 4, [13] = 4, [14] = 16, [15] = 32, [16] = 32,
        [17] = 256, [18] = 256,        // reserved texture / sampler descriptors
        [19] = 4, [20] = 8, [21] = 0x10000, [22] = 0x1000,
        [23] = 256, [24] = 256, [25] = 4096, [26] = 0x20000, [27] = 16,
        [28] = 1536, [29] = 1024, [30] = 64, [31] = 1536,
        [32] = 0x7FFFFFFF, [33] = 65535, [34] = 65535,
        [35] = 8, [36] = 1048576, [37] = 4096, [38] = 16, [39] = 0x10000,
        [40] = 1, [41] = 1, [42] = 1,  // min/max filtering, stencil8, ASTC
        [43] = 0x40000, [44] = 15, [45] = 2048,
        [46] = 1, [47] = 5, [48] = 1, [49] = 15,
        [50] = 256, [51] = 1, [52] = 0, [53] = 0, [54] = 0, [55] = 256, [56] = 1, [57] = 0x1000,
        [58] = 1, [59] = 1, [60] = 1, [61] = 1, [62] = 1, [63] = 1, [64] = 1,
    };
    return which < sizeof v / sizeof *v ? v[which] : 0;
}

NVN(nvnDeviceBuilderSetDefaults) { G(uint32_t, A(0), 0) = 0; }
NVN(nvnDeviceBuilderSetFlags) { G(uint32_t, A(0), 0) = (uint32_t)A(1); }
NVN(nvnDeviceBuilderGetFlags) { RET(G(uint32_t, A(0), 0)); }
NVN(nvnDeviceInitialize) {
    static bool once;
    if (!once) { once = true; backend_init(); }
    LOG("[nvn] device initialized\n");
    RET(1);
}
NVN(nvnDeviceFinalize) { (void)c; }
NVN(nvnDeviceSetDebugLabel) { (void)c; }
NVN(nvnDeviceGetInteger) { *(int32_t *)AP(2) = (int32_t)device_integer((uint32_t)A(1)); }
NVN(nvnDeviceGetCurrentTimestampInNanoseconds) { RET64(plat_now_ns()); }
NVN(nvnDeviceGetTimestampInNanoseconds) {  // counter.timestamp: Switch GPU ticks (614.4 MHz)
    RET64((uint64_t)((unsigned __int128)G(uint64_t, A(1), 8) * 625 / 384));
}
NVN(nvnDeviceSetIntermediateShaderCache) { (void)c; }
NVN(nvnDeviceGetTextureHandle) { RET64((A(1) & 0xFFFFF) | ((A(2) & 0xFFF) << 20)); }
NVN(nvnDeviceGetTexelFetchHandle) { RET64(A(1) & 0xFFFFF); }
NVN(nvnDeviceGetImageHandle) { RET64(A(1) & 0xFFFFF); }
NVN(nvnDeviceGetSeparateTextureHandle) { RET64(A(1) & 0xFFFFF); }
NVN(nvnDeviceGetSeparateSamplerHandle) { RET64((A(1) & 0xFFF) << 20); }
NVN(nvnDeviceInstallDebugCallback) { (void)c; }
NVN(nvnDeviceGenerateDebugDomainId) { RET(1); }
static uint32_t window_origin, depth_mode;
NVN(nvnDeviceSetWindowOriginMode) { window_origin = (uint32_t)A(1); LOG("[nvn] window origin mode %u\n", window_origin); }
uint32_t nvn_window_origin(void) { return window_origin; }
uint32_t nvn_depth_mode(void) { return depth_mode; }
NVN(nvnDeviceGetWindowOriginMode) { RET(window_origin); }
NVN(nvnDeviceSetDepthMode) { depth_mode = (uint32_t)A(1); LOG("[nvn] depth mode %u\n", depth_mode); }
NVN(nvnDeviceGetDepthMode) { RET(depth_mode); }
NVN(nvnDeviceRegisterFastClearColor) { RET(1); }
NVN(nvnDeviceRegisterFastClearColori) { RET(1); }
NVN(nvnDeviceRegisterFastClearColorui) { RET(1); }
NVNS(nvnDeviceRegisterFastClearDepth, "if") { RET(1); }
NVN(nvnDeviceApplyDeferredFinalizes) { (void)c; }
NVN(nvnDeviceFinalizeCommandHandle) { (void)c; }
NVN(nvnDeviceIsExternalDebuggerAttached) { RET(0); }
NVN(nvnDeviceWalkDebugDatabase) { (void)c; }

// ================================================================== queue
// QueueBuilder layout: device@0 flags@8 cmd@0xC compute@0x10 control@0x14 mem@0x18 memsize@0x20
//                      flush@0x28 priority@0x2C
NVN(nvnQueueBuilderSetDevice) { G(uint64_t, A(0), 0) = A(1); }
NVN(nvnQueueBuilderSetDefaults) {
    memset(AP(0), 0, 0x30);
    G(uint32_t, A(0), 0x0C) = 0x10000;
    G(uint32_t, A(0), 0x10) = 0x40000;
    G(uint32_t, A(0), 0x14) = 0x4000;
    G(uint32_t, A(0), 0x28) = 0x8000;
    G(uint32_t, A(0), 0x2C) = 1;
}
NVN(nvnQueueBuilderSetFlags) { G(uint32_t, A(0), 8) = (uint32_t)A(1); }
NVN(nvnQueueBuilderSetCommandMemorySize) { G(uint32_t, A(0), 0x0C) = (uint32_t)A(1); }
NVN(nvnQueueBuilderSetComputeMemorySize) { G(uint32_t, A(0), 0x10) = (uint32_t)A(1); }
NVN(nvnQueueBuilderSetControlMemorySize) { G(uint32_t, A(0), 0x14) = (uint32_t)A(1); }
NVN(nvnQueueBuilderSetQueueMemory) { G(uint64_t, A(0), 0x18) = A(1); G(uint64_t, A(0), 0x20) = A(2); }
NVN(nvnQueueBuilderSetCommandFlushThreshold) { G(uint32_t, A(0), 0x28) = (uint32_t)A(1); }
NVN(nvnQueueBuilderSetQueuePriority) { G(uint32_t, A(0), 0x2C) = (uint32_t)A(1); }
NVN(nvnQueueBuilderGetDevice) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnQueueBuilderGetFlags) { RET(G(uint32_t, A(0), 8)); }
NVN(nvnQueueBuilderGetCommandMemorySize) { *(int32_t *)AP(1) = G(int32_t, A(0), 0x0C); RET(1); }
NVN(nvnQueueBuilderGetComputeMemorySize) { *(int32_t *)AP(1) = G(int32_t, A(0), 0x10); RET(1); }
NVN(nvnQueueBuilderGetControlMemorySize) { *(int32_t *)AP(1) = G(int32_t, A(0), 0x14); RET(1); }
NVN(nvnQueueBuilderGetCommandFlushThreshold) { *(int32_t *)AP(1) = G(int32_t, A(0), 0x28); RET(1); }
NVN(nvnQueueBuilderGetQueuePriority) { RET(G(uint32_t, A(0), 0x2C)); }
NVN(nvnQueueBuilderGetMemorySize) { RET(0x10000); }  // required queue memory
NVN(nvnQueueBuilderGetQueueMemorySize) { RET(G(uint64_t, A(0), 0x20)); }
NVN(nvnQueueBuilderGetMemory) { RET(G(uint64_t, A(0), 0x18)); }

NVN(nvnQueueInitialize) { RET(1); }
NVN(nvnQueueFinalize) { (void)c; }
NVN(nvnQueueSetDebugLabel) { (void)c; }
NVN(nvnQueueGetError) { RET(0); }
NVN(nvnQueueGetTotalCommandMemoryUsed) { RET(0); }
NVN(nvnQueueGetTotalControlMemoryUsed) { RET(0); }
NVN(nvnQueueGetTotalComputeMemoryUsed) { RET(0); }
NVN(nvnQueueResetMemoryUsageCounts) { (void)c; }
NVN(nvnQueueSubmitCommands) {  // (queue, count, const NVNcommandHandle *handles): 64-bit handles
    int n = (int)A(1);
    for (int i = 0; i < n; i++) backend_execute(LD64((uint32_t)A(2) + 8u * (uint32_t)i));
}
NVN(nvnQueueFlush) { (void)c; }
NVN(nvnQueueFinish) { (void)c; renderer_sync(); }

static NvnSync *sync_of(uint64_t guest) {
    NvnSync *s = HOST(NvnSync, guest);
    if (!s) { s = calloc(1, sizeof *s); SET_HOST(guest, s); }
    return s;
}
NVN(nvnQueueFenceSync) { NvnSync *s = sync_of(A(1)); s->signaled = 1; s->seq = renderer_fence_seq(); }
void nvn_sync_set_seq(uint64_t guest, uint64_t seq) { NvnSync *s = sync_of(guest); s->signaled = 1; s->seq = seq; }
NVN(nvnQueueWaitSync) { RET(1); }

// ================================================================== sync / event
NVN(nvnSyncInitialize) { SET_HOST(A(0), calloc(1, sizeof(NvnSync))); RET(1); }
NVN(nvnSyncFinalize) { free(HOST(NvnSync, A(0))); SET_HOST(A(0), 0); }
NVN(nvnSyncSetDebugLabel) { (void)c; }
NVNS(nvnSyncWait, "il") {  // (sync, u64 timeout) -> NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED
    renderer_wait_seq(sync_of(A(0))->seq);
    RET(0);
}
NVN(nvnSyncCreateGLSync) { RET(0); }
NVN(nvnSyncInitializeFromFencedGLSync) { RET(0); }

// EventBuilder: pool@0 offset@8 ; Event: storage address (u32 value lives in guest memory)
NVN(nvnEventBuilderSetDefaults) { G(uint64_t, A(0), 0) = 0; G(uint64_t, A(0), 8) = 0; }
NVN(nvnEventBuilderSetStorage) { G(uint64_t, A(0), 0) = A(1); G(uint64_t, A(0), 8) = A(2); }
NVN(nvnEventBuilderGetStorage) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnEventBuilderGetMemoryPool) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnEventBuilderGetMemoryOffset) { RET(G(uint64_t, A(0), 8)); }
static uint64_t event_addr(uint64_t ev) { return G(uint64_t, ev, 0); }
NVN(nvnEventInitialize) {
    uint64_t pool = G(uint64_t, A(1), 0), off = G(uint64_t, A(1), 8);
    G(uint64_t, A(0), 0) = HOST(NvnPool, pool)->storage + off;
    G(uint64_t, A(0), 8) = pool;
    G(uint64_t, A(0), 16) = off;
    RET(1);
}
NVN(nvnEventFinalize) { (void)c; }
NVN(nvnEventGetValue) { RET(LD32(event_addr(A(0)))); }
NVN(nvnEventSignal) { ST32(event_addr(A(0)), (uint32_t)A(2)); }
NVN(nvnEventGetMemoryPool) { RET(G(uint64_t, A(0), 8)); }
NVN(nvnEventGetMemoryOffset) { RET(G(uint64_t, A(0), 16)); }

// ================================================================== memory pools
// Builder: device@0 flags@8 storage@0x10 size@0x18
NVN(nvnMemoryPoolBuilderSetDevice) { G(uint64_t, A(0), 0) = A(1); }
NVN(nvnMemoryPoolBuilderSetDefaults) { memset(AP(0), 0, 0x20); G(uint32_t, A(0), 8) = 0x9; }
NVN(nvnMemoryPoolBuilderSetFlags) { G(uint32_t, A(0), 8) = (uint32_t)A(1); }
NVN(nvnMemoryPoolBuilderSetStorage) { G(uint64_t, A(0), 0x10) = A(1); G(uint64_t, A(0), 0x18) = A(2); }
NVN(nvnMemoryPoolBuilderGetDevice) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnMemoryPoolBuilderGetFlags) { RET(G(uint32_t, A(0), 8)); }
NVN(nvnMemoryPoolBuilderGetMemory) { RET(G(uint64_t, A(0), 0x10)); }
NVN(nvnMemoryPoolBuilderGetSize) { RET(G(uint64_t, A(0), 0x18)); }

NVN(nvnMemoryPoolInitialize) {
    NvnPool *p = calloc(1, sizeof *p);
    p->storage = G(uint64_t, A(1), 0x10);
    p->size = G(uint64_t, A(1), 0x18);
    p->flags = G(uint32_t, A(1), 8);
    plat_commit(GUEST_BASE + p->storage, p->size);
    SET_HOST(A(0), p);
    RET(1);
}
NVN(nvnMemoryPoolFinalize) { free(HOST(NvnPool, A(0))); SET_HOST(A(0), 0); }
NVN(nvnMemoryPoolSetDebugLabel) { (void)c; }
NVN(nvnMemoryPoolMap) { RET(HOST(NvnPool, A(0))->storage); }
NVN(nvnMemoryPoolGetBufferAddress) { RET64(HOST(NvnPool, A(0))->storage); }  // GPU address == guest address
NVN(nvnMemoryPoolGetSize) { RET(HOST(NvnPool, A(0))->size); }
NVN(nvnMemoryPoolGetFlags) { RET(HOST(NvnPool, A(0))->flags); }
NVN(nvnMemoryPoolFlushMappedRange) {
    NvnPool *p = HOST(NvnPool, A(0));
    backend_pool_written(p->storage + A(1), A(2));
}
NVN(nvnMemoryPoolInvalidateMappedRange) { (void)c; }
NVN(nvnMemoryPoolMapVirtual) { RET(1); }

// ================================================================== buffers
// Builder: device@0 pool@8 offset@0x10 size@0x18. Buffer: address@0 pool@8 offset@0x10 size@0x18 id@0x20
NVN(nvnBufferBuilderSetDevice) { G(uint64_t, A(0), 0) = A(1); }
NVN(nvnBufferBuilderSetDefaults) { memset(AP(0) + 8, 0, 0x18); }
NVN(nvnBufferBuilderSetStorage) {
    G(uint64_t, A(0), 8) = A(1); G(uint64_t, A(0), 0x10) = A(2); G(uint64_t, A(0), 0x18) = A(3);
}
NVN(nvnBufferBuilderGetDevice) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnBufferBuilderGetMemoryPool) { RET(G(uint64_t, A(0), 8)); }
NVN(nvnBufferBuilderGetMemoryOffset) { RET(G(uint64_t, A(0), 0x10)); }
NVN(nvnBufferBuilderGetSize) { RET(G(uint64_t, A(0), 0x18)); }
NVN(nvnBufferInitialize) {
    uint64_t pool = G(uint64_t, A(1), 8), off = G(uint64_t, A(1), 0x10);
    G(uint64_t, A(0), 0) = HOST(NvnPool, pool)->storage + off;
    G(uint64_t, A(0), 8) = pool;
    G(uint64_t, A(0), 0x10) = off;
    G(uint64_t, A(0), 0x18) = G(uint64_t, A(1), 0x18);
    G(uint64_t, A(0), 0x20) = next_debug_id++;
    RET(1);
}
NVN(nvnBufferFinalize) { (void)c; }
NVN(nvnBufferSetDebugLabel) { (void)c; }
NVN(nvnBufferMap) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnBufferGetAddress) { RET64(G(uint64_t, A(0), 0)); }
NVN(nvnBufferGetMemoryPool) { RET(G(uint64_t, A(0), 8)); }
NVN(nvnBufferGetMemoryOffset) { RET(G(uint64_t, A(0), 0x10)); }
NVN(nvnBufferGetSize) { RET(G(uint64_t, A(0), 0x18)); }
NVN(nvnBufferGetDebugID) { RET64(G(uint64_t, A(0), 0x20)); }
NVN(nvnBufferFlushMappedRange) { backend_pool_written(G(uint64_t, A(0), 0) + A(1), A(2)); }
NVN(nvnBufferInvalidateMappedRange) { (void)c; }

// ================================================================== textures
// TextureBuilder: device@0 flags@8 target@0xC width@0x10 height@0x14 depth@0x18 levels@0x1C
//   format@0x20 samples@0x24 swizzle@0x28 dsmode@0x2C stride@0x30 pool@0x38 offset@0x40
//   packaged@0x48 layout@0x50 glname@0x58
#define TB(T, off) G(T, A(0), off)
NVN(nvnTextureBuilderSetDevice) { TB(uint64_t, 0) = A(1); }
NVN(nvnTextureBuilderSetDefaults) {
    memset(AP(0) + 8, 0, 0x58);
    TB(uint32_t, 0xC) = 1;  // 2D
    TB(uint32_t, 0x10) = TB(uint32_t, 0x14) = TB(uint32_t, 0x18) = 1;
    TB(uint32_t, 0x1C) = 1;
    TB(uint8_t, 0x28) = 2; TB(uint8_t, 0x29) = 3; TB(uint8_t, 0x2A) = 4; TB(uint8_t, 0x2B) = 5;
}
NVN(nvnTextureBuilderSetFlags) { TB(uint32_t, 8) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetTarget) { TB(uint32_t, 0xC) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetWidth) { TB(uint32_t, 0x10) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetHeight) { TB(uint32_t, 0x14) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetDepth) { TB(uint32_t, 0x18) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetSize1D) { TB(uint32_t, 0x10) = (uint32_t)A(1); TB(uint32_t, 0x14) = 1; TB(uint32_t, 0x18) = 1; }
NVN(nvnTextureBuilderSetSize2D) { TB(uint32_t, 0x10) = (uint32_t)A(1); TB(uint32_t, 0x14) = (uint32_t)A(2); TB(uint32_t, 0x18) = 1; }
NVN(nvnTextureBuilderSetSize3D) {
    TB(uint32_t, 0x10) = (uint32_t)A(1); TB(uint32_t, 0x14) = (uint32_t)A(2); TB(uint32_t, 0x18) = (uint32_t)A(3);
}
NVN(nvnTextureBuilderSetLevels) { TB(uint32_t, 0x1C) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetFormat) { TB(uint32_t, 0x20) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetSamples) { TB(uint32_t, 0x24) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetSwizzle) {
    TB(uint8_t, 0x28) = (uint8_t)A(1); TB(uint8_t, 0x29) = (uint8_t)A(2);
    TB(uint8_t, 0x2A) = (uint8_t)A(3); TB(uint8_t, 0x2B) = (uint8_t)A(4);
}
NVN(nvnTextureBuilderSetDepthStencilMode) { TB(uint32_t, 0x2C) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetStride) { TB(uint64_t, 0x30) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetStorage) { TB(uint64_t, 0x38) = A(1); TB(uint64_t, 0x40) = A(2); }
NVN(nvnTextureBuilderSetPackagedTextureData) { TB(uint64_t, 0x48) = A(1); }
NVN(nvnTextureBuilderSetPackagedTextureLayout) { TB(uint64_t, 0x50) = A(1) ? *(uint64_t *)AP(1) : 0; }
NVN(nvnTextureBuilderSetGLTextureName) { TB(uint32_t, 0x58) = (uint32_t)A(1); }
NVN(nvnTextureBuilderGetDevice) { RET(TB(uint64_t, 0)); }
NVN(nvnTextureBuilderGetFlags) { RET(TB(uint32_t, 8)); }
NVN(nvnTextureBuilderGetTarget) { RET(TB(uint32_t, 0xC)); }
NVN(nvnTextureBuilderGetWidth) { RET(TB(uint32_t, 0x10)); }
NVN(nvnTextureBuilderGetHeight) { RET(TB(uint32_t, 0x14)); }
NVN(nvnTextureBuilderGetDepth) { RET(TB(uint32_t, 0x18)); }
NVN(nvnTextureBuilderGetLevels) { RET(TB(uint32_t, 0x1C)); }
NVN(nvnTextureBuilderGetFormat) { RET(TB(uint32_t, 0x20)); }
NVN(nvnTextureBuilderGetSamples) { RET(TB(uint32_t, 0x24)); }
NVN(nvnTextureBuilderGetSwizzle) {
    for (int i = 0; i < 4; i++) *(uint32_t *)AP(1 + i) = TB(uint8_t, 0x28 + i);
}
NVN(nvnTextureBuilderGetDepthStencilMode) { RET(TB(uint32_t, 0x2C)); }
NVN(nvnTextureBuilderGetStride) { RET(TB(uint64_t, 0x30)); }
NVN(nvnTextureBuilderGetMemoryPool) { RET(TB(uint64_t, 0x38)); }
NVN(nvnTextureBuilderGetMemoryOffset) { RET(TB(uint64_t, 0x40)); }
NVN(nvnTextureBuilderGetPackagedTextureData) { RET(TB(uint64_t, 0x48)); }
NVN(nvnTextureBuilderGetPackagedTextureLayout) { RET(0); }
NVN(nvnTextureBuilderGetGLTextureName) { RET(TB(uint32_t, 0x58)); }
NVN(nvnTextureBuilderGetSparseTileLayout) { RET(0); }
NVN(nvnTextureBuilderGetZCullStorageSize) { RET(0); }
NVN(nvnTextureBuilderGetStorageClass) { RET(0); }
NVN(nvnTextureBuilderGetRawStorageClass) { RET(0); }

static bool target_is_array(uint32_t t) { return t == 3 || t == 4 || t == 6 || t == 8 || t == 9; }

static void builder_layout(uint64_t b, TexLayout *t) {
    uint32_t target = G(uint32_t, b, 0xC);
    t->fmt = nvn_format_info(G(uint32_t, b, 0x20));
    t->width = G(uint32_t, b, 0x10);
    t->height = target == 0 || target == 3 ? 1 : G(uint32_t, b, 0x14);
    if (target == 3) t->layers = G(uint32_t, b, 0x14);  // 1D array: height is the layer count
    t->depth = target == 2 ? G(uint32_t, b, 0x18) : 1;
    if (target_is_array(target) && target != 3) t->layers = G(uint32_t, b, 0x18);
    if (target == 8) t->layers = 6;
    if (!t->layers) t->layers = 1;
    t->levels = G(uint32_t, b, 0x1C) ? G(uint32_t, b, 0x1C) : 1;
    tegra_layout_init(t);
    // Packaged textures were swizzled offline with their own block height: NVNpackagedTextureLayout's low word is
    // log2(GOBs per block).
    uint64_t pl = G(uint64_t, b, 0x50);
    if (pl && (uint32_t)pl <= 5) t->block_height_log2 = (uint32_t)pl;
}

static bool is_linear(uint64_t b) { return (G(uint32_t, b, 8) & 0x10) || G(uint32_t, b, 0xC) == 10; }

static uint64_t builder_storage_size(uint64_t b) {
    if (G(uint32_t, b, 0xC) == 10)  // buffer texture
        return (uint64_t)G(uint32_t, b, 0x10) * nvn_format_info(G(uint32_t, b, 0x20))->bpb;
    if (is_linear(b)) return G(uint64_t, b, 0x30) * G(uint32_t, b, 0x14);
    TexLayout t = {0};
    builder_layout(b, &t);
    return tegra_total_size(&t);
}
NVN(nvnTextureBuilderGetStorageSize) { RET(builder_storage_size(A(0))); }
NVN(nvnTextureBuilderGetStorageAlignment) { RET(is_linear(A(0)) ? 32 : 512); }

static NvnTexture *texture_from_builder(uint64_t b) {
    NvnTexture *t = calloc(1, sizeof *t);
    t->target = G(uint32_t, b, 0xC);
    t->flags = G(uint32_t, b, 8);
    t->width = G(uint32_t, b, 0x10);
    t->height = G(uint32_t, b, 0x14);
    t->depth = G(uint32_t, b, 0x18);
    t->levels = G(uint32_t, b, 0x1C) ? G(uint32_t, b, 0x1C) : 1;
    t->format = G(uint32_t, b, 0x20);
    t->samples = G(uint32_t, b, 0x24);
    memcpy(t->swizzle, &G(uint8_t, b, 0x28), 4);
    t->ds_mode = G(uint32_t, b, 0x2C);
    t->stride = G(uint64_t, b, 0x30);
    uint64_t pool = G(uint64_t, b, 0x38);
    t->pool = pool ? HOST(NvnPool, pool) : NULL;
    t->pool_offset = G(uint64_t, b, 0x40);
    t->addr = t->pool ? t->pool->storage + t->pool_offset : 0;
    builder_layout(b, &t->layout);
    t->debug_id = next_debug_id++;
    return t;
}

_Thread_local bool g_nvn_making_screenshot;

NVN(nvnTextureInitialize) {
    NvnTexture *t = texture_from_builder(A(1));
    t->screenshot = g_nvn_making_screenshot;
    SET_HOST(A(0), t);
    if (ENV("MHGU_TEX_LOG"))  // (debug) every texture made: format, size, flags, storage, layout
        LOG("[tex] init fmt %#x %ux%ux%u target %u flags %#x levels %u addr %llx bh %u stride %llu\n", t->format, t->width,
            t->height, t->depth, t->target, t->flags, t->levels, (unsigned long long)t->addr, t->layout.block_height_log2,
            (unsigned long long)t->stride);
    RET(1);
}
NVN(nvnTextureFinalize) {
    NvnTexture *t = HOST(NvnTexture, A(0));
    if (t) { backend_texture_destroyed(t); free(t); }
    SET_HOST(A(0), 0);
}
NVN(nvnTextureSetDebugLabel) {
    NvnTexture *t = HOST(NvnTexture, A(0));
    if (t && A(1)) strncpy(t->label, (const char *)AP(1), sizeof t->label - 1);
}
#define TEX HOST(NvnTexture, A(0))
NVN(nvnTextureGetFlags) { RET(TEX->flags); }
NVN(nvnTextureGetTarget) { RET(TEX->target); }
NVN(nvnTextureGetWidth) { RET(TEX->width); }
NVN(nvnTextureGetHeight) { RET(TEX->height); }
NVN(nvnTextureGetDepth) { RET(TEX->depth); }
NVN(nvnTextureGetLevels) { RET(TEX->levels); }
NVN(nvnTextureGetFormat) { RET(TEX->format); }
NVN(nvnTextureGetSamples) { RET(TEX->samples); }
NVN(nvnTextureGetSwizzle) { for (int i = 0; i < 4; i++) *(uint32_t *)AP(1 + i) = TEX->swizzle[i]; }
NVN(nvnTextureGetDepthStencilMode) { RET(TEX->ds_mode); }
NVN(nvnTextureGetStride) { RET(TEX->stride); }
NVN(nvnTextureGetTextureAddress) { RET64(TEX->addr); }
NVN(nvnTextureGetMemoryPool) { RET(0); }
NVN(nvnTextureGetMemoryOffset) { RET(TEX->pool_offset); }
NVN(nvnTextureGetStorageSize) { RET(tegra_total_size(&TEX->layout)); }
NVN(nvnTextureGetDebugID) { RET64(TEX->debug_id); }
NVN(nvnTextureGetViewOffset) { RET(0); }
NVN(nvnTextureGetStorageClass) { RET(0); }
NVN(nvnTextureGetRawStorageClass) { RET(0); }
NVN(nvnTextureGetSparseTileLayout) { RET(0); }
NVN(nvnTextureGetZCullStorageSize) { RET(0); }
NVN(nvnTextureCompare) { RET(A(0) == A(1)); }
NVN(nvnTextureFlushTexels) { (void)c; }
NVN(nvnTextureInvalidateTexels) { (void)c; }

// Texture views (our layout of the 0x28-byte guest struct)
#define TV(T, off) G(T, A(0), off)
NVN(nvnTextureViewSetDefaults) { memset(AP(0), 0, 0x28); }
NVN(nvnTextureViewSetLevels) { TV(uint32_t, 0) |= 1; TV(uint16_t, 4) = (uint16_t)A(1); TV(uint16_t, 6) = (uint16_t)A(2); }
NVN(nvnTextureViewSetLayers) { TV(uint32_t, 0) |= 2; TV(uint16_t, 8) = (uint16_t)A(1); TV(uint16_t, 10) = (uint16_t)A(2); }
NVN(nvnTextureViewSetFormat) { TV(uint32_t, 0) |= 4; TV(uint32_t, 12) = (uint32_t)A(1); }
NVN(nvnTextureViewSetSwizzle) {
    NvnTextureView *v = (NvnTextureView *)AP(0);
    v->setmask |= 8;
    v->swizzle[0] = (uint8_t)A(1); v->swizzle[1] = (uint8_t)A(2); v->swizzle[2] = (uint8_t)A(3); v->swizzle[3] = (uint8_t)A(4);
}
NVN(nvnTextureViewSetDepthStencilMode) { NvnTextureView *v = (NvnTextureView *)AP(0); v->setmask |= 16; v->ds_mode = (uint32_t)A(1); }
NVN(nvnTextureViewSetTarget) { NvnTextureView *v = (NvnTextureView *)AP(0); v->setmask |= 32; v->target = (uint32_t)A(1); }
NVN(nvnTextureViewGetLevels) { *(int32_t *)AP(1) = TV(uint16_t, 4); *(int32_t *)AP(2) = TV(uint16_t, 6); RET(TV(uint32_t, 0) & 1); }
NVN(nvnTextureViewGetLayers) { *(int32_t *)AP(1) = TV(uint16_t, 8); *(int32_t *)AP(2) = TV(uint16_t, 10); RET(!!(TV(uint32_t, 0) & 2)); }
NVN(nvnTextureViewGetFormat) { *(uint32_t *)AP(1) = TV(uint32_t, 12); RET(!!(TV(uint32_t, 0) & 4)); }
NVN(nvnTextureViewGetSwizzle) {
    NvnTextureView *v = (NvnTextureView *)AP(0);
    for (int i = 0; i < 4; i++) *(uint32_t *)AP(1 + i) = v->swizzle[i];
    RET(!!(v->setmask & 8));
}
NVN(nvnTextureViewGetDepthStencilMode) { NvnTextureView *v = (NvnTextureView *)AP(0); *(uint32_t *)AP(1) = v->ds_mode; RET(!!(v->setmask & 16)); }
NVN(nvnTextureViewGetTarget) { NvnTextureView *v = (NvnTextureView *)AP(0); *(uint32_t *)AP(1) = v->target; RET(!!(v->setmask & 32)); }
NVN(nvnTextureViewCompare) { RET(memcmp(AP(0), AP(1), 0x1C) == 0); }

NVN(nvnTextureWriteTexels) {  // (texture, view, region, data): linear data into block-linear storage
    NvnTexture *t = HOST(NvnTexture, A(0));
    const int32_t *r = (const int32_t *)AP(2);  // xoffset, yoffset, zoffset, width, height, depth
    LevelLayout l = tegra_level(&t->layout, 0);
    if (A(2) && r[0] == 0 && r[1] == 0 && (uint32_t)r[3] == t->width && (uint32_t)r[4] == t->height) {
        tegra_swizzle(AP(3), (uint8_t *)G2H((uint32_t)t->addr) + l.offset, l.wb, l.hb, r[5] > 0 ? (uint32_t)r[5] : 1,
                      t->layout.fmt->bpb, l.block_height_log2, l.block_depth_log2);
        backend_pool_written(t->addr, tegra_total_size(&t->layout));
    } else {
        LOG("[nvn] partial WriteTexels not implemented\n");
    }
}
NVN(nvnTextureWriteTexelsStrided) { LOG("[nvn] WriteTexelsStrided not implemented\n"); (void)c; }
// Texels out of the texture's guest storage (what the CPU put there, or the game's own data): the character creation
// colour bars are read this way (the cursor's colour becomes the skin / hair colour), and a no-op left them black.
// Region: xoffset, yoffset, zoffset, width, height, depth. Output rows are packed unless a stride is given.
static void read_texels(NvnTexture *t, uint64_t view, uint64_t region, uint8_t *dst, uint64_t row_stride, uint64_t img_stride) {
    if (!t || !region || !dst || !t->addr) return;
    const FormatInfo *f = t->layout.fmt;
    if (!f || f->bw != 1 || f->bh != 1) { LOG("[nvn] ReadTexels of a compressed format not implemented\n"); return; }
    const int32_t *r = (const int32_t *)G2H((uint32_t)region);
    uint32_t level = (view && (G(uint32_t, view, 0) & 1)) ? G(uint16_t, view, 4) : 0;
    LevelLayout l = tegra_level(&t->layout, level);
    uint32_t x0 = (uint32_t)r[0], y0 = (uint32_t)r[1], z0 = (uint32_t)r[2];
    uint32_t w = (uint32_t)r[3], h = (uint32_t)r[4], d = r[5] > 0 ? (uint32_t)r[5] : 1;
    if (x0 + w > l.w || y0 + h > l.h || z0 + d > (l.d ? l.d : 1)) { LOG("[nvn] ReadTexels region outside the level\n"); return; }
    uint32_t bpb = f->bpb;
    if (!row_stride) row_stride = (uint64_t)w * bpb;
    if (!img_stride) img_stride = row_stride * h;
    const uint8_t *src = (const uint8_t *)G2H((uint32_t)t->addr) + l.offset;
    uint64_t row_bytes = (uint64_t)l.wb * bpb;
    uint8_t *lin = NULL;
    if (!t->stride) {  // block-linear: unswizzle the level, then cut the region out
        lin = malloc(row_bytes * l.hb * (l.d ? l.d : 1));
        if (!lin) return;
        tegra_deswizzle(src, lin, l.wb, l.hb, l.d ? l.d : 1, bpb, l.block_height_log2, l.block_depth_log2);
        src = lin;
    } else {
        row_bytes = t->stride;
    }
    for (uint32_t z = 0; z < d; z++)
        for (uint32_t y = 0; y < h; y++)
            memcpy(dst + z * img_stride + y * row_stride,
                   src + ((uint64_t)(z0 + z) * l.hb + y0 + y) * row_bytes + (uint64_t)x0 * bpb, (uint64_t)w * bpb);
    free(lin);
}
NVN(nvnTextureReadTexels) { read_texels(HOST(NvnTexture, A(0)), A(1), A(2), AP(3), 0, 0); }
NVN(nvnTextureReadTexelsStrided) { read_texels(HOST(NvnTexture, A(0)), A(1), A(2), AP(3), A(4), A(5)); }

// ================================================================== samplers
// SamplerBuilder: device@0 then an NvnSamplerDesc at 8
#define SD ((NvnSamplerDesc *)(AP(0) + 8))
NVN(nvnSamplerBuilderSetDevice) { G(uint64_t, A(0), 0) = A(1); }
NVN(nvnSamplerBuilderSetDefaults) {
    NvnSamplerDesc *d = SD;
    memset(d, 0, sizeof *d);
    d->wrap[0] = d->wrap[1] = d->wrap[2] = 1;  // REPEAT
    d->lod_max = 1000.0f;
    d->max_aniso = 1.0f;
    d->compare_func = 4;
}
NVN(nvnSamplerBuilderSetMinMagFilter) { SD->min_filter = (uint32_t)A(1); SD->mag_filter = (uint32_t)A(2); }
NVN(nvnSamplerBuilderSetWrapMode) { SD->wrap[0] = (uint32_t)A(1); SD->wrap[1] = (uint32_t)A(2); SD->wrap[2] = (uint32_t)A(3); }
NVNS(nvnSamplerBuilderSetLodClamp, "iff") { SD->lod_min = AF(0); SD->lod_max = AF(1); }
NVNS(nvnSamplerBuilderSetLodBias, "if") { SD->lod_bias = AF(0); }
NVN(nvnSamplerBuilderSetCompare) { SD->compare_mode = (uint32_t)A(1); SD->compare_func = (uint32_t)A(2); }
NVN(nvnSamplerBuilderSetBorderColor) { memcpy(SD->border, AP(1), 16); }
NVN(nvnSamplerBuilderSetBorderColori) { memcpy(SD->border, AP(1), 16); }
NVN(nvnSamplerBuilderSetBorderColorui) { memcpy(SD->border, AP(1), 16); }
NVNS(nvnSamplerBuilderSetMaxAnisotropy, "if") { SD->max_aniso = AF(0); }
NVN(nvnSamplerBuilderSetReductionFilter) { SD->reduction = (uint32_t)A(1); }
NVNS(nvnSamplerBuilderSetLodSnap, "if") { (void)c; (void)na_; }
NVN(nvnSamplerBuilderGetDevice) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnSamplerBuilderGetMinMagFilter) { *(uint32_t *)AP(1) = SD->min_filter; *(uint32_t *)AP(2) = SD->mag_filter; }
NVN(nvnSamplerBuilderGetWrapMode) { for (int i = 0; i < 3; i++) *(uint32_t *)AP(1 + i) = SD->wrap[i]; }
NVN(nvnSamplerBuilderGetLodClamp) { *(float *)AP(1) = SD->lod_min; *(float *)AP(2) = SD->lod_max; }
NVN(nvnSamplerBuilderGetLodBias) { RETF(SD->lod_bias); }
NVN(nvnSamplerBuilderGetCompare) { *(uint32_t *)AP(1) = SD->compare_mode; *(uint32_t *)AP(2) = SD->compare_func; }
NVN(nvnSamplerBuilderGetBorderColor) { memcpy(AP(1), SD->border, 16); }
NVN(nvnSamplerBuilderGetBorderColori) { memcpy(AP(1), SD->border, 16); }
NVN(nvnSamplerBuilderGetBorderColorui) { memcpy(AP(1), SD->border, 16); }
NVN(nvnSamplerBuilderGetMaxAnisotropy) { RETF(SD->max_aniso); }
NVN(nvnSamplerBuilderGetReductionFilter) { RET(SD->reduction); }
NVN(nvnSamplerBuilderGetLodSnap) { RETF(0); }

NVN(nvnSamplerInitialize) {
    NvnSamplerDesc *d = malloc(sizeof *d);
    *d = *(NvnSamplerDesc *)(AP(1) + 8);
    d->backend = NULL;
    SET_HOST(A(0), d);
    RET(1);
}
NVN(nvnSamplerFinalize) { free(HOST(NvnSamplerDesc, A(0))); SET_HOST(A(0), 0); }
NVN(nvnSamplerSetDebugLabel) { (void)c; }
#define SMP HOST(NvnSamplerDesc, A(0))
NVN(nvnSamplerGetMinMagFilter) { *(uint32_t *)AP(1) = SMP->min_filter; *(uint32_t *)AP(2) = SMP->mag_filter; }
NVN(nvnSamplerGetWrapMode) { for (int i = 0; i < 3; i++) *(uint32_t *)AP(1 + i) = SMP->wrap[i]; }
NVN(nvnSamplerGetLodClamp) { *(float *)AP(1) = SMP->lod_min; *(float *)AP(2) = SMP->lod_max; }
NVN(nvnSamplerGetLodBias) { RETF(SMP->lod_bias); }
NVN(nvnSamplerGetCompare) { *(uint32_t *)AP(1) = SMP->compare_mode; *(uint32_t *)AP(2) = SMP->compare_func; }
NVN(nvnSamplerGetBorderColor) { memcpy(AP(1), SMP->border, 16); }
NVN(nvnSamplerGetBorderColori) { memcpy(AP(1), SMP->border, 16); }
NVN(nvnSamplerGetBorderColorui) { memcpy(AP(1), SMP->border, 16); }
NVN(nvnSamplerGetMaxAnisotropy) { RETF(SMP->max_aniso); }
NVN(nvnSamplerGetReductionFilter) { RET(SMP->reduction); }
NVN(nvnSamplerCompare) {
    RET(memcmp(HOST(NvnSamplerDesc, A(0)), HOST(NvnSamplerDesc, A(1)), offsetof(NvnSamplerDesc, backend)) == 0);
}
NVN(nvnSamplerGetDebugID) { RET64(A(0)); }

// ================================================================== descriptor pools
NvnDescPool *g_texture_pools[64], *g_sampler_pools[64];

static NvnDescPool *desc_pool_init(uint64_t guest, uint64_t pool, uint64_t off, int count, size_t entry, NvnDescPool **reg) {
    NvnDescPool *d = calloc(1, sizeof *d);
    d->addr = HOST(NvnPool, pool)->storage + off;
    d->count = count;
    d->entries = calloc(count, entry);
    SET_HOST(guest, d);
    G(uint64_t, guest, 8) = pool;
    G(uint64_t, guest, 16) = off;
    G(uint32_t, guest, 24) = (uint32_t)count;
    for (int i = 0; i < 64; i++) if (!reg[i]) { reg[i] = d; break; }
    return d;
}

NVN(nvnTexturePoolInitialize) {  // (pool, const NVNmemoryPool *, ptrdiff_t offset, int count)
    desc_pool_init(A(0), A(1), A(2), (int)A(3), sizeof(NvnTexEntry), g_texture_pools);
    RET(1);
}
NVN(nvnTexturePoolFinalize) { (void)c; }
NVN(nvnTexturePoolSetDebugLabel) { (void)c; }
NVN(nvnTexturePoolGetMemoryPool) { RET(G(uint64_t, A(0), 8)); }
NVN(nvnTexturePoolGetMemoryOffset) { RET(G(uint64_t, A(0), 16)); }
NVN(nvnTexturePoolGetSize) { RET(G(uint32_t, A(0), 24)); }
static void register_texture(NvnArgs *na_) {
    NvnDescPool *d = HOST(NvnDescPool, A(0));
    int id = (int)A(1);
    if (id < 0 || id >= d->count) return;
    NvnTexEntry *e = &((NvnTexEntry *)d->entries)[id];
    e->tex = HOST(NvnTexture, A(2));
    e->has_view = A(3) != 0;
    if (e->has_view) memcpy(&e->view, AP(3), sizeof e->view);
}
NVN(nvnTexturePoolRegisterTexture) { (void)c; register_texture(na_); }
NVN(nvnTexturePoolRegisterImage) { (void)c; register_texture(na_); }

NVN(nvnSamplerPoolInitialize) {
    desc_pool_init(A(0), A(1), A(2), (int)A(3), sizeof(NvnSamplerDesc), g_sampler_pools);
    RET(1);
}
NVN(nvnSamplerPoolFinalize) { (void)c; }
NVN(nvnSamplerPoolSetDebugLabel) { (void)c; }
NVN(nvnSamplerPoolGetMemoryPool) { RET(G(uint64_t, A(0), 8)); }
NVN(nvnSamplerPoolGetMemoryOffset) { RET(G(uint64_t, A(0), 16)); }
NVN(nvnSamplerPoolGetSize) { RET(G(uint32_t, A(0), 24)); }
NVN(nvnSamplerPoolRegisterSampler) {
    NvnDescPool *d = HOST(NvnDescPool, A(0));
    int id = (int)A(1);
    if (id >= 0 && id < d->count) ((NvnSamplerDesc *)d->entries)[id] = *HOST(NvnSamplerDesc, A(2));
}
NVN(nvnSamplerPoolRegisterSamplerBuilder) {
    NvnDescPool *d = HOST(NvnDescPool, A(0));
    int id = (int)A(1);
    if (id >= 0 && id < d->count) {
        NvnSamplerDesc *e = &((NvnSamplerDesc *)d->entries)[id];
        *e = *(NvnSamplerDesc *)(AP(2) + 8);
        e->backend = NULL;
    }
}

// ================================================================== programs
NVN(nvnProgramInitialize) { SET_HOST(A(0), calloc(1, sizeof(NvnProgram))); RET(1); }
NVN(nvnProgramFinalize) { free(HOST(NvnProgram, A(0))); SET_HOST(A(0), 0); }
NVN(nvnProgramSetDebugLabel) { (void)c; }
NVN(nvnProgramSetSampleShading) { (void)c; }
NVN(nvnProgramSetSubroutineLinkage) { RET(1); }
// NVNshaderData[n] = {u64 data (GPU address), const void *control (32-bit pointer, then 4 bytes padding)}
static void set_shaders(Cpu *c, NvnArgs *na_, int stride) {
    NvnProgram *p = HOST(NvnProgram, A(0));
    int n = (int)A(1);
    uint32_t data = (uint32_t)A(2);
    memset(p->stages, 0, sizeof p->stages);
    p->stage_mask = 0;
    p->backend = NULL;
    for (int i = 0; i < n; i++) {
        uint64_t code = LD64(data + (uint32_t)(i * stride)) & 0xFFFFFFFFull;
        const uint8_t *control = (const uint8_t *)G2H(LD32(data + (uint32_t)(i * stride) + 8));
        // stage from the Maxwell shader program header (after the 0x30-byte NVN header)
        uint32_t sph0 = LD32(code + 0x30);
        uint32_t stage;
        switch ((sph0 >> 10) & 0xF) {
        case 1: stage = 0; break;  // vertex
        case 5: stage = 1; break;  // fragment
        case 4: stage = 2; break;  // geometry
        case 2: stage = 3; break;  // tess control
        case 3: stage = 4; break;  // tess eval
        default: stage = 5; break; // compute (no SPH)
        }
        if (LD32(code) != 0x12345678) stage = 5;
        p->stages[stage].stage = stage;
        p->stages[stage].code_addr = code;
        p->stages[stage].control = control;
        p->stages[stage].const_offset = *(const uint32_t *)(control + 0x700);
        p->stages[stage].const_size = *(const uint32_t *)(control + 0x6FC);
        p->stage_mask |= 1u << stage;
    }
    backend_program_created(p);
    RET(1);
}
NVN(nvnProgramSetShaders) { set_shaders(c, na_, 16); }     // NVNshaderData {data, control}
NVN(nvnProgramSetShadersExt) { set_shaders(c, na_, 24); }  // NVNshaderDataExt {data, control, debug hash}

// ================================================================== window
// WindowBuilder: device@0 native@8 num@0x10 textures@0x18 interval@0x20 active@0x24
NVN(nvnWindowBuilderSetDevice) { G(uint64_t, A(0), 0) = A(1); }
NVN(nvnWindowBuilderSetDefaults) { memset(AP(0) + 8, 0, 0x20); G(uint32_t, A(0), 0x20) = 1; }
NVN(nvnWindowBuilderSetNativeWindow) { G(uint64_t, A(0), 8) = A(1); }
NVN(nvnWindowBuilderSetTextures) { G(uint32_t, A(0), 0x10) = (uint32_t)A(1); G(uint64_t, A(0), 0x18) = A(2); }
NVN(nvnWindowBuilderSetPresentInterval) { G(uint32_t, A(0), 0x20) = (uint32_t)A(1); }
NVN(nvnWindowBuilderSetNumActiveTextures) { G(uint32_t, A(0), 0x24) = (uint32_t)A(1); }
NVN(nvnWindowBuilderGetDevice) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnWindowBuilderGetNativeWindow) { RET(G(uint64_t, A(0), 8)); }
NVN(nvnWindowBuilderGetNumTextures) { RET(G(uint32_t, A(0), 0x10)); }
NVN(nvnWindowBuilderGetTexture) {  // (an array of 32-bit NVNtexture pointers)
    int i = (int)A(1);
    RET(i < (int)G(uint32_t, A(0), 0x10) ? LD32((uint32_t)G(uint64_t, A(0), 0x18) + 4u * (uint32_t)i) : 0);
}
NVN(nvnWindowBuilderGetPresentInterval) { RET(G(uint32_t, A(0), 0x20)); }
NVN(nvnWindowBuilderGetNumActiveTextures) { RET(G(uint32_t, A(0), 0x24)); }

NVN(nvnWindowInitialize) {
    NvnWindow *w = calloc(1, sizeof *w);
    w->num = (int)G(uint32_t, A(1), 0x10);
    if (w->num > 8) w->num = 8;
    uint32_t arr = (uint32_t)G(uint64_t, A(1), 0x18);
    for (int i = 0; i < w->num; i++) w->textures[i] = LD32(arr + 4u * (uint32_t)i);
    w->interval = (int)G(uint32_t, A(1), 0x20);
    w->active = G(uint32_t, A(1), 0x24) ? (int)G(uint32_t, A(1), 0x24) : w->num;
    SET_HOST(A(0), w);
    LOG("[nvn] window with %d textures\n", w->num);
    RET(1);
}
NVN(nvnWindowFinalize) { (void)c; }
NVN(nvnWindowSetDebugLabel) { (void)c; }
#define WIN HOST(NvnWindow, A(0))
NVN(nvnWindowGetNativeWindow) { RET(0); }
NVN(nvnWindowGetNumTextures) { RET(WIN->num); }
NVN(nvnWindowGetNumActiveTextures) { RET(WIN->active); }
NVN(nvnWindowSetNumActiveTextures) { WIN->active = (int)A(1); }
NVN(nvnWindowGetPresentInterval) { RET(WIN->interval); }
NVN(nvnWindowSetPresentInterval) { WIN->interval = (int)A(1); }
NVN(nvnWindowSetCrop) { int *cr = WIN->crop; cr[0] = (int)A(1); cr[1] = (int)A(2); cr[2] = (int)A(3); cr[3] = (int)A(4); }
NVN(nvnWindowGetCrop) { memcpy(AP(1), WIN->crop, 16); }
NVN(nvnWindowAcquireTexture) {  // (window, sync, int *index)
    NvnWindow *w = WIN;
    if (A(1)) sync_of(A(1))->signaled = 1;
    *(int32_t *)AP(2) = w->next;
    w->next = (w->next + 1) % (w->active ? w->active : 1);
    RET(0);  // NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS
}
NVN(nvnQueueAcquireTexture) {
    NvnWindow *w = HOST(NvnWindow, A(1));
    *(int32_t *)AP(2) = w->next;
    w->next = (w->next + 1) % (w->active ? w->active : 1);
    RET(0);
}
NVN(nvnQueuePresentTexture) {
    NvnWindow *w = HOST(NvnWindow, A(1));
    int i = (int)A(2);
    if (i >= 0 && i < w->num) backend_present(HOST(NvnTexture, w->textures[i]));
}

// ================================================================== small state objects
// BlendState (8): target, srcC, dstC, srcA, dstA, eqC, eqA, advanced
NVN(nvnBlendStateSetDefaults) {
    uint8_t *b = AP(0);
    b[0] = 0; b[1] = 2; b[2] = 1; b[3] = 2; b[4] = 1; b[5] = 1; b[6] = 1; b[7] = 0;
}
NVN(nvnBlendStateSetBlendTarget) { G(uint8_t, A(0), 0) = (uint8_t)A(1); }
NVN(nvnBlendStateSetBlendFunc) {
    uint8_t *b = AP(0);
    b[1] = (uint8_t)A(1); b[2] = (uint8_t)A(2); b[3] = (uint8_t)A(3); b[4] = (uint8_t)A(4);
}
NVN(nvnBlendStateSetBlendEquation) { G(uint8_t, A(0), 5) = (uint8_t)A(1); G(uint8_t, A(0), 6) = (uint8_t)A(2); }
NVN(nvnBlendStateSetAdvancedMode) { G(uint8_t, A(0), 7) = (uint8_t)A(1); }
NVN(nvnBlendStateSetAdvancedOverlap) { (void)c; }
NVN(nvnBlendStateSetAdvancedPremultipliedSrc) { (void)c; }
NVN(nvnBlendStateSetAdvancedNormalizedDst) { (void)c; }
NVN(nvnBlendStateGetBlendTarget) { RET(G(uint8_t, A(0), 0)); }
NVN(nvnBlendStateGetBlendFunc) {
    uint8_t *b = AP(0);
    for (int i = 0; i < 4; i++) *(uint32_t *)AP(1 + i) = b[1 + i];
}
NVN(nvnBlendStateGetBlendEquation) { *(uint32_t *)AP(1) = G(uint8_t, A(0), 5); *(uint32_t *)AP(2) = G(uint8_t, A(0), 6); }
NVN(nvnBlendStateGetAdvancedMode) { RET(G(uint8_t, A(0), 7)); }
NVN(nvnBlendStateGetAdvancedOverlap) { RET(0); }
NVN(nvnBlendStateGetAdvancedPremultipliedSrc) { RET(1); }
NVN(nvnBlendStateGetAdvancedNormalizedDst) { RET(1); }

// ChannelMaskState (4): 4 bits per target
NVN(nvnChannelMaskStateSetDefaults) { G(uint32_t, A(0), 0) = 0xFFFFFFFF; }
NVN(nvnChannelMaskStateSetChannelMask) {
    int t = (int)A(1);
    uint32_t m = (A(2) ? 1 : 0) | (A(3) ? 2 : 0) | (A(4) ? 4 : 0) | (A(5) ? 8 : 0);
    G(uint32_t, A(0), 0) = (G(uint32_t, A(0), 0) & ~(0xFu << (t * 4))) | (m << (t * 4));
}
NVN(nvnChannelMaskStateGetChannelMask) {
    uint32_t m = G(uint32_t, A(0), 0) >> ((int)A(1) * 4);
    for (int i = 0; i < 4; i++) *(uint8_t *)AP(2 + i) = (m >> i) & 1;
}

// ColorState (4): blend enable mask, logic op, alpha test
NVN(nvnColorStateSetDefaults) { G(uint8_t, A(0), 0) = 0; G(uint8_t, A(0), 1) = 3; G(uint8_t, A(0), 2) = 8; }
NVN(nvnColorStateSetBlendEnable) {
    uint8_t m = G(uint8_t, A(0), 0), bt = (uint8_t)(1u << A(1));
    G(uint8_t, A(0), 0) = A(2) ? (m | bt) : (m & ~bt);
}
NVN(nvnColorStateSetLogicOp) { G(uint8_t, A(0), 1) = (uint8_t)A(1); }
NVN(nvnColorStateSetAlphaTest) { G(uint8_t, A(0), 2) = (uint8_t)A(1); }
NVN(nvnColorStateGetBlendEnable) { RET((G(uint8_t, A(0), 0) >> A(1)) & 1); }
NVN(nvnColorStateGetLogicOp) { RET(G(uint8_t, A(0), 1)); }
NVN(nvnColorStateGetAlphaTest) { RET(G(uint8_t, A(0), 2)); }

// DepthStencilState (8): [0]=depthTest [1]=depthWrite [2]=depthFunc [3]=stencilTest
//   [4]=front func|back func<<4 [5..7]=ops (front fail,zfail,zpass / back) in 4-bit fields
NVN(nvnDepthStencilStateSetDefaults) {
    uint8_t *d = AP(0);
    d[0] = 0; d[1] = 1; d[2] = 2; d[3] = 0; d[4] = 0x88;
    d[5] = 0x11; d[6] = 0x11; d[7] = 0x11;
}
NVN(nvnDepthStencilStateSetDepthTestEnable) { G(uint8_t, A(0), 0) = (uint8_t)A(1); }
NVN(nvnDepthStencilStateSetDepthWriteEnable) { G(uint8_t, A(0), 1) = (uint8_t)A(1); }
NVN(nvnDepthStencilStateSetDepthFunc) { G(uint8_t, A(0), 2) = (uint8_t)A(1); }
NVN(nvnDepthStencilStateSetStencilTestEnable) { G(uint8_t, A(0), 3) = (uint8_t)A(1); }
NVN(nvnDepthStencilStateSetStencilFunc) {
    uint8_t *d = AP(0);
    int face = (int)A(1);  // 1 front, 2 back, 3 both
    if (face & 1) d[4] = (uint8_t)((d[4] & 0xF0) | (A(2) & 0xF));
    if (face & 2) d[4] = (uint8_t)((d[4] & 0x0F) | ((A(2) & 0xF) << 4));
}
NVN(nvnDepthStencilStateSetStencilOp) {
    uint8_t *d = AP(0);
    int face = (int)A(1);
    uint32_t ops = (uint32_t)((A(2) & 0xF) | (A(3) & 0xF) << 4 | (A(4) & 0xF) << 8);
    uint32_t packed = d[5] | d[6] << 8 | d[7] << 16;
    if (face & 1) packed = (packed & ~0xFFFu) | ops;
    if (face & 2) packed = (packed & 0xFFF) | ops << 12;
    d[5] = (uint8_t)packed; d[6] = (uint8_t)(packed >> 8); d[7] = (uint8_t)(packed >> 16);
}
NVN(nvnDepthStencilStateGetDepthTestEnable) { RET(G(uint8_t, A(0), 0)); }
NVN(nvnDepthStencilStateGetDepthWriteEnable) { RET(G(uint8_t, A(0), 1)); }
NVN(nvnDepthStencilStateGetDepthFunc) { RET(G(uint8_t, A(0), 2)); }
NVN(nvnDepthStencilStateGetStencilTestEnable) { RET(G(uint8_t, A(0), 3)); }
NVN(nvnDepthStencilStateGetStencilFunc) { RET((A(1) & 2) ? G(uint8_t, A(0), 4) >> 4 : G(uint8_t, A(0), 4) & 0xF); }
NVN(nvnDepthStencilStateGetStencilOp) {
    uint8_t *d = AP(0);
    uint32_t packed = d[5] | d[6] << 8 | d[7] << 16;
    if (A(1) & 2) packed >>= 12;
    for (int i = 0; i < 3; i++) *(uint32_t *)AP(2 + i) = (packed >> (4 * i)) & 0xF;
}

// MultisampleState (0x18): enable@0 samples@4 raster@8 a2c@0xC a2cdither@0xD cov2color@0xE output@0xF
NVN(nvnMultisampleStateSetDefaults) { memset(AP(0), 0, 0x18); G(uint8_t, A(0), 0) = 1; G(uint8_t, A(0), 0xD) = 1; }
NVN(nvnMultisampleStateSetMultisampleEnable) { G(uint8_t, A(0), 0) = (uint8_t)A(1); }
NVN(nvnMultisampleStateSetSamples) { G(uint32_t, A(0), 4) = (uint32_t)A(1); }
NVN(nvnMultisampleStateSetRasterSamples) { G(uint32_t, A(0), 8) = (uint32_t)A(1); }
NVN(nvnMultisampleStateSetAlphaToCoverageEnable) { G(uint8_t, A(0), 0xC) = (uint8_t)A(1); }
NVN(nvnMultisampleStateSetAlphaToCoverageDither) { G(uint8_t, A(0), 0xD) = (uint8_t)A(1); }
NVN(nvnMultisampleStateSetCoverageToColorEnable) { G(uint8_t, A(0), 0xE) = (uint8_t)A(1); }
NVN(nvnMultisampleStateSetCoverageToColorOutput) { G(uint8_t, A(0), 0xF) = (uint8_t)A(1); }
NVN(nvnMultisampleStateSetCoverageModulationMode) { G(uint8_t, A(0), 0x10) = (uint8_t)A(1); }
NVN(nvnMultisampleStateSetSampleLocations) { (void)c; }
NVN(nvnMultisampleStateSetSampleLocationsEnable) { (void)c; }
NVN(nvnMultisampleStateSetSampleLocationsGridEnable) { (void)c; }
NVN(nvnMultisampleStateGetMultisampleEnable) { RET(G(uint8_t, A(0), 0)); }
NVN(nvnMultisampleStateGetSamples) { RET(G(uint32_t, A(0), 4)); }
NVN(nvnMultisampleStateGetRasterSamples) { RET(G(uint32_t, A(0), 8)); }
NVN(nvnMultisampleStateGetAlphaToCoverageEnable) { RET(G(uint8_t, A(0), 0xC)); }
NVN(nvnMultisampleStateGetAlphaToCoverageDither) { RET(G(uint8_t, A(0), 0xD)); }
NVN(nvnMultisampleStateGetCoverageToColorEnable) { RET(G(uint8_t, A(0), 0xE)); }
NVN(nvnMultisampleStateGetCoverageToColorOutput) { RET(G(uint8_t, A(0), 0xF)); }
NVN(nvnMultisampleStateGetCoverageModulationMode) { RET(G(uint8_t, A(0), 0x10)); }
NVN(nvnMultisampleStateGetSampleLocationsEnable) { RET(0); }
NVN(nvnMultisampleStateGetSampleLocationsGrid) { (void)c; }
NVN(nvnMultisampleStateGetSampleLocationsGridEnable) { RET(0); }

// PolygonState (4): cull, front face, polygon mode, offset enables
NVN(nvnPolygonStateSetDefaults) { uint8_t *p = AP(0); p[0] = 0; p[1] = 1; p[2] = 2; p[3] = 0; }
NVN(nvnPolygonStateSetCullFace) { G(uint8_t, A(0), 0) = (uint8_t)A(1); }
NVN(nvnPolygonStateSetFrontFace) { G(uint8_t, A(0), 1) = (uint8_t)A(1); }
NVN(nvnPolygonStateSetPolygonMode) { G(uint8_t, A(0), 2) = (uint8_t)A(1); }
NVN(nvnPolygonStateSetPolygonOffsetEnables) { G(uint8_t, A(0), 3) = (uint8_t)A(1); }
NVN(nvnPolygonStateGetCullFace) { RET(G(uint8_t, A(0), 0)); }
NVN(nvnPolygonStateGetFrontFace) { RET(G(uint8_t, A(0), 1)); }
NVN(nvnPolygonStateGetPolygonMode) { RET(G(uint8_t, A(0), 2)); }
NVN(nvnPolygonStateGetPolygonOffsetEnables) { RET(G(uint8_t, A(0), 3)); }

// VertexAttribState (4): format bits 0-7, stream bits 8-12, relative offset bits 13-31
NVN(nvnVertexAttribStateSetDefaults) { G(uint32_t, A(0), 0) = 0; }
NVN(nvnVertexAttribStateSetFormat) {
    uint32_t v = G(uint32_t, A(0), 0);
    G(uint32_t, A(0), 0) = (v & 0x1F00) | (uint32_t)(A(1) & 0xFF) | (uint32_t)(A(2) << 13);
}
NVN(nvnVertexAttribStateSetStreamIndex) {
    G(uint32_t, A(0), 0) = (G(uint32_t, A(0), 0) & ~0x1F00u) | (uint32_t)((A(1) & 0x1F) << 8);
}
NVN(nvnVertexAttribStateGetFormat) {  // (state, NVNformat *, ptrdiff_t *relative offset: 32-bit)
    uint32_t v = G(uint32_t, A(0), 0);
    *(uint32_t *)AP(1) = v & 0xFF;
    *(int32_t *)AP(2) = (int32_t)(v >> 13);
}
NVN(nvnVertexAttribStateGetStreamIndex) { RET((G(uint32_t, A(0), 0) >> 8) & 0x1F); }

// VertexStreamState (8): stride u32, divisor u32
NVN(nvnVertexStreamStateSetDefaults) { G(uint64_t, A(0), 0) = 0; }
NVN(nvnVertexStreamStateSetStride) { G(uint32_t, A(0), 0) = (uint32_t)A(1); }
NVN(nvnVertexStreamStateSetDivisor) { G(uint32_t, A(0), 4) = (uint32_t)A(1); }
NVN(nvnVertexStreamStateGetStride) { RET(G(uint32_t, A(0), 0)); }
NVN(nvnVertexStreamStateGetDivisor) { RET(G(uint32_t, A(0), 4)); }
