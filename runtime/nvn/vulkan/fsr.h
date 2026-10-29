// FSR 3.1 upscaling of the game's 3D picture ([Upscaler] FSR = On): runtime/nvn/vulkan/fsr.c. The hook points in
// exec.c and device.c are marked "FSR:"; with FSR off none of them changes anything.
#pragma once
#include "shaders.h"
#include "vk.h"

// A draw as the game recorded it, before its render pass begins (exec.c draw()).
typedef struct {
    NvnTexture *color0, *depth;  // its render targets (colour target 0, depth / stencil)
    NvnTexture *tex[16];         // the textures its fragment stage samples
    int ntex;
    uint32_t count;              // vertices (or indices)
    bool depth_write;            // it writes depth
    const uint64_t *vubuf;       // its vertex stage's uniform buffers: (guest address, size) pairs
    int nvubuf;
} FsrDraw;

extern bool g_fsr_on;  // FSR is on and works (set once at start-up: vk_fsr_init)
// FSR, or SMAA T2x / 4x ([Anti-aliasing] AntiAliasing = SMAAT2x / SMAA4x, picked live; never with FSR), runs: exec.c tells fsr.c
// the frame's draws (vk_fsr_draw) and captures them for object motion. T2x uses the same frame structure, camera,
// jitter and motion vectors as FSR, at the render size and without stand-ins (vk_fsr_map is FSR's only).
extern bool g_temporal_on;

void vk_fsr_init(void);                    // device.c, once the device exists
void vk_fsr_draw(const FsrDraw *d);        // exec.c draw(): may run FSR / T2x (the start of the HUD) before this draw
// exec.c, from the start of the HUD to the frame's present: the game's full-screen image, the window texture it is
// copied to, and the depth target drawn with them, as stand-ins at the output size (else t itself). depth: t is a
// depth target of a pass whose colour targets have stand-ins.
NvnTexture *vk_fsr_map(NvnTexture *t, bool depth);
// device.c backend_present, every frame: the texture to show (FSR: its stand-in); also where SMAA T2x starts and stops
NvnTexture *vk_fsr_present(NvnTexture *t);
// SMAA 4x (exec.c vk_ms_resolve): whether b is this frame's 3D scene, to resolve by SMAA S2x (phase: the frame's jitter,
// -1 for none), and that it was (the HUD start's T2x then adds no edges of its own).
bool vk_s2x_target(TexBackend *b, int *phase);
void vk_s2x_done(void);

// ---- object motion ([Upscaler] ObjectMotion): a scene draw is drawn a second time through a variant of its vertex
// shader that stores each vertex's clip position (gl_Position) in a buffer, indexed by gl_VertexIndex; the next frame's
// copy of the same draw gives the previous positions (fsr.c "object motion").
#define FSR_CAP_OUT_BINDING 30   // the variant's bindings (set 0; the translator uses 0-22 and 32+): positions out
#define FSR_CAP_BASE_BINDING 31  // ivec4 x: added to gl_VertexIndex
extern bool g_fsr_objvel;        // object motion on and possible (vertexPipelineStoresAndAtomics)

typedef struct FsrCapProg {
    _Atomic int state;           // 0 not built, 1 building, 2 ready, 3 failed
    char *src;                   // the vertex shader's GLSL until the variant is built
    VkShaderModule vs;
    VkBindingInfo binds[96];
    int nbinds;
    VkDescriptorSetLayout set0;  // the variant's only descriptor set (the vertex stage's, plus the two above)
    VkPipelineLayout layout;
} FsrCapProg;

typedef struct {
    NvnTexture *color0, *depth;  // the draw's render targets
    uint64_t vs_code;            // its vertex program (guest address)
    uint64_t vbuf[16];           // its vertex buffers (guest addresses of the streams it reads, else 0)
    uint32_t used_streams;
    uint64_t iaddr;              // its index buffer (guest address; 0: not indexed)
    uint32_t mode, count, first;
    int32_t base_vertex;
    const void *idx;             // the indices as drawn (NULL: not indexed), itype, icount; read from gidx (cached guest
    const void *gidx;            // memory, the same values) where set: the transient copy is write-combined, slow to read
    VkIndexType itype;
    uint32_t icount;
    // its vertex input (exec.c PipeKey), for the capture pipeline
    uint32_t attr_mask, attr_fmt[32], attr_off[32], attr_bind[32], stride[17], divisor[17];
    uint8_t topology, restart;
    VkRenderPass rp;
    bool depth_write;            // false: a depth-tested, not depth-writing draw of a skinned mesh (blended)
    bool skinned;                // its vertex shader indexes a constant buffer with a register (bone palettes)
    const uint64_t *vubuf;       // its vertex stage's uniform buffers: 18 (guest address, size) pairs
} FsrCapture;

FsrCapProg *vk_fsr_capprog_new(const char *vs_glsl);  // exec.c compile_program (keeps a copy of the source)
// exec.c draw(), after a depth-writing scene draw: true = record it again with *pipe and the variant's set 0, whose
// FSR_CAP_BASE_BINDING gets *base.
bool vk_fsr_capture(const FsrCapture *c, FsrCapProg *cp, VkPipeline *pipe, int32_t *base);
VkDescriptorBufferInfo vk_fsr_cap_buffer(void);      // the positions buffer (FSR_CAP_OUT_BINDING)
void vk_run_job(void (*fn)(void *), void *arg);      // (exec.c: on the shader build threads)
