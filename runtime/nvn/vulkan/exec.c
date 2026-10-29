// Executes recorded NVN command lists with Vulkan 1.2. A lean port of LAS Recomp's runtime/nvn/vulkan/exec.c (same
// command interpretation, tables and shader contract) for the 1.2 baseline: VkRenderPass objects + framebuffers instead
// of dynamic rendering, all fixed-function state in the pipeline, v1 barriers. Guest memory is copied (vertex / index /
// uniform ranges, once per submission) rather than imported: the 32-bit guest's heap is 2.5 GB and lazily committed.
//   MHGU_SYNC_SHADERS=1   build shaders / pipelines on first use, blocking (deterministic runs; default: on worker
//                         threads, draws needing them wait a bounded time, then are skipped until ready)
//   MHGU_PIPE_WAIT_MS=n   per-frame budget for those waits (default 6; 0: skip at once)
//   MHGU_NO_PROGRAM_CACHE=1  don't keep translated programs / pipeline states in cache/ for the next run to prebuild
//   MHGU_SAVE_GLSL=1      translated shaders -> build/glsl/<code address>_<stage>.glsl
//   MHGU_VK_STATS=1       draw / pass / skip counts in the log every 2 s (and shader / pipeline build stats)
//   MHGU_SKIP_LOG=1       every draw skipped for a building program / pipeline: its shaders and colour target
//   MHGU_NO_PREWARM=1     don't translate the game's programs ahead of their first use (see "prewarm"; the ini's
//                         [Performance] ShaderPrewarm = Off does the same)
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>  // ftruncate (mingw pulls it in through another header; POSIX does not)

#include "../../kernel.h"
#include "fsr.h"
#include "shaders.h"
#include "vk.h"

static uint64_t stat_draws, stat_skipped, stat_passes, stat_dispatches, stat_snapshots;
// (MHGU_VK_STATS) time spent: translating command lists, waiting for the GPU, guest memory copied per submission
uint64_t stat_exec_ns, stat_wait_ns, stat_copy_bytes, stat_upload_ns;
static _Atomic int stat_compiles;
// (MHGU_VK_STATS) shader / pipeline builds: skips by cause, frames with skips, the waits, build times per job kind
// (translate, GLSL -> SPIR-V + modules, pipeline), request -> ready latency, the slowest frame (exec thread's view)
static uint64_t stat_skip_prog, stat_skip_pipe, stat_skip_frames, stat_waited_ns, stat_waits, stat_rescued, stat_frame_max;
static _Atomic uint64_t stat_pw_new, stat_pw_known, stat_pw_left;  // prewarm: translated, already cached, jobs queued
static _Atomic uint64_t stat_job_n[3], stat_job_ns[3], stat_job_max[3], stat_lat_n, stat_lat_ns, stat_lat_max;
static void atomic_max(_Atomic uint64_t *m, uint64_t v) {
    uint64_t o = atomic_load(m);
    while (v > o && !atomic_compare_exchange_weak(m, &o, v)) {}
}
static void job_stat(int kind, uint64_t t0) {
    uint64_t d = plat_now_ns() - t0;
    atomic_fetch_add(&stat_job_n[kind], 1);
    atomic_fetch_add(&stat_job_ns[kind], d);
    atomic_max(&stat_job_max[kind], d);
}

// ================================================================== submissions
// A submission = optional upload command buffer + the main one, signalling the timeline with its number.
#define SUBS 8
typedef struct {
    VkCommandBuffer main, upload;
    bool main_begun, upload_begun;
    uint64_t seq;
    VkDescriptorPool pools[32];
    int npools, cur_pool;
} Sub;
static Sub subs[SUBS];
static int cur_sub;
static uint64_t submit_seq;  // last submitted
static pthread_mutex_t seq_lock = PTHREAD_MUTEX_INITIALIZER;
static bool in_pass;
static void ss_forget(void);

uint64_t vk_completed_seq(void) {
    uint64_t v = 0;
    vkGetSemaphoreCounterValue(g_vk.dev, g_vk.timeline, &v);
    return v;
}

void vk_wait_seq(uint64_t seq) {
    if (!seq || vk_exiting()) return;  // (exit in progress: never reach the driver from a late guest thread)
    uint64_t t0_ = plat_now_ns();
    VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &g_vk.timeline,
                              .pValues = &seq};
    VkResult r = vkWaitSemaphores(g_vk.dev, &wi, 10ull * 1000000000ull);
    stat_wait_ns += plat_now_ns() - t0_;
    if (r == VK_TIMEOUT) LOG("[vk] GPU did not finish submission %llu within 10 s (device lost?)\n", (unsigned long long)seq);
    else if (r != VK_SUCCESS) FATAL("vkWaitSemaphores: %d (device lost)\n", r);
}

uint64_t vk_pending_seq(void) { return submit_seq + 1; }

static Sub *sub(void) {
    Sub *s = &subs[cur_sub];
    if (s->seq != submit_seq + 1) {  // slot reused: its previous submission must be done
        vk_wait_seq(s->seq);
        for (int i = 0; i < s->npools; i++) vkResetDescriptorPool(g_vk.dev, s->pools[i], 0);
        s->cur_pool = 0;
        s->seq = submit_seq + 1;
        s->main_begun = s->upload_begun = false;
    }
    return s;
}

static void begin_cb(VkCommandBuffer *cb, bool *begun) {
    if (*begun) return;
    if (!*cb) {
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g_vk.pool,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        VK_CHECK(vkAllocateCommandBuffers(g_vk.dev, &ai, cb));
    }
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK_CHECK(vkBeginCommandBuffer(*cb, &bi));
    *begun = true;
}

VkCommandBuffer vk_cmd(void) {
    Sub *s = sub();
    if (!s->main_begun) {
        begin_cb(&s->main, &s->main_begun);
        ss_forget();
    }
    return s->main;
}

VkCommandBuffer vk_upload_cmd(void) {
    Sub *s = sub();
    if (!s->upload_begun) {
        begin_cb(&s->upload, &s->upload_begun);
        // after everything submitted before it: these copies overwrite what earlier work may still read
        vk_full_barrier(s->upload);
    }
    return s->upload;
}

void vk_full_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_ALL_WRITES, .dstAccessMask = VK_ACCESS_ALL};
    vkCmdPipelineBarrier(cb, VK_STAGES_ALL, VK_STAGES_ALL, 0, 1, &mb, 0, NULL, 0, NULL);
}

static void guest_copy_new_submission(void);

// Submit the pending work (if any). wait / signal: binary semaphores for presentation (may be null).
void vk_submit_extra(VkSemaphore wait, VkSemaphore signal) {
    vk_end_pass();
    Sub *s = sub();
    if (!s->main_begun && !s->upload_begun && !wait && !signal) return;
    VkCommandBuffer cbs[2];
    uint32_t n = 0;
    if (s->upload_begun) {
        vk_full_barrier(s->upload);  // uploads complete before anything in the main buffer reads them
        VK_CHECK(vkEndCommandBuffer(s->upload));
        cbs[n++] = s->upload;
    }
    vk_cmd();  // (an empty main buffer still carries the semaphores)
    VK_CHECK(vkEndCommandBuffer(s->main));
    cbs[n++] = s->main;
    VkSemaphore sig[2] = {g_vk.timeline, signal};
    uint64_t sig_val[2] = {submit_seq + 1, 0};
    uint64_t wait_val = 0;
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkTimelineSemaphoreSubmitInfo ts = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                                        .waitSemaphoreValueCount = wait ? 1 : 0, .pWaitSemaphoreValues = &wait_val,
                                        .signalSemaphoreValueCount = signal ? 2 : 1, .pSignalSemaphoreValues = sig_val};
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &ts, .waitSemaphoreCount = wait ? 1 : 0,
                       .pWaitSemaphores = &wait, .pWaitDstStageMask = &wait_stage, .commandBufferCount = n,
                       .pCommandBuffers = cbs, .signalSemaphoreCount = signal ? 2 : 1, .pSignalSemaphores = sig};
    VK_CHECK(vkQueueSubmit(g_vk.queue, 1, &si, VK_NULL_HANDLE));
    pthread_mutex_lock(&seq_lock);
    submit_seq++;
    pthread_mutex_unlock(&seq_lock);
    cur_sub = (cur_sub + 1) % SUBS;
    guest_copy_new_submission();
    vk_collect_dead();
}

static void renderer_submit(void) { vk_submit_extra(VK_NULL_HANDLE, VK_NULL_HANDLE); }

void renderer_flush(void) {
    if (vk_exiting()) return;
    vk_lock();
    renderer_submit();
    uint64_t s = submit_seq;
    vk_unlock();
    vk_wait_seq(s);
}

void renderer_sync(void) {
    pthread_mutex_lock(&seq_lock);
    uint64_t s = submit_seq;
    pthread_mutex_unlock(&seq_lock);
    vk_wait_seq(s);
}

uint64_t renderer_fence_seq(void) {
    pthread_mutex_lock(&seq_lock);
    uint64_t q = submit_seq + (subs[cur_sub].main_begun || subs[cur_sub].upload_begun ? 1 : 0);
    pthread_mutex_unlock(&seq_lock);
    return q;
}

void renderer_wait_seq(uint64_t seq) {
    if (!seq) return;
    pthread_mutex_lock(&seq_lock);
    uint64_t s = submit_seq;
    pthread_mutex_unlock(&seq_lock);
    vk_wait_seq(seq > s ? s : seq);  // not submitted yet (other thread): everything submitted so far
}

VkDescriptorSet vk_alloc_set(VkDescriptorSetLayout layout) {
    Sub *s = sub();
    for (;;) {
        if (s->cur_pool == s->npools) {
            if (s->npools == 32) FATAL("descriptor pools exhausted\n");
            VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 16384}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2048},
                                            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16384},
                                            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 512}, {VK_DESCRIPTOR_TYPE_SAMPLER, 512},
                                            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 512}};
            VkDescriptorPoolCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 4096,
                                             .poolSizeCount = 6, .pPoolSizes = sizes};
            VK_CHECK(vkCreateDescriptorPool(g_vk.dev, &ci, NULL, &s->pools[s->npools++]));
        }
        VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                          .descriptorPool = s->pools[s->cur_pool], .descriptorSetCount = 1, .pSetLayouts = &layout};
        VkDescriptorSet set;
        if (vkAllocateDescriptorSets(g_vk.dev, &ai, &set) == VK_SUCCESS) return set;
        s->cur_pool++;
    }
}

// ================================================================== transient memory ring
// 32 MB host-visible chunks; a chunk is reused once every submission that allocated from it has completed.
#define CHUNK_SIZE (32ull << 20)
typedef struct { VkBuffer buf; VmaAllocation mem; uint8_t *ptr; uint64_t size, used, seq; } Chunk;
static Chunk *chunks;
static int nchunks, active = -1;

static int new_chunk(uint64_t size) {
    chunks = realloc(chunks, (nchunks + 1) * sizeof *chunks);
    Chunk *c = &chunks[nchunks];
    memset(c, 0, sizeof *c);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                             .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &c->buf, &c->mem, &info));
    c->ptr = info.pMappedData;
    c->size = size;
    return nchunks++;
}

VkSpan vk_transient(uint64_t size, uint64_t align) {
    if (align < 16) align = 16;
    uint64_t pending = vk_pending_seq();
    if (active >= 0) {
        Chunk *c = &chunks[active];
        uint64_t off = (c->used + align - 1) & ~(align - 1);
        if (off + size <= c->size) {
            c->used = off + size;
            c->seq = pending;
            return (VkSpan){c->buf, off, c->ptr + off};
        }
    }
    uint64_t done = vk_completed_seq();
    int pick = -1;
    for (int i = 0; i < nchunks; i++)
        if (i != active && chunks[i].seq <= done && chunks[i].size >= size) { pick = i; break; }
    if (pick < 0) pick = new_chunk(size > CHUNK_SIZE ? size : CHUNK_SIZE);
    active = pick;
    chunks[pick].used = size;
    chunks[pick].seq = pending;
    return (VkSpan){chunks[pick].buf, 0, chunks[pick].ptr};
}

VkSpan vk_transient_copy(const void *data, uint64_t size, uint64_t align) {
    VkSpan s = vk_transient(size ? size : 16, align);
    if (size) memcpy(s.ptr, data, size);
    return s;
}

// GPU -> CPU copies: one grow-only host-cached buffer (every user waits for the GPU and reads before the next one).
VkSpan vk_readback(uint64_t size) {
    static VkBuffer buf;
    static VmaAllocation mem;
    static uint8_t *ptr;
    static uint64_t cap;
    if (size > cap) {
        if (buf) { vk_wait_seq(submit_seq); vmaDestroyBuffer(g_vk.vma, buf, mem); }
        cap = (size + (16 << 20) - 1) & ~((16ull << 20) - 1);
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = cap, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
        VmaAllocationInfo info;
        VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &buf, &mem, &info));
        ptr = info.pMappedData;
    }
    return (VkSpan){buf, 0, ptr};
}

// Guest memory copied once per submission: draws binding the same range share the copy.
#define GCOPY_TAB 4096
static struct { uint64_t addr, size, gen; VkSpan span; } gcopy[GCOPY_TAB];
static uint64_t gcopy_gen = 1;  // bumped per submission and when guest memory is written mid-list
static void guest_copy_new_submission(void) { gcopy_gen++; }
static VkSpan guest_copy(uint64_t addr, uint64_t size) {
    uint64_t h = (addr * 0x9E3779B97F4A7C15ull) ^ (size * 0xC2B2AE3D27D4EB4Full);
    uint32_t slot = (uint32_t)(h ^ (h >> 32)) & (GCOPY_TAB - 1);
    if (gcopy[slot].gen == gcopy_gen && gcopy[slot].addr == addr && gcopy[slot].size == size) return gcopy[slot].span;
    VkSpan s = vk_transient_copy(G2H((uint32_t)addr), size, 256);
    stat_copy_bytes += size;
    gcopy[slot].addr = addr; gcopy[slot].size = size; gcopy[slot].gen = gcopy_gen; gcopy[slot].span = s;
    return s;
}

static bool guest_range_ok(uint64_t addr, uint64_t size) { return addr && addr < GUEST_SPACE && size <= GUEST_SPACE - addr; }

// ---- vertex cache: device-local copies of bound vertex ranges, kept across submissions and refreshed only when the
// CPU wrote their pages since (page write tracking, textures.c vk_pages_changed). The copy goes into the submission's
// upload command buffer, which starts with a full barrier (earlier submissions' reads are done) and completes before
// its draws. Without tracking for a range (outside the heap): not cached. MHGU_VK_VCACHE=0: off.
#define VC_TAB 4096
#define VC_BUDGET (384ull << 20)
typedef struct { uint64_t addr, size, used_seq, checked_seq; uint32_t gen; VkBuffer buf; VmaAllocation mem; } VcEnt;
static VcEnt vc_tab[VC_TAB];
static uint64_t vc_bytes;
uint64_t stat_vc_hits, stat_vc_refresh;

static void vc_drop(VcEnt *e) {
    vk_defer_buffer(e->buf, e->mem);
    vc_bytes -= e->size;
    memset(e, 0, sizeof *e);
}

static bool vcache(uint64_t addr, uint64_t size, VkBuffer *buf) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("MHGU_VK_VCACHE"); on = !e || atoi(e) != 0; }
    if (!on || size < 4096 || size > (32u << 20)) return false;
    uint64_t pend = vk_pending_seq();
    uint64_t h = (addr * 0x9E3779B97F4A7C15ull) ^ (size * 0xC2B2AE3D27D4EB4Full);
    uint32_t slot = (uint32_t)(h ^ (h >> 32)) & (VC_TAB - 1), free_slot = ~0u, old = ~0u;
    VcEnt *e = NULL;
    for (int probe = 0; probe < 16; probe++, slot = (slot + 1) & (VC_TAB - 1)) {
        VcEnt *c = &vc_tab[slot];
        if (c->buf && c->addr == addr && c->size == size) { e = c; break; }
        if (!c->buf && free_slot == ~0u) free_slot = slot;
        if (c->buf && c->used_seq < pend && (old == ~0u || c->used_seq < vc_tab[old].used_seq)) old = slot;
    }
    bool copy = false;
    uint32_t since = 0;  // refresh: only pages written after this generation
    if (!e) {
        uint32_t now = 0;
        if (vk_pages_changed(addr, size, 0, &now) < 0) return false;  // (no tracking here: per-submission copies)
        if (free_slot == ~0u) {
            if (old == ~0u) return false;
            vc_drop(&vc_tab[old]);
            free_slot = old;
        }
        while (vc_bytes + size > VC_BUDGET) {  // the least recently used go
            int lru = -1;
            for (int i = 0; i < VC_TAB; i++)
                if (vc_tab[i].buf && vc_tab[i].used_seq < pend && (lru < 0 || vc_tab[i].used_seq < vc_tab[lru].used_seq)) lru = i;
            if (lru < 0) return false;
            vc_drop(&vc_tab[lru]);
        }
        e = &vc_tab[free_slot];
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                                 .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateBuffer(g_vk.vma, &bi, &ai, &e->buf, &e->mem, NULL) != VK_SUCCESS) { memset(e, 0, sizeof *e); return false; }
        e->addr = addr; e->size = size; e->gen = now; e->checked_seq = pend;
        vc_bytes += size;
        copy = true;
    } else if (e->checked_seq != pend) {  // first use in this submission: written since the last copy?
        uint32_t now = 0;
        int ch = vk_pages_changed(addr, size, e->gen, &now);
        e->checked_seq = pend;
        if (ch != 0) { copy = true; since = ch > 0 ? e->gen : 0; e->gen = now; }
    }
    if (copy && since) {  // the pages written since the last refresh, as runs
        extern uint32_t vk_page_gen(uint64_t addr);
        VkBufferCopy rg[64];
        int n = 0;
        uint64_t first = addr & ~4095ull;
        for (uint64_t p = first; p < addr + size; p += 4096) {
            if (vk_page_gen(p) <= since) continue;
            uint64_t lo = p < addr ? addr : p, hi = p + 4096 > addr + size ? addr + size : p + 4096;
            if (n && rg[n - 1].dstOffset + rg[n - 1].size == lo - addr) { rg[n - 1].size += hi - lo; continue; }
            if (n == 64) { n = -1; break; }
            rg[n++] = (VkBufferCopy){lo - addr, lo - addr, hi - lo};  // (srcOffset fixed up below)
        }
        if (n < 0) since = 0;  // (scattered: the whole range)
        else {
            for (int k = 0; k < n; k++) {
                VkSpan s = vk_transient_copy((uint8_t *)G2H((uint32_t)addr) + rg[k].dstOffset, rg[k].size, 16);
                rg[k].srcOffset = s.off;
                vkCmdCopyBuffer(vk_upload_cmd(), s.buf, e->buf, 1, &rg[k]);
                stat_copy_bytes += rg[k].size;
            }
            stat_vc_refresh++;
        }
    }
    if (copy && !since) {
        VkSpan s = vk_transient_copy(G2H((uint32_t)addr), size, 16);
        VkBufferCopy rg = {s.off, 0, size};
        vkCmdCopyBuffer(vk_upload_cmd(), s.buf, e->buf, 1, &rg);
        stat_copy_bytes += size;
        stat_vc_refresh++;
    } else {
        stat_vc_hits++;
    }
    e->used_seq = pend;
    *buf = e->buf;
    return true;
}

// ================================================================== worker threads (shader / pipeline builds)
// Two lists per queue: jobs a draw is (or may be) waiting for, and the start-up prewarm's (low: taken only when the
// first list is empty).
typedef struct Job { void (*fn)(void *); void *arg; struct Job *next; } Job;
typedef struct { Job *head, *tail, *lhead, *ltail; pthread_mutex_t lock; pthread_cond_t cond; bool started; } JobQueue;
static JobQueue compile_q = {NULL, NULL, NULL, NULL, PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, false};
static JobQueue translate_q = {NULL, NULL, NULL, NULL, PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, false};

static void *worker(void *a) {
    JobQueue *q = a;
    for (;;) {
        pthread_mutex_lock(&q->lock);
        while (!q->head && !q->lhead) pthread_cond_wait(&q->cond, &q->lock);
        Job *j;
        if (q->head) {
            j = q->head;
            q->head = j->next;
            if (!q->head) q->tail = NULL;
        } else {
            j = q->lhead;
            q->lhead = j->next;
            if (!q->lhead) q->ltail = NULL;
        }
        pthread_mutex_unlock(&q->lock);
        j->fn(j->arg);
        free(j);
    }
    return NULL;
}

static bool sync_shaders(void) {
    static int v = -1;
    if (v < 0) v = getenv("MHGU_SYNC_SHADERS") != NULL;
    return v;
}

static void enqueue(JobQueue *q, int nthreads, bool low, void (*fn)(void *), void *arg) {
    if (sync_shaders()) { fn(arg); return; }
    pthread_mutex_lock(&q->lock);
    if (!q->started) {
        q->started = true;
        for (int i = 0; i < nthreads; i++) { pthread_t t; pthread_create(&t, NULL, worker, q); }
    }
    Job *j = malloc(sizeof *j);
    j->fn = fn; j->arg = arg; j->next = NULL;
    Job **head = low ? &q->lhead : &q->head, **tail = low ? &q->ltail : &q->tail;
    if (*tail) (*tail)->next = j; else *head = j;
    *tail = j;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->lock);
}

// The translator isn't re-entrant: one thread translates, the compiles run on a few workers.
static pthread_mutex_t translate_lock = PTHREAD_MUTEX_INITIALIZER;
static void run_translate(void (*fn)(void *), void *arg) { enqueue(&translate_q, 1, false, fn, arg); }
static void run_job(void (*fn)(void *), void *arg) { enqueue(&compile_q, 3, false, fn, arg); }
void vk_run_job(void (*fn)(void *), void *arg) { run_job(fn, arg); }  // (fsr.c: its capture shaders / pipelines)

// A queued job a draw is waiting for goes to the front of its queue (the queues are FIFO, and an area load queues a
// few hundred builds at once), from the prewarm's list too.
static void promote(JobQueue *q, void *arg) {
    pthread_mutex_lock(&q->lock);
    for (int low = 0; low < 2; low++) {
        Job **head = low ? &q->lhead : &q->head, **tail = low ? &q->ltail : &q->tail;
        for (Job *j = *head, *prev = NULL; j; prev = j, j = j->next) {
            if (j->arg != arg) continue;
            if (!low && !prev) goto done;  // (already first)
            if (prev) prev->next = j->next; else *head = j->next;
            if (*tail == j) *tail = prev;
            j->next = q->head;
            q->head = j;
            if (!q->tail) q->tail = j;
            goto done;
        }
    }
done:
    pthread_mutex_unlock(&q->lock);
}

// Build states (0 building, 1 ready, 2 failed) change under ready_lock so a draw can wait for one.
static pthread_mutex_t ready_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready_cond = PTHREAD_COND_INITIALIZER;
static void set_state(_Atomic int *state, int v, uint64_t t_req) {
    if (t_req) {
        uint64_t d = plat_now_ns() - t_req;
        atomic_fetch_add(&stat_lat_n, 1);
        atomic_fetch_add(&stat_lat_ns, d);
        atomic_max(&stat_lat_max, d);
    }
    pthread_mutex_lock(&ready_lock);
    atomic_store(state, v);
    pthread_cond_broadcast(&ready_cond);
    pthread_mutex_unlock(&ready_lock);
}

// A draw whose program / pipeline is still building waits for it, up to MHGU_PIPE_WAIT_MS per frame in all, rather
// than being skipped at once: most builds take a few ms (SPIR-V and pipeline caches), so the effect appears in its
// first frame instead of a few frames later, and a slow (uncached) build costs at most the budget once per frame.
// at_least_ns: a draw that mustn't be skipped waits that long whatever is left of the budget.
//
// The quest photo (Quest Rewards / Reward Box screen): at the clear the game copies the scene into its photo buffer
// once and never again, so a draw skipped then leaves the photo as whatever the buffer's memory held: black (the R9),
// or other textures' data (RADV, Steam Deck). The game's request (0xa6f97c, hooked in tools/gen.py) opens a window of
// PHOTO_FRAMES frames in which every draw waits for its build, up to PHOTO_WAIT_NS: the capture and the scene it copies.
#define PHOTO_FRAMES 8
#define PHOTO_WAIT_NS 2000000000ll
static _Atomic uint64_t photo_until;
void backend_photo_requested(void) {
    uint64_t f = vk_frame_counter();
    if (atomic_exchange(&photo_until, f + PHOTO_FRAMES) < f)
        LOG("[vk] quest photo requested (frame %llu): draws wait for their shaders until it is taken\n", (unsigned long long)f);
}
static int64_t wait_budget_ns;
static uint64_t wait_frame = ~0ull;
static bool await_build(_Atomic int *state, void *job, int64_t at_least_ns) {
    int s = atomic_load(state);
    if (s) return s == 1;
    promote(&translate_q, job);
    promote(&compile_q, job);
    static int64_t budget = -1;
    if (budget < 0) {
        const char *e = getenv("MHGU_PIPE_WAIT_MS");
        budget = (e ? atoi(e) : 6) * 1000000ll;
        if (budget < 0) budget = 0;
    }
    uint64_t f = vk_frame_counter();
    uint64_t photo = atomic_load(&photo_until);
    if (photo && f <= photo && at_least_ns < PHOTO_WAIT_NS) at_least_ns = PHOTO_WAIT_NS;
    if (f != wait_frame) wait_frame = f, wait_budget_ns = budget;
    int64_t allowed = wait_budget_ns > at_least_ns ? wait_budget_ns : at_least_ns;
    if (allowed <= 0) return false;
    uint64_t t0 = plat_now_ns(), deadline = t0 + (uint64_t)allowed;
    struct timespec rt;
    clock_gettime(CLOCK_REALTIME, &rt);  // (the condition variable's clock)
    int64_t rt_off = (int64_t)rt.tv_sec * 1000000000ll + rt.tv_nsec - (int64_t)t0;
    pthread_mutex_lock(&ready_lock);
    for (uint64_t now = t0; !(s = atomic_load(state)) && now < deadline; now = plat_now_ns()) {
        // 1 ms slices: a program's GLSL compile is queued once its translation is done, and goes to the front too
        uint64_t until = now + 1000000 < deadline ? now + 1000000 : deadline;
        int64_t abs_ns = (int64_t)until + rt_off;
        struct timespec ts = {(time_t)(abs_ns / 1000000000ll), (long)(abs_ns % 1000000000ll)};
        pthread_cond_timedwait(&ready_cond, &ready_lock, &ts);
        if (!atomic_load(state)) {
            pthread_mutex_unlock(&ready_lock);
            promote(&compile_q, job);
            pthread_mutex_lock(&ready_lock);
        }
    }
    pthread_mutex_unlock(&ready_lock);
    s = atomic_load(state);
    uint64_t d = plat_now_ns() - t0;
    wait_budget_ns -= (int64_t)d;
    stat_waited_ns += d;
    stat_waits++;
    if (s == 1) stat_rescued++;
    return s == 1;
}

// ================================================================== state
typedef struct {
    NvnTexture *color[8];
    NvnTextureView cview[8];
    uint8_t has_cview[8];
    int ncolor;
    NvnTexture *depth;
    NvnTextureView dview;
    uint8_t has_dview;
} RtState;

typedef struct { uint64_t addr, size; } Range;

static struct {
    RtState rt;
    bool rt_dirty;
    NvnProgram *program;
    uint8_t blend[8][8];
    uint32_t channel_mask;
    uint8_t color_state[4], ds[8], ms[0x18], poly[4];
    float poly_offset[3];
    uint32_t attrib[16];
    int nattrib;
    uint32_t stream[16][2];
    int nstream;
    Range vbuf[16], ubuf[6][18], sbuf[6][16];
    uint64_t tex[6][32], img[6][8];
    NvnDescPool *tpool, *spool;
    float viewport[16][4];
    int32_t scissor[16][4];
    float depth_range[16][2];
    float blend_color[4];
    uint32_t stencil_ref[2], stencil_mask[2], stencil_vmask[2];
    bool prim_restart, depth_clamp;
    uint32_t restart_index;
    float alpha_ref;
    uint64_t copy_row_stride, copy_image_stride;
} st;

// A draw skipped because its program / pipeline is still building. MHGU_SKIP_LOG=1 logs each one with its shaders and
// colour target 0 (which pass lost it; a one-off pass, like the quest photo, stays wrong).
static void skip_draw(uint64_t *cause) {
    static uint64_t last = ~0ull;
    static int log = -1;
    uint64_t f = vk_frame_counter();
    if (f != last) last = f, stat_skip_frames++;
    stat_skipped++;
    (*cause)++;
    if (log < 0) log = getenv("MHGU_SKIP_LOG") != NULL;
    if (log && st.program) {
        NvnTexture *t = st.rt.ncolor ? st.rt.color[0] : NULL;
        LOG("[skip] frame %llu %s vs %llx fs %llx | rt0 %p %ux%u f%#x\n", (unsigned long long)f,
            cause == &stat_skip_prog ? "program" : "pipeline", (unsigned long long)st.program->stages[0].code_addr,
            (unsigned long long)st.program->stages[1].code_addr, (void *)t, t ? t->width : 0, t ? t->height : 0, t ? t->format : 0);
    }
}

uint32_t nvn_window_origin(void);
uint32_t nvn_depth_mode(void);

static void st_defaults(void) {
    memset(&st, 0, sizeof st);
    for (int i = 0; i < 16; i++) {
        st.viewport[i][2] = 1280; st.viewport[i][3] = 720;
        st.scissor[i][2] = 16384; st.scissor[i][3] = 16384;
        st.depth_range[i][1] = 1.0f;
    }
    st.channel_mask = 0xFFFFFFFF;
    st.stencil_mask[0] = st.stencil_mask[1] = 0xFF;
    st.stencil_vmask[0] = st.stencil_vmask[1] = 0xFF;
    st.ds[1] = 1; st.ds[2] = 2;
    st.poly[1] = 1; st.poly[2] = 2;
    for (int i = 0; i < 8; i++) { st.blend[i][1] = 2; st.blend[i][2] = 1; st.blend[i][3] = 2; st.blend[i][4] = 1; st.blend[i][5] = 1; st.blend[i][6] = 1; }
}

void vk_exec_init(void) { st_defaults(); }

// ================================================================== render passes and framebuffers
static pthread_mutex_t rp_lock = PTHREAD_MUTEX_INITIALIZER;  // (pipeline workers ask for render passes too)

VkRenderPass vk_render_pass(const VkFormat *color, uint32_t ncolor, VkFormat depth, uint32_t samples) {
    static struct { VkFormat c[8], d; uint32_t n, s; VkRenderPass rp; } tab[256];
    static int ntab;
    pthread_mutex_lock(&rp_lock);
    for (int i = 0; i < ntab; i++)
        if (tab[i].n == ncolor && tab[i].d == depth && tab[i].s == samples && !memcmp(tab[i].c, color, ncolor * sizeof *color)) {
            VkRenderPass rp = tab[i].rp;
            pthread_mutex_unlock(&rp_lock);
            return rp;
        }
    VkAttachmentDescription a[9];
    VkAttachmentReference cref[8], dref;
    uint32_t na = 0;
    // (MSAA: the multisampled images stay in their attachment layouts: textures.c vk_tex_ms)
    VkImageLayout cl = samples > 1 ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
    VkImageLayout dl = samples > 1 ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
    for (uint32_t i = 0; i < ncolor; i++) {
        if (color[i] == VK_FORMAT_UNDEFINED) { cref[i] = (VkAttachmentReference){VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_GENERAL}; continue; }
        a[na] = (VkAttachmentDescription){.format = color[i], .samples = (VkSampleCountFlagBits)samples,
                                          .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                          .initialLayout = cl, .finalLayout = cl};
        cref[i] = (VkAttachmentReference){na++, cl};
    }
    if (depth != VK_FORMAT_UNDEFINED) {
        a[na] = (VkAttachmentDescription){.format = depth, .samples = (VkSampleCountFlagBits)samples,
                                          .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
                                          .initialLayout = dl, .finalLayout = dl};
        dref = (VkAttachmentReference){na++, dl};
    }
    VkSubpassDescription sp = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = ncolor,
                               .pColorAttachments = cref, .pDepthStencilAttachment = depth != VK_FORMAT_UNDEFINED ? &dref : NULL};
    // (the full barriers around passes order them; these keep the implicit external dependencies broad)
    VkSubpassDependency dep[2] = {
        {VK_SUBPASS_EXTERNAL, 0, VK_STAGES_ALL, VK_STAGES_ALL & ~(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT),
         VK_ACCESS_ALL_WRITES, VK_ACCESS_ALL, 0},
        {0, VK_SUBPASS_EXTERNAL, VK_STAGES_ALL & ~(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT), VK_STAGES_ALL,
         VK_ACCESS_ALL_WRITES, VK_ACCESS_ALL, 0}};
    VkRenderPassCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = na, .pAttachments = a,
                                 .subpassCount = 1, .pSubpasses = &sp, .dependencyCount = 2, .pDependencies = dep};
    VkRenderPass rp;
    VK_CHECK(vkCreateRenderPass(g_vk.dev, &ci, NULL, &rp));
    if (ntab < 256) {
        memcpy(tab[ntab].c, color, ncolor * sizeof *color);
        tab[ntab].n = ncolor; tab[ntab].d = depth; tab[ntab].s = samples; tab[ntab].rp = rp;
        ntab++;
    }
    pthread_mutex_unlock(&rp_lock);
    return rp;
}

#define FB_TAB 1024
typedef struct { VkRenderPass rp; VkImageView v[9]; uint32_t n, w, h; VkFramebuffer fb; } FbEnt;
static FbEnt fb_tab[FB_TAB];

VkFramebuffer vk_framebuffer(VkRenderPass rp, const VkImageView *views, uint32_t n, uint32_t w, uint32_t h) {
    uint64_t hh = (uint64_t)(uintptr_t)rp * 0x9E3779B97F4A7C15ull ^ ((uint64_t)w << 32 | h);
    for (uint32_t i = 0; i < n; i++) hh = (hh ^ (uint64_t)(uintptr_t)views[i]) * 0xC2B2AE3D27D4EB4Full;
    uint32_t slot = (uint32_t)(hh ^ (hh >> 32)) & (FB_TAB - 1);
    for (int probe = 0; probe < 16; probe++, slot = (slot + 1) & (FB_TAB - 1)) {
        FbEnt *e = &fb_tab[slot];
        if (e->fb && e->rp == rp && e->n == n && e->w == w && e->h == h && !memcmp(e->v, views, n * sizeof *views)) return e->fb;
        if (!e->fb) break;
    }
    VkFramebufferCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = n,
                                  .pAttachments = views, .width = w, .height = h, .layers = 1};
    VkFramebuffer fb;
    VK_CHECK(vkCreateFramebuffer(g_vk.dev, &ci, NULL, &fb));
    FbEnt *e = &fb_tab[slot];
    if (e->fb) vk_defer_framebuffer(e->fb);  // (evicted)
    e->rp = rp; e->n = n; e->w = w; e->h = h; e->fb = fb;
    memcpy(e->v, views, n * sizeof *views);
    return fb;
}

void vk_framebuffers_forget_view(VkImageView v) {
    for (int i = 0; i < FB_TAB; i++) {
        FbEnt *e = &fb_tab[i];
        if (!e->fb) continue;
        for (uint32_t k = 0; k < e->n; k++)
            if (e->v[k] == v) { vk_defer_framebuffer(e->fb); e->fb = VK_NULL_HANDLE; break; }
    }
}

// ================================================================== passes
static VkFormat pass_color[8], pass_depth;
static uint32_t pass_ncolor, pass_samples = 1, pass_w, pass_h;
static bool ui_relax_scissor;  // the draw being set up moved a HUD piece out from under the game's scissor
static int ui_scissor_v = -1;  // 16:10: the draw being set up is a HUD piece anchored so (0 top, 1 centre, 2 bottom),
                               // and its scissor moves with it
static bool ui_menu_pass;  // this pass drew a layer 97 menu before (the windows of layer 100 then sit beside Top-anchored ones)
// The HUD quads this pass drew last (16:9 canvas place, vertical anchor 0 top / 1 centre / 2 bottom): a model preview
// follows the window drawn over it (ui_preview_map)
#define UI_RECENT 256
static struct { float x, y; uint8_t vanchor; } ui_recent[UI_RECENT];
static uint32_t ui_nrecent;  // (quads recorded this pass; the slot is the count modulo UI_RECENT)
static bool ui_loading_pass;  // this pass draws the loading screen (its backdrop, layer 110, came first)
static bool ui_title_pass;  // this pass draws the title screen (its layout, layer 108 at the canvas centre, came first)
static bool ui_results_pass, ui_results_last;  // this pass / the last pass with HUD in it draws the quest results screen
volatile int g_hudtrace_active;  // (debug) MHGU_CONTROL "hudtrace": log every HUD quad of the next frame and its anchor (device.c)

// 16:10: a window is drawn as pieces (body, edges, corners, then its text), each placed by its own centre, so one rule
// could send its top edge up and its bottom edge down (the chat window, the shoutout list, the Guild Card greeting).
// The pieces drawn one after another on one layer, near each other, are a group; when their anchors disagree the whole
// group takes one, from where it sits: by the nearer edge (top or bottom) or, with about as much room above as below,
// the centre. Their matrices are still in transient memory (not submitted before the pass ends), so the group is
// settled when it is complete: at the next piece that doesn't belong to it, a 3D draw (a model preview inside the
// window follows its anchor) or the end of the pass.
#define UI_GROUP_MAX 512
static struct {
    int n, predict;  // predict: the anchor this group got last frame (ui_group_known), -1 if not seen
    float z, x0, y0, xmin, xmax, ymin, ymax;
    uint8_t anchors;  // (bit per vertical anchor among its pieces)
    struct { float *m, pos_y; uint8_t vanchor; uint32_t recent; } p[UI_GROUP_MAX];
} ui_group;
// Each group's final anchor, by its first piece (layer and place): the HUD is drawn the same way every frame, so the
// scissors of a group's pieces, recorded before the group is complete, take the anchor it got last frame
#define UI_KNOWN 64
static struct { float z, x, y; int v; } ui_group_known[UI_KNOWN];
static uint32_t ui_nknown;
static int ui_group_lookup(float z, float x, float y) {
    for (int i = 0; i < UI_KNOWN; i++)
        if (ui_group_known[i].z == z && ui_group_known[i].x == x && ui_group_known[i].y == y) return ui_group_known[i].v;
    return -1;
}
static void ui_group_remember(int v) {
    for (int i = 0; i < UI_KNOWN; i++)
        if (ui_group_known[i].z == ui_group.z && ui_group_known[i].x == ui_group.x0 && ui_group_known[i].y == ui_group.y0) {
            ui_group_known[i].v = v;
            return;
        }
    int i = (int)(ui_nknown++ % UI_KNOWN);
    ui_group_known[i].z = ui_group.z, ui_group_known[i].x = ui_group.x0, ui_group_known[i].y = ui_group.y0, ui_group_known[i].v = v;
}
// The anchor the group's pieces so far get: their own when they agree, else one from where the group sits (-1: agree)
static int ui_group_vanchor(void) {
    bool agree = true;
    for (int i = 1; i < ui_group.n; i++) agree &= ui_group.p[i].vanchor == ui_group.p[0].vanchor;
    if (agree) return -1;
    float above = ui_group.ymin, below = 720.0f - ui_group.ymax;  // (piece centres: the edges and corners lie on the frame)
    return above < 0.5f * below ? 0 : below < 0.5f * above ? 2 : 1;
}
static void ui_group_end(void) {
    int n = ui_group.n;
    if (!n) return;
    int v = n < 2 ? -1 : ui_group_vanchor();
    ui_group_remember(v >= 0 ? v : ui_group.p[0].vanchor);
    ui_group.n = 0;
    if (v < 0) return;
    float sy = g_render_aspect / (16.0f / 9.0f);
    for (int i = 0; i < n; i++) {
        float y = ui_group.p[i].pos_y;
        ui_group.p[i].m[13] = v == 0 ? y * sy : v == 2 ? 720.0f - (720.0f - y) * sy : 360.0f + (y - 360.0f) * sy;
        if (ui_nrecent - ui_group.p[i].recent <= UI_RECENT) ui_recent[ui_group.p[i].recent % UI_RECENT].vanchor = (uint8_t)v;
    }
    if (g_hudtrace_active) LOG("[hud] group z %.1f: %d pieces, y %.1f..%.1f -> %c\n", ui_group.z, n, ui_group.ymin, ui_group.ymax, "TCB"[v]);
}
static void ui_group_add(float *m, float pos_z, float pos_x, float pos_y, int vanchor) {
    // Not near: more than 400 above or below (a frame drawn as its corners only, the help windows' 336 high, is one group;
    // the target cam at y 42..82 and the Hunter Arts prompts below y 600 are two), or a piece of another anchor beside the
    // group both sideways and vertically (the Guild Card's page counter, top right, next to its page; a window's own
    // pieces line up with the rest of it on one side: its other corner, its bottom edge)
    const float margin = 24.0f;
    bool apart = !(ui_group.anchors & (1u << vanchor)) && (pos_x < ui_group.xmin - margin || pos_x > ui_group.xmax + margin) &&
                 (pos_y < ui_group.ymin - margin || pos_y > ui_group.ymax + margin);
    if (ui_group.n && (fabsf(pos_z - ui_group.z) > 0.5f || pos_y < ui_group.ymin - 400.0f || pos_y > ui_group.ymax + 400.0f ||
                       apart || ui_group.n == UI_GROUP_MAX))
        ui_group_end();
    if (!ui_group.n) {
        ui_group.z = pos_z, ui_group.x0 = ui_group.xmin = ui_group.xmax = pos_x, ui_group.y0 = ui_group.ymin = ui_group.ymax = pos_y;
        ui_group.anchors = 0;
        ui_group.predict = ui_group_lookup(pos_z, pos_x, pos_y);
    }
    ui_group.xmin = fminf(ui_group.xmin, pos_x), ui_group.xmax = fmaxf(ui_group.xmax, pos_x);
    ui_group.ymin = fminf(ui_group.ymin, pos_y), ui_group.ymax = fmaxf(ui_group.ymax, pos_y);
    ui_group.anchors |= (uint8_t)(1u << vanchor);
    ui_group.p[ui_group.n].m = m, ui_group.p[ui_group.n].pos_y = pos_y, ui_group.p[ui_group.n].vanchor = (uint8_t)vanchor;
    ui_group.p[ui_group.n].recent = ui_nrecent - 1;  // (cbuf_info recorded it just before)
    ui_group.n++;
}
static VkRenderPass pass_rp;
static float pass_sx = 1.0f, pass_sy = 1.0f;  // FSR: the pass renders into output-size stand-ins (fsr.c): viewport, scissor x

// Redundant-state filter: what draw() last set in the current command buffer (forgotten whenever something else may
// have recorded state: a new command buffer, a new pass, the clear pipeline).
enum { SS_PIPE = 1 << 0, SS_STEN = 1 << 1, SS_BC = 1 << 2, SS_DB = 1 << 3, SS_VP = 1 << 4, SS_SC = 1 << 5, SS_IB = 1 << 6 };
static struct {
    uint32_t known, vb_known;
    VkPipeline pipe;
    uint32_t sten[2][3];  // compare mask, write mask, reference per face
    float bc[4], db[3];
    VkViewport vp;
    VkRect2D sc;
    VkBuffer ib; VkDeviceSize io; VkIndexType it;
    VkBuffer vb[17]; VkDeviceSize vo[17];
} ss;
static void ss_forget(void) { ss.known = 0; ss.vb_known = 0; }
static inline bool ss_need(uint32_t bit, bool same) {
    if ((ss.known & bit) && same) return false;
    ss.known |= bit;
    return true;
}

// ---- (debug) pass dumps: MHGU_CONTROL "dump" saves colour target 0 of every render pass of the next frame as
// shots/pass_NNN_<format>_<w>x<h>.png (RGBA8 / BGRA8 / RGBA16F / B10G11R11; HDR values clamped to [0, 1] and also
// listed as the max in the log line).
volatile int g_dump_active;  // (device.c: set for one frame)
static char dump_draw_info[4096];  // the pass's last draw: program and fragment textures
static uint32_t level_extent(uint32_t v, uint32_t lv) { v >>= lv; return v ? v : 1; }
int g_dump_no, g_dump_frame;  // (pass number, frame of a multi-frame dump)
static const char *dump_prefix(void) { static char p[32]; snprintf(p, sizeof p, g_dump_frame ? "shots/f%d_pass_" : "shots/pass_", g_dump_frame); return p; }
static uint64_t pass_draws_at;

static float half_to_float(uint16_t h) {
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v = e == 0 ? ldexpf((float)m, -24) : e == 31 ? (m ? NAN : INFINITY) : ldexpf((float)(m | 1024), (int)e - 25);
    return s ? -v : v;
}
static float uf11(uint32_t v, int mbits) {  // unsigned small float: 5-bit exponent, mbits mantissa
    uint32_t e = v >> mbits, m = v & ((1u << mbits) - 1);
    if (e == 0) return ldexpf((float)m, -14 - mbits);
    if (e == 31) return m ? NAN : INFINITY;
    return ldexpf((float)(m | (1u << mbits)), (int)e - 15 - mbits);
}

// (the pass's own colour target 0: st.rt already names the next pass's targets when this one ends)
static NvnTexture *dump_tex, *dump_depth;
static uint32_t dump_lv, dump_ly;

static void dump_pass(void) {
    NvnTexture *t = dump_tex;
    uint64_t draws = stat_draws - pass_draws_at;
    if (t && t->backend) vk_ms_resolve(t->backend);  // (MSAA: resolved for the dump)
    if (dump_depth && dump_depth->backend) vk_ms_resolve(dump_depth->backend);
    if ((!t || !t->backend) && dump_depth && dump_depth->backend && ((TexBackend *)dump_depth->backend)->format == VK_FORMAT_D32_SFLOAT) {
        // depth-only pass (shadow maps): depth as grey, stretched over its own min..max
        TexBackend *b = dump_depth->backend;
        uint32_t w = b->width, h = b->height;
        VkCommandBuffer cb = vk_cmd();
        vk_full_barrier(cb);
        VkSpan out = vk_readback((uint64_t)w * h * 4);
        VkBufferImageCopy c = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
        vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
        renderer_submit();
        vk_wait_seq(submit_seq);
        const float *d = out.ptr;
        float lo = INFINITY, hi = -INFINITY;
        for (uint64_t i = 0; i < (uint64_t)w * h; i++) if (d[i] < 1.0f) { if (d[i] < lo) lo = d[i]; if (d[i] > hi) hi = d[i]; }
        uint8_t *px = malloc((size_t)w * h * 4);
        for (uint64_t i = 0; i < (uint64_t)w * h; i++) {
            float v = hi > lo ? (d[i] - lo) / (hi - lo) : 0;
            uint8_t g = d[i] >= 1.0f ? 255 : (uint8_t)(fminf(fmaxf(v, 0), 1) * 254.0f);
            px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = g; px[i * 4 + 3] = 255;
        }
        char path[128];
        mkdir("shots", 0755);
        snprintf(path, sizeof path, "%s%03d_depth_%ux%u.png", dump_prefix(), g_dump_no, w, h);
        vk_save_png(path, px, w, h);
        free(px);
        snprintf(path, sizeof path, "%s%03d_depth.raw", dump_prefix(), g_dump_no);  // (float32, w x h)
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(d, 4, (size_t)w * h, f); fclose(f); }
        LOG("[dump] pass %03d: depth only, %llu draws, range %g..%g | %s\n", g_dump_no++, (unsigned long long)draws, lo, hi, dump_draw_info);
        dump_draw_info[0] = 0;
        return;
    }
    if (!t || !t->backend) { LOG("[dump] pass %03d: no colour 0 (%llu draws)\n", g_dump_no++, (unsigned long long)draws); return; }
    TexBackend *b = t->backend;
    uint32_t lv = dump_lv, ly = dump_ly;
    uint32_t w = level_extent(b->width, lv), h = level_extent(b->height, lv), bpp;
    switch (b->format) {
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: bpp = 4; break;
    case VK_FORMAT_R16G16B16A16_SFLOAT: bpp = 8; break;
    default:
        LOG("[dump] pass %03d: format %d %ux%u (not dumped, %llu draws)\n", g_dump_no++, b->format, w, h, (unsigned long long)draws);
        return;
    }
    if (b->samples > 1) { LOG("[dump] pass %03d: multisampled (%llu draws)\n", g_dump_no++, (unsigned long long)draws); return; }
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkSpan out = vk_readback((uint64_t)w * h * bpp);
    VkBufferImageCopy c = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv, ly, 1}, .imageExtent = {w, h, 1}};
    vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
    renderer_submit();
    vk_wait_seq(submit_seq);
    uint8_t *px = malloc((size_t)w * h * 4), *ax = malloc((size_t)w * h * 4);
    float maxv = 0;
    for (uint64_t i = 0; i < (uint64_t)w * h; i++) {
        float f[4] = {0, 0, 0, 1};
        const uint8_t *s = (const uint8_t *)out.ptr + i * bpp;
        if (b->format == VK_FORMAT_R16G16B16A16_SFLOAT) {
            const uint16_t *q = (const uint16_t *)s;
            for (int k = 0; k < 4; k++) f[k] = half_to_float(q[k]);
        } else if (b->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32) {
            uint32_t v;
            memcpy(&v, s, 4);
            f[0] = uf11(v & 0x7FF, 6); f[1] = uf11((v >> 11) & 0x7FF, 6); f[2] = uf11(v >> 22, 5);
        } else {
            bool bgr = b->format == VK_FORMAT_B8G8R8A8_UNORM || b->format == VK_FORMAT_B8G8R8A8_SRGB;
            for (int k = 0; k < 4; k++) f[k] = s[bgr && k < 3 ? 2 - k : k] / 255.0f;
        }
        for (int k = 0; k < 3; k++) {
            if (f[k] != f[k]) f[k] = 1e9f;  // NaN: shown white, counted as huge
            if (f[k] > maxv) maxv = f[k];
            px[i * 4 + k] = (uint8_t)(fminf(fmaxf(f[k], 0), 1) * 255.0f + 0.5f);
        }
        px[i * 4 + 3] = 255;
        uint8_t a = (uint8_t)(fminf(fmaxf(f[3], 0), 1) * 255.0f + 0.5f);
        ax[i * 4] = ax[i * 4 + 1] = ax[i * 4 + 2] = a;
        ax[i * 4 + 3] = 255;
    }
    char path[128];
    mkdir("shots", 0755);
    snprintf(path, sizeof path, "%s%03d_f%d_%ux%u.png", dump_prefix(), g_dump_no, b->format, w, h);
    vk_save_png(path, px, w, h);
    snprintf(path, sizeof path, "%s%03d_alpha.png", dump_prefix(), g_dump_no);
    vk_save_png(path, ax, w, h);
    free(ax);
    LOG("[dump] pass %03d: %llu draws, nvn fmt %#x, max value %g | %s\n", g_dump_no, (unsigned long long)draws, t->format, maxv, dump_draw_info);
    dump_draw_info[0] = 0;
    g_dump_no++;
    free(px);
}

// ================================================================== (debug) GPU profile: MHGU_CONTROL "gpuprof"
// GPU timestamps between the passes (and MSAA resolves) of the next frame, logged in milliseconds by vk_gpuprof_frame.
volatile int g_gpuprof_request;
static int gp_active, gp_n, gp_pass;
static VkQueryPool gp_pool;
static char gp_label[256][64];

static void gp_mark(const char *label) {
    if (!gp_active || gp_n >= 256) return;
    vkCmdWriteTimestamp(vk_cmd(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, gp_pool, (uint32_t)gp_n);
    snprintf(gp_label[gp_n++], sizeof gp_label[0], "%s", label);
}

void vk_gpuprof_frame(void) {  // (device.c, after each presented frame's submission)
    if (gp_active) {
        gp_active = 0;
        uint64_t t[256];
        if (gp_n > 1 && vkGetQueryPoolResults(g_vk.dev, gp_pool, 0, (uint32_t)gp_n, sizeof t, t, 8,
                                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
            double per = g_vk.props.limits.timestampPeriod / 1e6;
            bool ok = t[gp_n - 1] > t[0];
            for (int i = 1; i < gp_n; i++) ok = ok && t[i] >= t[i - 1];
            if (!ok) LOG("[gpuprof] no usable timestamps this frame: ask again\n");
            for (int i = 1; ok && i < gp_n; i++) LOG("[gpuprof] %7.3f ms  %s\n", (t[i] - t[i - 1]) * per, gp_label[i]);
            if (ok) LOG("[gpuprof] %7.3f ms  frame\n", (t[gp_n - 1] - t[0]) * per);
        }
    }
    if (g_gpuprof_request) {
        g_gpuprof_request = 0;
        if (!gp_pool) {
            VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                        .queryCount = 256};
            VK_CHECK(vkCreateQueryPool(g_vk.dev, &qi, NULL, &gp_pool));
        }
        vkCmdResetQueryPool(vk_cmd(), gp_pool, 0, 256);
        gp_active = 1, gp_n = 0, gp_pass = 0;
        gp_mark("start");
    }
}

static bool pass_msaa;  // (MSAA, below) the pass renders through multisampled images (resolved when next read)
static TexBackend *pass_att[9];
static uint32_t pass_natt;

void vk_end_pass(void) {
    if (!in_pass) return;
    ui_group_end();
    if (ui_nrecent) ui_results_last = ui_results_pass;
    vkCmdEndRenderPass(vk_cmd());
    in_pass = false;
    if (gp_active) {
        char l[64];
        snprintf(l, sizeof l, "pass %d %ux%u%s, %llu draws", gp_pass++, pass_w, pass_h, pass_msaa ? " MSAA" : "",
                 (unsigned long long)(stat_draws - pass_draws_at));
        gp_mark(l);
    }
    if (pass_msaa) for (uint32_t k = 0; k < pass_natt; k++) pass_att[k]->ms_dirty = true;
    pass_msaa = false;
    if (g_dump_active) dump_pass();
}


// ================================================================== MSAA
// With MSAA on (settings), the 3D scene's passes (those whose depth target has the 3D resolution) render into
// multisampled companions of their targets (TexBackend.ms). A companion is used only while it holds its target's
// contents (ms_valid): written last by such a pass, or cleared whole at the start of this one. Any other write to the
// target (a pass without MSAA, a copy) makes it stale, and a pass with a stale companion that isn't cleared first
// renders without MSAA. The target itself is brought up to date (resolved: vk_ms_resolve) only when something reads
// it - a draw or dispatch sampling it, a copy, a pass without MSAA, presentation - so the scene's passes in a row
// share one resolve: on the R9 270X a resolve is the expensive part of MSAA.
static uint64_t cur_rec;  // guest address of the command being executed (exec_list)
static int rt_kind(uint32_t f);

// Attachments of the pass about to begin that the commands from cur_rec on clear whole before anything else (bit i:
// colour target i, bit 8: depth, and stencil too where there is stencil).
static uint32_t leading_clears(uint32_t w, uint32_t h, bool stencil) {
    if (!cur_rec) return 0;
    int32_t sc[4];
    memcpy(sc, st.scissor[0], sizeof sc);
    uint32_t got = 0;
    uint64_t p = cur_rec;
    for (int n = 0; n < 64; n++) {
        const CmdRec *r = (const CmdRec *)G2H((uint32_t)p);
        bool full = sc[0] <= 0 && sc[1] <= 0 && (int64_t)sc[0] + sc[2] >= w && (int64_t)sc[1] + sc[3] >= h;
        switch (r->op) {
        case CMD_JUMP: p = r->x[0]; continue;
        case CMD_SET_SCISSORS: if (r->x[0] == 0 && r->x[1]) memcpy(sc, r->extra, sizeof sc); break;
        case CMD_CLEAR_COLOR: if (full && (r->x[2] & 0xF) == 0xF && r->x[0] < 8) got |= 1u << r->x[0]; break;
        case CMD_CLEAR_DEPTH_STENCIL: if (full && r->x[0] && (!stencil || (r->x[2] & 0xFF) == 0xFF)) got |= 1u << 8; break;
        case CMD_UPDATE_UNIFORM_BUFFER: case CMD_REPORT_COUNTER: break;
        default:
            if (r->op >= CMD_BIND_PROGRAM && r->op <= CMD_SET_DEPTH_CLAMP && r->op != CMD_SET_RENDER_TARGETS) break;
            return got;  // (a draw, a copy, other targets...)
        }
        p += r->size;
    }
    return got;
}

// vk_ms_resolve: a target brought up to date from its multisampled image. Colour: vkCmdResolveImage (the samples
// averaged). Depth / stencil: sample 0, by a shader from a sampleable copy (the image itself must stay unsampleable to
// keep its compression; Vulkan 1.2 render pass depth resolves were no faster). Measured on the R9 270X, 1080p, 4x:
// ~1.6 ms per colour target, ~9 ms for the depth / stencil target (mostly the driver decompressing it), so the scene's
// passes share one resolve per target (see above).
static VkDescriptorSetLayout rs_set_layout;
static VkPipelineLayout rs_layout;

static VkPipeline resolve_pipeline(VkRenderPass rp, int kind) {  // kind 1 depth, 2 depth + stencil
    static struct { VkRenderPass rp; int kind; VkPipeline p; } tab[32];
    static int n;
    for (int i = 0; i < n; i++) if (tab[i].rp == rp && tab[i].kind == kind) return tab[i].p;
    if (!rs_layout) {
        VkDescriptorSetLayoutBinding b[2] = {
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
            {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL}};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2,
                                              .pBindings = b};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &rs_set_layout));
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                         .pSetLayouts = &rs_set_layout};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &rs_layout));
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("resolve.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader(kind == 2 ? "resolve.frag.depth.s" : "resolve.frag.depth"),
         .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1,
                                            .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkStencilOpState so = {VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE, VK_COMPARE_OP_ALWAYS, 0xFF, 0xFF, 0};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                .depthTestEnable = VK_TRUE, .depthWriteEnable = VK_TRUE,
                                                .depthCompareOp = VK_COMPARE_OP_ALWAYS, .stencilTestEnable = kind == 2,
                                                .front = so, .back = so};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2,
                                           .pDynamicStates = dyn};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
                                       .pViewportState = &vs, .pRasterizationState = &rs, .pMultisampleState = &ms,
                                       .pDepthStencilState = &ds, .pColorBlendState = &cb, .pDynamicState = &dy,
                                       .layout = rs_layout, .renderPass = rp};
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p) != VK_SUCCESS) LOG("[vk] resolve pipeline failed\n");
    if (n < 32) tab[n].rp = rp, tab[n].kind = kind, tab[n++].p = p;
    return p;
}

void vk_ms_resolve(TexBackend *b) {
    if (!b || !b->ms_dirty) return;
    b->ms_dirty = false;
    TexBackend *m = b->ms;
    if (!m) return;
    if (in_pass) { vk_end_pass(); st.rt_dirty = true; }
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    bool depth = b->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    VkImageLayout att = depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkImageMemoryBarrier ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_ALL_WRITES,
                               .dstAccessMask = VK_ACCESS_ALL, .oldLayout = att, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = m->image, .subresourceRange = {m->aspect, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_STAGES_ALL, VK_STAGES_ALL, 0, 0, NULL, 0, NULL, 1, &ib);
    TexBackend *r = NULL;
    if (!depth) {  // colour: the GPU's own resolve (averages the samples)
        VkImageResolve rv = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                             .extent = {b->width, b->height, 1}};
        vkCmdResolveImage(cb, m->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rv);
    } else if ((r = vk_tex_ms_readable(m))) {  // depth / stencil: copied where a shader can read it (below)
        VkImageCopy c = {.srcSubresource = {m->aspect, 0, 0, 1}, .dstSubresource = {m->aspect, 0, 0, 1},
                         .extent = {b->width, b->height, 1}};
        vkCmdCopyImage(cb, m->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    }
    ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ib.newLayout = att;
    ib.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_STAGES_ALL, VK_STAGES_ALL, 0, 0, NULL, 0, NULL, 1, &ib);
    if (r) {  // depth (and stencil) of sample 0 written by a shader
        vk_full_barrier(cb);
        static const VkComponentMapping id = {0};
        VkFormat f = b->ms_fmt, none = VK_FORMAT_UNDEFINED;
        bool sten = (r->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) && g_vk.stencil_export;
        VkRenderPass rp = vk_render_pass(&none, 0, f, 1);
        VkPipeline p = resolve_pipeline(rp, sten ? 2 : 1);
        VkImageView dst = vk_image_view(b, VK_IMAGE_VIEW_TYPE_2D, f, 0, 1, 0, 1, id, b->aspect);
        if (!p || !dst) return;
        VkDescriptorSet set = vk_alloc_set(rs_set_layout);
        VkDescriptorImageInfo ii[2] = {
            {vk_default_sampler(false),
             vk_image_view(r, VK_IMAGE_VIEW_TYPE_2D, r->format, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_DEPTH_BIT), VK_IMAGE_LAYOUT_GENERAL},
            {vk_default_sampler(false),
             sten ? vk_image_view(r, VK_IMAGE_VIEW_TYPE_2D, r->format, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_STENCIL_BIT) : VK_NULL_HANDLE,
             VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet w[2] = {
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[0]},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[1]}};
        vkUpdateDescriptorSets(g_vk.dev, sten ? 2 : 1, w, 0, NULL);
        VkRect2D area = {{0, 0}, {b->width, b->height}};
        VkRenderPassBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp,
                                    .framebuffer = vk_framebuffer(rp, &dst, 1, b->width, b->height), .renderArea = area};
        vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, rs_layout, 0, 1, &set, 0, NULL);
        VkViewport vp = {0, 0, (float)b->width, (float)b->height, 0, 1};
        vkCmdSetViewport(cb, 0, 1, &vp);
        vkCmdSetScissor(cb, 0, 1, &area);
        vkCmdDraw(cb, 3, 1, 0, 0);
        vkCmdEndRenderPass(cb);
        ss_forget();
    }
    gp_mark("  resolve");
}

// Before a draw / dispatch: the textures it samples up to date (attachments of the pass: resolve_feedback).
static NvnTexture *shader_texture_object(int stage, ShaderTexture *t);
static void ms_resolve_sampled(int stage, ShaderInfo *si) {
    for (int i = 0; i < si->ntex; i++) {
        NvnTexture *t = shader_texture_object(stage, &si->tex[i]);
        if (t && t->backend && ((TexBackend *)t->backend)->ms_dirty) vk_ms_resolve(t->backend);
    }
}

static void begin_pass(void) {
    if (in_pass && !st.rt_dirty) return;
    vk_end_pass();
    st.rt_dirty = false;
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);  // passes, copies and dispatches see each other's results
    VkImageView views[9];
    TexBackend *att[9];  // per view: its image, the format it's rendered as, colour index (-1 depth)
    VkFormat att_fmt[9];
    int att_index[9];
    uint32_t msaa = g_vk.msaa;
    bool ms_ok = msaa > 1;  // (every attachment one whole level 0 / layer 0, not an integer format)
    uint32_t nviews = 0, minw = UINT32_MAX, minh = UINT32_MAX;
    pass_ncolor = (uint32_t)st.rt.ncolor;
    pass_samples = 1;
    pass_sx = pass_sy = 1.0f;
    for (int i = 0; i < st.rt.ncolor; i++) {
        NvnTexture *t = st.rt.color[i];
        pass_color[i] = VK_FORMAT_UNDEFINED;
        if (!t) continue;
        if (g_fsr_on) {  // FSR: from the start of the HUD on, an output-size stand-in (fsr.c)
            NvnTexture *s = vk_fsr_map(t, false);
            if (s != t) pass_sx = (float)s->width / (float)t->width, pass_sy = (float)s->height / (float)t->height, t = s;
        }
        renderer_mark_gpu_owned(t);
        VkImageView v = vk_view(t, &st.rt.cview[i], st.rt.has_cview[i], true, TEXTYPE_2D);
        TexBackend *b = t->backend;
        if (!v) continue;
        pass_color[i] = st.rt.has_cview[i] && (st.rt.cview[i].setmask & 4) && vk_format(t->format) == b->format
                      ? vk_format(st.rt.cview[i].format) : b->format;
        att[nviews] = b, att_fmt[nviews] = pass_color[i], att_index[nviews] = i;
        views[nviews++] = v;
        uint32_t lv = st.rt.has_cview[i] && (st.rt.cview[i].setmask & 1) ? st.rt.cview[i].base_level : 0;
        uint32_t ly = st.rt.has_cview[i] && (st.rt.cview[i].setmask & 2) ? st.rt.cview[i].min_layer : 0;
        ms_ok = ms_ok && !lv && !ly && b->samples == 1 && !rt_kind(t->format);
        uint32_t ew = level_extent(b->width, lv), eh = level_extent(b->height, lv);
        if (ew < minw) minw = ew;
        if (eh < minh) minh = eh;
        pass_samples = b->samples;
    }
    pass_depth = VK_FORMAT_UNDEFINED;
    NvnTexture *dt = st.rt.depth && pass_sx != 1.0f ? vk_fsr_map(st.rt.depth, true) : st.rt.depth;  // (FSR: as above)
    if (dt) {
        renderer_mark_gpu_owned(dt);
        VkImageView v = vk_view(dt, &st.rt.dview, st.rt.has_dview, true, TEXTYPE_2D);
        TexBackend *b = dt->backend;
        if (v && renderer_format_is_depth(dt->format) && b->aspect != VK_IMAGE_ASPECT_COLOR_BIT) {
            pass_depth = b->format;
            att[nviews] = b, att_fmt[nviews] = b->format, att_index[nviews] = -1;
            views[nviews++] = v;
            uint32_t lv = st.rt.has_dview && (st.rt.dview.setmask & 1) ? st.rt.dview.base_level : 0;
            uint32_t ly = st.rt.has_dview && (st.rt.dview.setmask & 2) ? st.rt.dview.min_layer : 0;
            ms_ok = ms_ok && !lv && !ly && b->samples == 1;
            uint32_t ew = level_extent(b->width, lv), eh = level_extent(b->height, lv);
            if (ew < minw) minw = ew;
            if (eh < minh) minh = eh;
            if (!pass_ncolor) pass_samples = b->samples;
        }
    }
    if (minw == UINT32_MAX) minw = minh = 16;
    pass_w = minw; pass_h = minh;
    ui_loading_pass = false;
    ui_title_pass = false;
    ui_results_pass = false;
    ui_menu_pass = false;
    ui_nrecent = 0;
    pass_msaa = false;
    if (ms_ok && pass_depth != VK_FORMAT_UNDEFINED && minw == g_render_size[0] && minh == g_render_size[1]) {
        bool stencil = pass_depth == VK_FORMAT_D24_UNORM_S8_UINT || pass_depth == VK_FORMAT_D32_SFLOAT_S8_UINT;
        uint32_t cleared = leading_clears(minw, minh, stencil);
        bool ok = true;
        for (uint32_t k = 0; k < nviews && ok; k++) {
            TexBackend *b = att[k];
            bool clr = (cleared >> (att_index[k] < 0 ? 8 : att_index[k])) & 1;
            ok = b->width == minw && b->height == minh && (clr || (b->ms && b->ms_valid && b->ms->samples == msaa));
        }
        VkImageView msv[9];
        VkFormat ms_depth_fmt = pass_depth;
        for (uint32_t k = 0; k < nviews && ok; k++) {
            TexBackend *m = vk_tex_ms(att[k], msaa);
            ok = m && (msv[k] = vk_image_view(m, VK_IMAGE_VIEW_TYPE_2D, att_index[k] < 0 ? m->format : att_fmt[k], 0, 1, 0, 1,
                                              (VkComponentMapping){0}, m->aspect));
            if (ok && att_index[k] < 0) ms_depth_fmt = m->format;
        }
        if (ok) {  // rendered into the multisampled images (resolved at the end: vk_end_pass)
            memcpy(views, msv, nviews * sizeof *views);
            memcpy(pass_att, att, nviews * sizeof *att);
            pass_natt = nviews;
            for (uint32_t k = 0; k < nviews; k++) att[k]->ms_fmt = att_fmt[k];
            pass_samples = msaa;
            pass_depth = ms_depth_fmt;
            pass_msaa = true;
        }
    }
    for (uint32_t k = 0; k < nviews; k++) {
        if (!pass_msaa) vk_ms_resolve(att[k]);  // (rendered into directly: up to date first)
        att[k]->ms_valid = pass_msaa;
    }
    pass_rp = vk_render_pass(pass_color, pass_ncolor, pass_depth, pass_samples);
    if (!nviews) {  // nothing to render into: a pass without attachments still needs a framebuffer size
        static VkImageView none;
        (void)none;
    }
    VkFramebuffer fb = vk_framebuffer(pass_rp, views, nviews, minw, minh);
    gp_mark("  (between passes)");
    VkRenderPassBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = pass_rp, .framebuffer = fb,
                                .renderArea = {{0, 0}, {minw, minh}}};
    vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    ss_forget();
    in_pass = true;
    pass_draws_at = stat_draws;
    if (g_dump_active) {  // (debug) the pass's targets: texture object, size, NVN format
        char tg[400];
        int n = 0;
        for (int i = 0; i < st.rt.ncolor && n < 300; i++)
            if (st.rt.color[i]) n += snprintf(tg + n, sizeof tg - n, " c%d %p %ux%u f%#x", i, (void *)st.rt.color[i],
                                              st.rt.color[i]->width, st.rt.color[i]->height, st.rt.color[i]->format);
        if (st.rt.depth) snprintf(tg + n, sizeof tg - n, " d %p %ux%u f%#x", (void *)st.rt.depth, st.rt.depth->width,
                                  st.rt.depth->height, st.rt.depth->format);
        else tg[n] = 0;
        LOG("[dump] begin pass %03d:%s%s\n", g_dump_no, tg, pass_msaa ? " (MSAA)" : "");
    }
    dump_tex = st.rt.ncolor ? st.rt.color[0] : NULL;
    dump_depth = st.rt.depth;
    dump_lv = st.rt.has_cview[0] && (st.rt.cview[0].setmask & 1) ? st.rt.cview[0].base_level : 0;
    dump_ly = st.rt.has_cview[0] && (st.rt.cview[0].setmask & 2) ? st.rt.cview[0].min_layer : 0;
    stat_passes++;
}

static void pass_size(int *w, int *h) {
    *w = *h = 0;
    for (int i = 0; i < st.rt.ncolor && !*w; i++)
        if (st.rt.color[i]) { *w = (int)st.rt.color[i]->width; *h = (int)st.rt.color[i]->height; }
    if (!*w && st.rt.depth) { *w = (int)st.rt.depth->width; *h = (int)st.rt.depth->height; }
    if (pass_sx != 1.0f) *w = (int)(*w * pass_sx + 0.5f), *h = (int)(*h * pass_sy + 0.5f);  // (FSR: output-size stand-ins)
}

volatile int g_shadow_scissor_mode;  // (debug, F8 in host_input.c) depth-only passes: 0 as set, 1 none, 2 mirrored in the viewport

// The pass's size as the guest sees it: its viewport and scissor (FSR: the output-size stand-ins scaled back by pass_s*)
static void pass_guest_size(float *w, float *h) { *w = roundf((float)pass_w / pass_sx), *h = roundf((float)pass_h / pass_sy); }

// A 3D model shown inside a HUD window (the Smithy's weapon / armor preview, the equipment screens) is drawn through a
// viewport and scissor the game sized for its 1280x720 canvas, spread over the whole picture. The window itself is moved
// and squeezed by cbuf_info (the same anchoring), so the model would stay where the window was before it moved and spill
// past its frame: on a picture wider than 16:9 sideways, on a narrower one (16:10) up or down. x' = sx + x * kx (and
// likewise y) puts the rectangle where the window went: sideways left / right / centre by where it is, vertically as the
// window's frame (the last HUD quad drawn over the viewport, ui_recent) was anchored. The scissor follows it exactly; the
// viewport keeps its size and only its centre moves (the game's projection is made for that shape; squeezing it squeezes
// the model).
// Returns false when the draw is not such a rectangle.
typedef struct { float sx, kx, sy, ky; } PreviewMap;
static bool ui_preview_map(PreviewMap *pm) {
    float asp = g_render_aspect, pw, ph;
    pass_guest_size(&pw, &ph);
    if (fabsf(asp - 16.0f / 9.0f) <= 0.01f || !st.ds[0] || !pass_w || ph != (float)g_render_size[1]) return false;
    ui_group_end();  // (the window drawn before it is complete: its anchor is the one to follow)
    const float *vp = st.viewport[0];
    float w = vp[2], h = fabsf(vp[3]);
    if (w < 8.0f || w > 0.9f * pw || h > 0.98f * ph) return false;  // (the scene itself: the whole picture)
    *pm = (PreviewMap){0, 1, 0, 1};
    // The window it is drawn in: the last quad over the viewport (inside it sideways, not below it). The last quad of all
    // can be a button prompt under the window ("Rotate" under the Palico Scout preview), anchored apart from it.
    int vanchor = ui_nrecent ? ui_recent[(ui_nrecent - 1) % UI_RECENT].vanchor : 1;
    {
        float x0 = vp[0] / pw * 1280.0f, x1 = (vp[0] + w) / pw * 1280.0f, y1 = (fminf(vp[1], vp[1] + vp[3]) + h) / ph * 720.0f;
        for (uint32_t i = ui_nrecent; i > 0 && ui_nrecent - i < UI_RECENT; i--) {
            uint32_t k = (i - 1) % UI_RECENT;
            if (ui_recent[k].x >= x0 && ui_recent[k].x <= x1 && ui_recent[k].y <= y1) { vanchor = ui_recent[k].vanchor; break; }
        }
    }
    if (asp > 16.0f / 9.0f) {
        float s = (16.0f / 9.0f) / asp;
        float anchor = g_ui_anchor_aspect < asp ? g_ui_anchor_aspect : asp;
        float left_px = 640.0f * (1.0f - anchor / asp) * (pw / 1280.0f);
        float cx = (vp[0] + w * 0.5f) / pw * 1280.0f;
        pm->kx = s;
        if (cx <= 430.0f) pm->sx = left_px;
        else if (cx >= 650.0f) pm->sx = pw - left_px - pw * s;
        else pm->sx = pw * 0.5f * (1.0f - s);
    } else {
        float sy = asp / (16.0f / 9.0f);
        pm->ky = sy;
        if (vanchor == 0) pm->sy = 0;
        else if (vanchor == 2) pm->sy = ph * (1.0f - sy);
        else pm->sy = ph * 0.5f * (1.0f - sy);
    }
    if (g_hudtrace_active)
        LOG("[hud] preview vp %.0f,%.0f %.0fx%.0f sc %d,%d %dx%d (pass %.0fx%.0f) -> sx %.1f kx %.3f sy %.1f ky %.3f\n", vp[0], vp[1], w, h,
            st.scissor[0][0], st.scissor[0][1], st.scissor[0][2], st.scissor[0][3], pw, ph, pm->sx, pm->kx, pm->sy, pm->ky);
    return true;
}

static VkRect2D scissor_rect(void) {
    int w, h;
    pass_size(&w, &h);
    if (!w) return (VkRect2D){{0, 0}, {pass_w, pass_h}};
    int x = st.scissor[0][0], y = st.scissor[0][1], sw = st.scissor[0][2], sh = st.scissor[0][3];
    if (ui_relax_scissor) { x = y = 0; sw = w; sh = h; }
    if (ui_scissor_v >= 0 && !ui_relax_scissor) {  // (16:10: a HUD piece's scissor moves with it, unless it is the whole pass)
        float pw, ph;
        pass_guest_size(&pw, &ph);
        if (x > 0 || y > 0 || sw < (int)pw || sh < (int)ph) {
            float sy = g_render_aspect / (16.0f / 9.0f), y0 = (float)y, y1 = (float)(y + sh);
            float off = ui_scissor_v == 0 ? 0.0f : ui_scissor_v == 2 ? ph * (1.0f - sy) : ph * 0.5f * (1.0f - sy);
            y = (int)floorf(off + y0 * sy);
            sh = (int)ceilf(off + y1 * sy) - y;
        }
    }
    {
        PreviewMap pm;
        if (!ui_relax_scissor && ui_preview_map(&pm)) {
            float x0 = pm.sx + x * pm.kx, x1 = pm.sx + (x + sw) * pm.kx, y0 = pm.sy + y * pm.ky, y1 = pm.sy + (y + sh) * pm.ky;
            x = (int)lroundf(x0);
            sw = (int)lroundf(x1 - x0);
            y = (int)lroundf(y0);
            sh = (int)lroundf(y1 - y0);
        }
    }
    if (!st.rt.ncolor && g_shadow_scissor_mode == 1) { x = (int)st.viewport[0][0]; y = (int)st.viewport[0][1]; sw = (int)st.viewport[0][2]; sh = (int)st.viewport[0][3]; }
    if (!st.rt.ncolor && g_shadow_scissor_mode == 2) y = 2 * (int)st.viewport[0][1] + (int)st.viewport[0][3] - y - sh;
    if (pass_sx != 1.0f) {  // FSR: the guest's rectangle (render size) on the output-size stand-in
        double x1 = floor(x * (double)pass_sx), y1 = floor(y * (double)pass_sy);
        double x2 = ceil(((double)x + sw) * pass_sx), y2 = ceil(((double)y + sh) * pass_sy);
        x = (int)x1, y = (int)y1;
        sw = (int)fmin(x2 - x1, 1 << 30), sh = (int)fmin(y2 - y1, 1 << 30);
    }
    if (x < 0) { sw += x; x = 0; }
    if (y < 0) { sh += y; y = 0; }
    if (w > (int)pass_w) w = (int)pass_w;
    if (h > (int)pass_h) h = (int)pass_h;
    if (x >= w || y >= h) { x = y = 0; sw = sh = 0; }
    if (x + sw > w || sw > w) sw = w - x;
    if (y + sh > h || sh > h) sh = h - y;
    if (sw < 0) sw = 0;
    if (sh < 0) sh = 0;
    return (VkRect2D){{x, y}, {(uint32_t)sw, (uint32_t)sh}};
}

static void apply_scissor(VkCommandBuffer cb) {
    VkRect2D r = scissor_rect();
    if (!ss_need(SS_SC, !memcmp(&r, &ss.sc, sizeof r))) return;
    ss.sc = r;
    vkCmdSetScissor(cb, 0, 1, &r);
}

// The translator's clip space is Metal's (y up in NDC, origin top-left): a negative-height viewport.
static void set_viewport(VkCommandBuffer cb, float x, float y, float w, float h, float zn, float zf) {
    if (w <= 0) w = 1;
    if (h == 0) h = 1;
    VkViewport vp = {x, y + h, w, -h, fminf(fmaxf(zn, 0), 1), fminf(fmaxf(zf, 0), 1)};
    if (!ss_need(SS_VP, !memcmp(&vp, &ss.vp, sizeof vp))) return;
    ss.vp = vp;
    vkCmdSetViewport(cb, 0, 1, &vp);
}

// ================================================================== fixed-function tables
static const VkDynamicState DYN[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS,
                                     VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                                     VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE};
#define NDYN (sizeof DYN / sizeof DYN[0])

static int attr_kind(uint32_t fmt) {
    switch (fmt) {
    case 0x03: case 0x08: case 0x0B: case 0x0F: case 0x14: case 0x17: case 0x1B: case 0x20: case 0x23:
    case 0x27: case 0x2C: case 0x2F: case 0x3E:
        return 2;
    case 0x04: case 0x09: case 0x0C: case 0x10: case 0x15: case 0x18: case 0x1C: case 0x21: case 0x24:
    case 0x28: case 0x2D: case 0x30:
        return 1;
    default: return 0;
    }
}

static VkFormat vertex_format(uint32_t f) {
    static const VkFormat tab[0x31] = {
        [0x01] = VK_FORMAT_R8_UNORM, [0x02] = VK_FORMAT_R8_SNORM, [0x03] = VK_FORMAT_R8_UINT, [0x04] = VK_FORMAT_R8_SINT,
        [0x05] = VK_FORMAT_R16_SFLOAT, [0x06] = VK_FORMAT_R16_UNORM, [0x07] = VK_FORMAT_R16_SNORM, [0x08] = VK_FORMAT_R16_UINT,
        [0x09] = VK_FORMAT_R16_SINT, [0x0A] = VK_FORMAT_R32_SFLOAT, [0x0B] = VK_FORMAT_R32_UINT, [0x0C] = VK_FORMAT_R32_SINT,
        [0x0D] = VK_FORMAT_R8G8_UNORM, [0x0E] = VK_FORMAT_R8G8_SNORM, [0x0F] = VK_FORMAT_R8G8_UINT, [0x10] = VK_FORMAT_R8G8_SINT,
        [0x11] = VK_FORMAT_R16G16_SFLOAT, [0x12] = VK_FORMAT_R16G16_UNORM, [0x13] = VK_FORMAT_R16G16_SNORM,
        [0x14] = VK_FORMAT_R16G16_UINT, [0x15] = VK_FORMAT_R16G16_SINT, [0x16] = VK_FORMAT_R32G32_SFLOAT,
        [0x17] = VK_FORMAT_R32G32_UINT, [0x18] = VK_FORMAT_R32G32_SINT, [0x19] = VK_FORMAT_R8G8B8_UNORM,
        [0x1A] = VK_FORMAT_R8G8B8_SNORM, [0x1B] = VK_FORMAT_R8G8B8_UINT, [0x1C] = VK_FORMAT_R8G8B8_SINT,
        [0x1D] = VK_FORMAT_R16G16B16_SFLOAT, [0x1E] = VK_FORMAT_R16G16B16_UNORM, [0x1F] = VK_FORMAT_R16G16B16_SNORM,
        [0x20] = VK_FORMAT_R16G16B16_UINT, [0x21] = VK_FORMAT_R16G16B16_SINT, [0x22] = VK_FORMAT_R32G32B32_SFLOAT,
        [0x23] = VK_FORMAT_R32G32B32_UINT, [0x24] = VK_FORMAT_R32G32B32_SINT, [0x25] = VK_FORMAT_R8G8B8A8_UNORM,
        [0x26] = VK_FORMAT_R8G8B8A8_SNORM, [0x27] = VK_FORMAT_R8G8B8A8_UINT, [0x28] = VK_FORMAT_R8G8B8A8_SINT,
        [0x29] = VK_FORMAT_R16G16B16A16_SFLOAT, [0x2A] = VK_FORMAT_R16G16B16A16_UNORM, [0x2B] = VK_FORMAT_R16G16B16A16_SNORM,
        [0x2C] = VK_FORMAT_R16G16B16A16_UINT, [0x2D] = VK_FORMAT_R16G16B16A16_SINT, [0x2E] = VK_FORMAT_R32G32B32A32_SFLOAT,
        [0x2F] = VK_FORMAT_R32G32B32A32_UINT, [0x30] = VK_FORMAT_R32G32B32A32_SINT};
    if (f < 0x31 && tab[f]) return tab[f];
    switch (f) {
    case 0x3D: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 0x69: return VK_FORMAT_A2B10G10R10_SNORM_PACK32;
    case 0x3F: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case 0x40: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    case 0x51: return VK_FORMAT_R8_UINT;
    default: return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
}

// 3-component 8/16-bit vertex formats aren't universally supported: read 4 components instead.
static VkFormat vertex_format_supported(VkFormat f) {
    static uint8_t known[256];  // 1 ok, 2 not
    uint32_t i = f < 256 ? f : 0;
    if (!known[i]) {
        VkFormatProperties p;
        vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &p);
        known[i] = (p.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) ? 1 : 2;
    }
    if (known[i] == 1) return f;
    switch (f) {
    case VK_FORMAT_R8G8B8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8_SNORM: return VK_FORMAT_R8G8B8A8_SNORM;
    case VK_FORMAT_R8G8B8_UINT: return VK_FORMAT_R8G8B8A8_UINT;
    case VK_FORMAT_R8G8B8_SINT: return VK_FORMAT_R8G8B8A8_SINT;
    case VK_FORMAT_R16G16B16_SFLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case VK_FORMAT_R16G16B16_UNORM: return VK_FORMAT_R16G16B16A16_UNORM;
    case VK_FORMAT_R16G16B16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM;
    case VK_FORMAT_R16G16B16_UINT: return VK_FORMAT_R16G16B16A16_UINT;
    case VK_FORMAT_R16G16B16_SINT: return VK_FORMAT_R16G16B16A16_SINT;
    default: return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
}

static VkBlendFactor blend_factor(uint8_t f) {
    switch (f) {
    case 1: return VK_BLEND_FACTOR_ZERO;
    case 2: return VK_BLEND_FACTOR_ONE;
    case 3: return VK_BLEND_FACTOR_SRC_COLOR;
    case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_DST_ALPHA;
    case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 9: return VK_BLEND_FACTOR_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x10: return VK_BLEND_FACTOR_SRC1_COLOR;
    case 0x11: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case 0x12: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case 0x13: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: {
        static uint8_t warned[256];
        if (!warned[f]++) LOG("[vk] unknown blend factor %#x\n", f);
        return VK_BLEND_FACTOR_ONE;
    }
    }
}

static VkBlendOp blend_op(uint8_t e) {
    switch (e) {
    case 2: return VK_BLEND_OP_SUBTRACT;
    case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 4: return VK_BLEND_OP_MIN;
    case 5: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}

static VkCompareOp compare_fn(uint8_t f) {
    switch (f) {
    case 1: return VK_COMPARE_OP_NEVER;
    case 2: return VK_COMPARE_OP_LESS;
    case 3: return VK_COMPARE_OP_EQUAL;
    case 4: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case 5: return VK_COMPARE_OP_GREATER;
    case 6: return VK_COMPARE_OP_NOT_EQUAL;
    case 7: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

static VkStencilOp stencil_op(uint8_t o) {
    switch (o) {
    case 2: return VK_STENCIL_OP_ZERO;
    case 3: return VK_STENCIL_OP_REPLACE;
    case 4: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 5: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 6: return VK_STENCIL_OP_INVERT;
    case 7: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 8: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;
    }
}

static int rt_kind(uint32_t f) {
    switch (f) {
    case 0x03: case 0x08: case 0x0B: case 0x0F: case 0x14: case 0x17: case 0x27: case 0x2C: case 0x2F: case 0x3E: return 2;
    case 0x04: case 0x09: case 0x0C: case 0x10: case 0x15: case 0x18: case 0x28: case 0x2D: case 0x30: return 1;
    default: return 0;
    }
}

static uint64_t hash_words(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (; n >= 8; n -= 8, b += 8) { uint64_t w; memcpy(&w, b, 8); h = (h ^ w) * 0x9E3779B97F4A7C15ull; h ^= h >> 29; }
    for (; n; n--, b++) { h = (h ^ *b) * 0x100000001B3ull; }
    return h ^ (h >> 32);
}

// ================================================================== clears
static VkPipelineLayout pc_layout(void) {  // push constants only (clears)
    static VkPipelineLayout l;
    if (!l) {
        VkPushConstantRange r = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1,
                                         .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &l));
    }
    return l;
}

// A masked clear (some channels / stencil bits only): a fullscreen triangle with the masks in the pipeline.
static VkPipeline clear_pipeline(int index, int kind, uint32_t mask, bool depth, bool stencil, uint32_t smask) {
    static struct { uint64_t key; VkPipeline pipe; } tab[256];
    uint64_t key = 0xcbf29ce484222325ull;
    uint64_t parts[] = {(uint64_t)index, (uint64_t)kind, mask, depth, stencil, smask, (uint64_t)(uintptr_t)pass_rp, pass_ncolor};
    key = hash_words(key, parts, sizeof parts);
    if (!key) key = 1;
    uint32_t slot = (uint32_t)key & 255;
    for (int p = 0; p < 256; p++, slot = (slot + 1) & 255) {
        if (tab[slot].key == key) return tab[slot].pipe;
        if (!tab[slot].key) break;
    }
    char fsname[32];
    if (index >= 0) snprintf(fsname, sizeof fsname, "clear.frag.%c%d", kind == 1 ? 'i' : kind == 2 ? 'u' : 'f', index);
    else snprintf(fsname, sizeof fsname, "clear.frag.none");
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("clear.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader(fsname), .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = (VkSampleCountFlagBits)pass_samples};
    VkStencilOpState so = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE, VK_COMPARE_OP_ALWAYS, 0xFF, smask, 0};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                .depthTestEnable = depth, .depthWriteEnable = depth, .depthCompareOp = VK_COMPARE_OP_ALWAYS,
                                                .stencilTestEnable = stencil, .front = so, .back = so};
    VkPipelineColorBlendAttachmentState cba[8] = {0};
    for (uint32_t i = 0; i < pass_ncolor; i++)
        if ((int)i == index) cba[i].colorWriteMask = mask & 0xF;
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = pass_ncolor, .pAttachments = cba};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 3, .pDynamicStates = dyn};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = pc_layout(), .renderPass = pass_rp};
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p) != VK_SUCCESS) LOG("[vk] clear pipeline failed\n");
    tab[slot].key = key;
    tab[slot].pipe = p;
    return p;
}

volatile int g_clear_draw = -1;  // colour clears as draws (MHGU_CLEAR_DRAW, F9 toggles; host_input.c)

static void clear(int index, int kind, const void *color, uint32_t mask, bool depth, float dval, bool stencil, uint32_t sval,
                  uint32_t smask) {
    begin_pass();
    if (g_dump_active) LOG("[dump]   clear %d mask %#x depth %d stencil %d (after %llu draws)\n", index, mask, depth, stencil,
                           (unsigned long long)(stat_draws - pass_draws_at));
    VkCommandBuffer cb = vk_cmd();
    VkRect2D rect = scissor_rect();
    if (!rect.extent.width || !rect.extent.height) return;
    if (index >= 0 && (index >= (int)pass_ncolor || pass_color[index] == VK_FORMAT_UNDEFINED)) return;
    bool has_stencil = pass_depth == VK_FORMAT_D24_UNORM_S8_UINT || pass_depth == VK_FORMAT_D32_SFLOAT_S8_UINT ||
                       pass_depth == VK_FORMAT_S8_UINT;
    bool has_depth = pass_depth != VK_FORMAT_UNDEFINED && pass_depth != VK_FORMAT_S8_UINT;
    depth = depth && has_depth;
    stencil = stencil && has_stencil;
    if (index < 0 && !depth && !stencil) return;
    if (g_clear_draw < 0) { const char *e = getenv("MHGU_CLEAR_DRAW"); g_clear_draw = e && atoi(e); }
    bool simple = (index < 0 || ((mask & 0xF) == 0xF && !g_clear_draw)) && (!stencil || (smask & 0xFF) == 0xFF);
    if (simple) {
        VkClearAttachment a[2];
        uint32_t n = 0;
        if (index >= 0) {
            a[n] = (VkClearAttachment){.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = (uint32_t)index};
            memcpy(a[n].clearValue.color.float32, color, 16);
            n++;
        }
        VkImageAspectFlags dsa = (depth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        if (dsa) a[n++] = (VkClearAttachment){.aspectMask = dsa, .clearValue.depthStencil = {dval, sval}};
        VkClearRect cr = {rect, 0, 1};
        if (n) vkCmdClearAttachments(cb, n, a, 1, &cr);
        return;
    }
    VkPipeline p = clear_pipeline(index, kind, mask, depth, stencil, smask & 0xFF);
    if (!p) return;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    ss_forget();  // (its own pipeline and state: the next draw sets everything again)
    if (stencil) vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, sval);
    int w, h;
    pass_size(&w, &h);
    VkViewport vp = {0, 0, (float)(w ? w : (int)pass_w), (float)(h ? h : (int)pass_h), 0, 1};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &rect);
    float pc[8] = {0};
    if (color) memcpy(pc, color, 16);
    pc[4] = dval;
    vkCmdPushConstants(cb, pc_layout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
}

// ================================================================== programs
#define MAX_BIND 96
typedef struct {
    ShaderInfo *vi, *fi;
    uint64_t code[2], cmax[2];
    ShaderKey vkey, fkey;
    VkShaderModule vs, fs;
    VkBindingInfo binds[MAX_BIND];
    int nbinds;
    VkDescriptorSetLayout set_layout[2];
    VkPipelineLayout layout;
    _Atomic int state;  // 0 building, 1 ready, 2 failed, 3 dormant (in programs.bin, not built: prog_want)
    uint64_t t_req;     // (stats) when first needed
    uint64_t chash[2], ck;  // program cache: hashes of the vertex / fragment code, the lookup key
    long src_off;           // program cache: where its sources are in programs.src (when src_saved)
    uint32_t vlen, flen;
    bool src_saved;
    bool wanted;            // (under ready_lock) a draw needs it: a prewarm build goes on to make its modules
    struct Pipe *prewarm;   // pipelines of earlier runs to build once this is (pipeline cache)
    FsrCapProg *fsrcap;     // FSR object motion: the capture variant of its vertex shader (fsr.c)
    bool capprog_asked;     // (draw thread) capprog_later tried to make fsrcap
} Prog;
static void prewarm_pipelines(Prog *p);

static VkPipelineLayout make_layout(Prog *p, int nsets, VkShaderStageFlags s0, VkShaderStageFlags s1) {
    p->set_layout[0] = vk_set_layout(p->binds, p->nbinds, 0, s0);
    if (nsets > 1) p->set_layout[1] = vk_set_layout(p->binds, p->nbinds, 1, s1);
    VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = (uint32_t)nsets,
                                     .pSetLayouts = p->set_layout};
    VkPipelineLayout l;
    VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &l));
    return l;
}

static void save_glsl(const char *src, uint64_t addr, const char *suffix) {
    if (!getenv("MHGU_SAVE_GLSL")) return;
    mkdir("build", 0755);
    mkdir("build/glsl", 0755);
    char path[256];
    snprintf(path, sizeof path, "build/glsl/%llx_%s.glsl", (unsigned long long)addr, suffix);
    FILE *f = fopen(path, "w");
    if (f) { fputs(src, f); fclose(f); }
}

static void compile_program(void *arg) {
    Prog *p = arg;
    uint64_t t0 = plat_now_ns();
    size_t nv = 0, nf = 0;
    uint32_t *sv = vk_glsl_to_spirv(p->vi->msl, STAGE_VERTEX, "vs", &nv);
    const char *fsrc = p->fi->msl;
    char *over = NULL;
    if (getenv("MHGU_GLSL_OVERRIDE")) {  // (debug) glsl_override/<code address>_fs.glsl replaces the translated shader
        char path[96];
        snprintf(path, sizeof path, "glsl_override/%llx_fs.glsl", (unsigned long long)p->code[1]);
        FILE *f = fopen(path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            over = calloc(1, (size_t)n + 1);
            if (fread(over, 1, (size_t)n, f) == (size_t)n) fsrc = over;
            fclose(f);
            LOG("[shader] using %s\n", path);
        }
    }
    uint32_t *sf = sv ? vk_glsl_to_spirv(fsrc, STAGE_FRAGMENT, "fs", &nf) : NULL;
    free(over);
    if (!sv || !sf) { free(sv); free(sf); set_state(&p->state, 2, p->t_req); prewarm_pipelines(p); return; }
    p->nbinds = vk_reflect(sv, nv, p->binds, MAX_BIND);
    p->nbinds += vk_reflect(sf, nf, p->binds + p->nbinds, MAX_BIND - p->nbinds);
    p->vs = vk_module(sv, nv);
    p->fs = vk_module(sf, nf);
    free(sv);
    free(sf);
    p->layout = make_layout(p, 2, VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT);
    if (g_fsr_objvel) p->fsrcap = vk_fsr_capprog_new(p->vi->msl);  // (FSR: its vertex shader's source, for the capture)
    free(p->vi->msl);  // (not needed any more: the source of every program ever seen would add up)
    free(p->fi->msl);
    p->vi->msl = p->fi->msl = NULL;
    atomic_fetch_add(&stat_compiles, 1);
    job_stat(1, t0);
    set_state(&p->state, p->vs && p->fs ? 1 : 2, p->t_req);
    prewarm_pipelines(p);
}

static void progcache_store(Prog *p);

// Translates p and records it in programs.bin; false: failed (state 2). quiet: the prewarm's (not logged: a program
// no draw uses may hit translator gaps no one sees)
static _Atomic uint64_t stat_pw_failed;
static bool translate_core(Prog *p, bool quiet) {
    uint64_t t0 = plat_now_ns();
    maxwell_gmem_base = HEAP_BASE;
    maxwell_gmem_size = HEAP_MAX;
    pthread_mutex_lock(&translate_lock);
    p->vi = maxwell_translate((const uint8_t *)G2H((uint32_t)p->code[0]), p->cmax[0], STAGE_VERTEX, &p->vkey);
    if (p->vi->msl) {
        p->fkey.vs_out_mask = p->vi->out_mask;
        p->fi = maxwell_translate((const uint8_t *)G2H((uint32_t)p->code[1]), p->cmax[1], STAGE_FRAGMENT, &p->fkey);
    }
    pthread_mutex_unlock(&translate_lock);
    job_stat(0, t0);
    if (!p->vi->msl || !p->fi->msl) {
        if (quiet) stat_pw_failed++;
        else if (!p->vi->msl) LOG("[shader] VS translate failed: %s (code %llx)\n", p->vi->error, (unsigned long long)p->code[0]);
        else LOG("[shader] FS translate failed: %s (code %llx)\n", p->fi->error, (unsigned long long)p->code[1]);
        set_state(&p->state, 2, p->t_req);
        prewarm_pipelines(p);  // (a pipeline list's, waiting for it: failed too)
        return false;
    }
    if (!quiet && (p->vi->uses_gmem || p->fi->uses_gmem)) {
        static int n;
        if (n++ < 10) LOG("[shader] program %llx/%llx reads global memory (not available: zeros)\n",
                          (unsigned long long)p->code[0], (unsigned long long)p->code[1]);
    }
    save_glsl(p->vi->msl, p->code[0], "vs");
    char fsuf[16] = "fs";
    if (p->fkey.alpha_func)  // (alpha-test variants)
        snprintf(fsuf, sizeof fsuf, "fs_at%d%s", p->fkey.alpha_func, p->fkey.alpha_coverage ? "c" : "");
    save_glsl(p->fi->msl, p->code[1], fsuf);
    progcache_store(p);
    return true;
}

static void translate_program(void *arg) {
    Prog *p = arg;
    if (translate_core(p, false)) run_job(compile_program, p);
}

// ---- program cache: cache/programs.bin keeps every translated program (reflection, keyed by a hash of the guest code
// the translator reads and the shader keys; the sources in programs.src), and the next run builds the ones draws used
// on the workers at its first draw, as Ryujinx's shader cache does: an area's programs are then ready when it is
// entered (translation is serial and was most of the wait); the prewarm's (below) wait dormant until a draw needs one.
// The files are tied to the translator (the Makefile's SHADER_ID, a hash of its sources, and CACHE_FORMAT below), so
// they outlive updates that leave the translator alone, as Ryujinx's cache does; they are appended to as programs are
// translated (a killed run keeps them). MHGU_NO_PROGRAM_CACHE=1: off
// (also off with the translator debug options MHGU_SAVE_GLSL / MHGU_SAVE_MSL / MHGU_NO_DISCARD).
#define PROG_TAB 16384
// The records are an index read whole at start-up; the sources (~27 KB a program, read back only when a dormant program
// is built) are kept apart in programs.src.
typedef struct {
    uint32_t magic, size;  // size: sizeof (ProgRec)
    uint64_t ck, chash[2], code[2], cmax[2];
    ShaderKey vkey, fkey;
    ShaderInfo vi, fi;  // (msl: NULL)
    uint64_t src_off;   // its sources in programs.src: vlen + flen bytes there
    uint32_t vlen, flen;
} ProgRec;
#define PROGREC_MAGIC 0x31475250u  // "PRG1"
#define PROGCACHE_PATH "cache/programs.bin"
#define PROGSRC_PATH "cache/programs.src"
static FILE *progcache_f, *progsrc_f;
static bool progcache_on;
typedef struct { char magic[8]; uint64_t stamp; } CacheHeader;
#define CACHE_FORMAT 1  // bump when what a programs.bin / pipelines.bin record means changes here (ProgRec, PipeKey)
static void pipecache_init(const CacheHeader *hdr);
static void plist_load(void);
static void plist_export_finish(void);
static uint64_t stat_prog_cached;
static struct { uint64_t ck; Prog *prog; } progcache_tab[PROG_TAB];

// The bytes maxwell_translate reads: NVN header + SPH, and the code up to its end (found the same way).
static uint64_t code_hash(uint64_t addr, uint64_t cmax, int stage) {
    const uint8_t *blob = (const uint8_t *)G2H((uint32_t)addr);
    size_t hdr = stage == STAGE_COMPUTE ? 0 : 0x80;
    if (cmax <= hdr) return 0;
    size_t n = (cmax - hdr) / 8, i;
    if (n > 65536) n = 65536;
    bool end = false;
    for (i = 0; i < n; i++) {
        uint64_t w;
        memcpy(&w, blob + hdr + i * 8, 8);
        if (i % 4 == 0) continue;
        int32_t off = (int32_t)((uint32_t)(w >> 20) << 8) >> 8;
        if ((w & 0xfff0000000000000ull) == 0xe240000000000000ull && off == -8 && end) { i++; break; }  // bra self
        if (w == 0 && i > 4) { i++; break; }
        end = true;
    }
    return hash_words(0x84222325cbf29ce4ull ^ i, blob, hdr + i * 8);
}

static uint64_t prog_content_key(const uint64_t chash[2], const ShaderKey *vkey, const ShaderKey *fkey) {
    uint64_t h = hash_words(0xcbf29ce484222325ull, chash, 16);
    h = hash_words(h, vkey, sizeof *vkey);
    h = hash_words(h, fkey->rt_kind, sizeof fkey->rt_kind);
    h = hash_words(h, &fkey->alpha_func, 1);
    if (fkey->alpha_coverage) h = hash_words(h, &fkey->alpha_coverage, 1);  // (MSAA variant)
    return h ? h : 1;
}

static Prog **progcache_slot(uint64_t ck, bool insert) {
    uint32_t slot = (uint32_t)(ck ^ (ck >> 31)) & (PROG_TAB - 1);
    for (int n = 0; n < PROG_TAB; n++, slot = (slot + 1) & (PROG_TAB - 1)) {
        if (progcache_tab[slot].ck == ck) return &progcache_tab[slot].prog;
        if (!progcache_tab[slot].ck) {
            if (!insert) return NULL;
            progcache_tab[slot].ck = ck;
            return &progcache_tab[slot].prog;
        }
    }
    return NULL;
}

// progcache_tab is shared by the render thread (draws) and the translate thread (the prewarm): under progcache_lock
// once progcache_init has run (progcache_ready; the prewarm waits for it).
static pthread_mutex_t progcache_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t progcache_cond = PTHREAD_COND_INITIALIZER;
static bool progcache_ready;
static FILE *progcache_rf;  // reads dormant programs' sources back from programs.src (load_program; under progcache_rlock)
static pthread_mutex_t progcache_rlock = PTHREAD_MUTEX_INITIALIZER;

// A dormant program's sources, read back from programs.bin, then built (its SPIR-V is in cache/spirv: file reads).
static void load_program(void *arg) {
    Prog *p = arg;
    char *vs = malloc(p->vlen + 1), *fs = malloc(p->flen + 1);
    pthread_mutex_lock(&progcache_rlock);
    bool ok = progcache_rf && !fseek(progcache_rf, p->src_off, SEEK_SET) && fread(vs, 1, p->vlen, progcache_rf) == p->vlen &&
              fread(fs, 1, p->flen, progcache_rf) == p->flen;
    pthread_mutex_unlock(&progcache_rlock);
    if (!ok) { free(vs); free(fs); set_state(&p->state, 2, p->t_req); prewarm_pipelines(p); return; }
    vs[p->vlen] = fs[p->flen] = 0;
    p->vi->msl = vs;
    p->fi->msl = fs;
    compile_program(p);
}

// Object motion turned on after p was built (SMAA T2x picked in the menu): its capture variant from the vertex source
// the program cache keeps (NULL: none; asked once).
static FsrCapProg *capprog_later(Prog *p) {
    if (p->capprog_asked || !p->src_saved) return NULL;
    p->capprog_asked = true;
    char *vs = malloc(p->vlen + 1);
    if (!vs) return NULL;
    pthread_mutex_lock(&progcache_rlock);
    bool ok = progcache_rf && !fseek(progcache_rf, p->src_off, SEEK_SET) && fread(vs, 1, p->vlen, progcache_rf) == p->vlen;
    pthread_mutex_unlock(&progcache_rlock);
    if (ok) vs[p->vlen] = 0, p->fsrcap = vk_fsr_capprog_new(vs);
    free(vs);
    return p->fsrcap;
}

static void progcache_open(void) {
    if (getenv("MHGU_NO_PROGRAM_CACHE") || getenv("MHGU_SAVE_GLSL") || getenv("MHGU_SAVE_MSL") || getenv("MHGU_NO_DISCARD"))
        return;
    CacheHeader hdr = {"MHGUPRG", sizeof(ProgRec)}, fh;
    hdr.stamp = hash_words(hdr.stamp, (uint64_t[]){CACHE_FORMAT}, 8);
#ifdef SHADER_ID
    hdr.stamp = hash_words(hdr.stamp, SHADER_ID, sizeof SHADER_ID - 1);
#else  // (built without the Makefile's id: tied to this executable, size + time stamp)
    char exe[1024];
    struct stat es;
    if (!plat_exe_path(exe, sizeof exe) || stat(exe, &es)) return;
    hdr.stamp = hash_words(hdr.stamp, (uint64_t[]){(uint64_t)es.st_size, (uint64_t)es.st_mtime}, 16);
#endif
    mkdir("cache", 0755);
    uint64_t t0 = plat_now_ns();
    int n = 0, used = 0;
    long good = 0;
    FILE *f = fopen(PROGCACHE_PATH, "r+b"), *sf = fopen(PROGSRC_PATH, "r+b");
    if (f && sf && fread(&fh, sizeof fh, 1, f) == 1 && !memcmp(&fh, &hdr, sizeof hdr)) {
        good = (long)sizeof hdr;
        fseek(sf, 0, SEEK_END);
        uint64_t send = (uint64_t)ftell(sf);
        ProgRec r;
        while (fread(&r, sizeof r, 1, f) == 1) {
            if (r.magic != PROGREC_MAGIC || r.size != sizeof r || r.vlen > (16u << 20) || r.flen > (16u << 20)) break;
            if (r.src_off + r.vlen + r.flen > send) break;  // (its sources cut short by a killed run)
            good = ftell(f);
            long src = (long)r.src_off;
            Prog **slot = progcache_slot(r.ck, true);
            if (!slot || *slot) continue;  // (a duplicate: translated twice in a run)
            Prog *p = calloc(1, sizeof *p);
            p->vi = malloc(sizeof *p->vi);
            p->fi = malloc(sizeof *p->fi);
            *p->vi = r.vi;
            *p->fi = r.fi;
            p->vi->msl = p->fi->msl = NULL;
            p->src_off = src;
            p->src_saved = true;
            p->vlen = r.vlen;
            p->flen = r.flen;
            memcpy(p->code, r.code, sizeof p->code);
            memcpy(p->cmax, r.cmax, sizeof p->cmax);
            memcpy(p->chash, r.chash, sizeof p->chash);
            p->ck = r.ck;
            p->vkey = r.vkey;
            p->fkey = r.fkey;
            atomic_store(&p->state, 3);
            *slot = p;
            n++;
        }
        fseek(f, good, SEEK_SET);
        if (ftruncate(fileno(f), good)) {}  // (a record cut short by a killed run)
    } else {
        if (f) fclose(f);
        if (sf) fclose(sf);
        f = fopen(PROGCACHE_PATH, "wb");
        sf = fopen(PROGSRC_PATH, "wb");
        if (f) fwrite(&hdr, sizeof hdr, 1, f);
    }
    if (!f || !sf) { if (f) fclose(f); if (sf) fclose(sf); return; }
    fflush(f);
    fseek(sf, 0, SEEK_END);
    progcache_f = f;
    progsrc_f = sf;
    progcache_rf = fopen(PROGSRC_PATH, "rb");
    progcache_on = true;
    pipecache_init(&hdr);  // (attaches the recorded pipelines to their programs before these are built)
    plist_load();          // (and the shipped list's whose programs are in programs.bin)
    plist_export_finish();
    // Programs a draw has used (each has recorded pipelines) are built now; the others (the prewarm's) stay dormant
    // until a draw wants one (prog_want)
    for (int i = 0; i < PROG_TAB; i++) {
        Prog *p = progcache_tab[i].prog;
        if (!p || !p->prewarm) continue;
        atomic_store(&p->state, 0);
        run_job(load_program, p);
        used++;
    }
    LOG("[shader] program cache: %d programs (%d used before: building on the workers), read in %.0f ms\n", n, used,
        (plat_now_ns() - t0) / 1e6);
}

static void progcache_init(void) {
    static bool done;
    if (done) return;
    done = true;
    progcache_open();
    pthread_mutex_lock(&progcache_lock);
    progcache_ready = true;
    pthread_cond_broadcast(&progcache_cond);
    pthread_mutex_unlock(&progcache_lock);
}

// (translate thread only: the one writer)
static void progcache_store(Prog *p) {
    if (!progcache_f) return;
    ProgRec r;
    memset(&r, 0, sizeof r);
    r.magic = PROGREC_MAGIC;
    r.ck = p->ck;
    memcpy(r.chash, p->chash, sizeof r.chash);
    memcpy(r.code, p->code, sizeof r.code);
    memcpy(r.cmax, p->cmax, sizeof r.cmax);
    r.vkey = p->vkey;
    r.fkey = p->fkey;
    r.vi = *p->vi;
    r.fi = *p->fi;
    r.vi.msl = r.fi.msl = NULL;
    r.vlen = (uint32_t)strlen(p->vi->msl);
    r.flen = (uint32_t)strlen(p->fi->msl);
    r.size = (uint32_t)sizeof r;
    long at = ftell(progsrc_f);
    if (at < 0 || fwrite(p->vi->msl, 1, r.vlen, progsrc_f) != r.vlen || fwrite(p->fi->msl, 1, r.flen, progsrc_f) != r.flen) return;
    fflush(progsrc_f);  // (the sources before the record that points at them)
    r.src_off = (uint64_t)at;
    if (fwrite(&r, sizeof r, 1, progcache_f) == 1) {
        p->src_off = at;
        p->src_saved = true;
        p->vlen = r.vlen;
        p->flen = r.flen;
    }
    fflush(progcache_f);
}

// A draw needs p (found in the program cache): a dormant one is loaded on the workers; one the prewarm is still
// building goes on to make its modules.
static void prog_want(Prog *p) {
    bool load = false;
    pthread_mutex_lock(&ready_lock);
    p->wanted = true;
    if (atomic_load(&p->state) == 3) { atomic_store(&p->state, 0); load = true; }
    pthread_mutex_unlock(&ready_lock);
    if (!atomic_load(&p->state)) p->t_req = plat_now_ns();
    if (load) run_job(load_program, p);
}

// ---- prewarm: the game makes all its programs at start-up (nvnProgramSetShaders, ~6300 vertex / fragment pairs
// before its first frame). Each one not in programs.bin yet is translated with the keys most draws use (float vertex
// inputs and colour targets, no alpha test) on the translate thread and its SPIR-V compiled into cache/spirv on the
// workers, behind every build a draw asked for (the queues' low lists), then left dormant: when a draw first needs it
// its modules come from the SPIR-V cache in about a millisecond, within the per-frame wait. So a program is ready
// the first time it is drawn even on a first run once the prewarm has got to it (it starts at the first frame; a
// draw needing one it has not reached yet is skipped, as before, never waited for past the budget). Variants with
// other keys (integer vertex inputs: skinned models; alpha-tested materials) are still made on first use.
// MHGU_NO_PREWARM=1: off.
typedef struct { uint64_t code[2], cmax[2]; } PrewarmReq;
static void plist_seen(const uint64_t code[2], const uint64_t cmax[2], const uint64_t chash[2]);  // (pipelines.list)
static uint64_t pw_t0;
static _Atomic uint64_t pw_total_new, pw_total_known;

static void prewarm_done(void) {
    if (atomic_fetch_sub(&stat_pw_left, 1) != 1) return;
    LOG("[shader] prewarm: %llu programs translated, %llu already cached, %llu failed, %.1f s after the first one\n",
        (unsigned long long)atomic_load(&pw_total_new), (unsigned long long)atomic_load(&pw_total_known),
        (unsigned long long)atomic_load(&stat_pw_failed), (plat_now_ns() - pw_t0) / 1e9);
}

// (workers, low list) the SPIR-V into cache/spirv; then the sources go and the program is dormant, unless a draw
// already wants it.
static void prewarm_compile(void *arg) {
    Prog *p = arg;
    size_t n;
    free(vk_glsl_to_spirv(p->vi->msl, STAGE_VERTEX, "vs", &n));
    free(vk_glsl_to_spirv(p->fi->msl, STAGE_FRAGMENT, "fs", &n));
    pthread_mutex_lock(&ready_lock);
    bool wanted = p->wanted || !p->src_saved;
    if (!wanted) {
        free(p->vi->msl);
        free(p->fi->msl);
        p->vi->msl = p->fi->msl = NULL;
        atomic_store(&p->state, 3);
    }
    pthread_mutex_unlock(&ready_lock);
    if (wanted) compile_program(p);
    prewarm_done();
}

// (translate thread, low list)
static void prewarm_translate(void *arg) {
    PrewarmReq *r = arg;
    pthread_mutex_lock(&progcache_lock);
    while (!progcache_ready) pthread_cond_wait(&progcache_cond, &progcache_lock);
    pthread_mutex_unlock(&progcache_lock);
    Prog *p = NULL;
    if (progcache_on) {
        ShaderKey vkey = {0}, fkey = {0};  // (as draw() makes them)
        vkey.glsl = fkey.glsl = 1;
        vkey.flip_y = fkey.flip_y = nvn_window_origin() == 0;
        vkey.depth_minus_one = nvn_depth_mode() == 0;
        uint64_t chash[2];
        for (int i = 0; i < 2; i++) chash[i] = code_hash(r->code[i], r->cmax[i], i);
        uint64_t ck = prog_content_key(chash, &vkey, &fkey);
        plist_seen(r->code, r->cmax, chash);  // (the pipeline list's variants of this code: made on the render thread)
        pthread_mutex_lock(&progcache_lock);
        Prog **c = progcache_slot(ck, true);
        if (c && !*c) {
            p = calloc(1, sizeof *p);
            memcpy(p->code, r->code, sizeof p->code);
            memcpy(p->cmax, r->cmax, sizeof p->cmax);
            memcpy(p->chash, chash, sizeof chash);
            p->ck = ck;
            p->vkey = vkey;
            p->fkey = fkey;
            *c = p;
        }
        pthread_mutex_unlock(&progcache_lock);
    }
    free(r);
    if (!p) { atomic_fetch_add(&stat_pw_known, 1); atomic_fetch_add(&pw_total_known, 1); prewarm_done(); return; }
    atomic_fetch_add(&stat_pw_new, 1);
    atomic_fetch_add(&pw_total_new, 1);
    if (translate_core(p, true)) enqueue(&compile_q, 3, true, prewarm_compile, p);
    else prewarm_done();
}

// (game thread) nvnProgramSetShaders
void backend_program_created(const NvnProgram *np) {
    static int on = -1;
    if (on < 0) on = g_config.shader_prewarm && !getenv("MHGU_NO_PREWARM") && !sync_shaders();
    if (!on || !(np->stage_mask & 1) || !(np->stage_mask & 2)) return;
    PrewarmReq *r = malloc(sizeof *r);
    for (int i = 0; i < 2; i++) {
        r->code[i] = np->stages[i].code_addr;
        r->cmax[i] = np->stages[i].const_offset ? np->stages[i].const_offset : 0x10000;
    }
    if (atomic_fetch_add(&stat_pw_left, 1) == 0 && !pw_t0) pw_t0 = plat_now_ns();
    enqueue(&translate_q, 1, true, prewarm_translate, r);
}

static struct { uint64_t key; Prog *prog; } prog_tab[PROG_TAB];

static Prog *get_program(NvnProgram *np, ShaderKey *vkey, ShaderKey *fkey) {
    if (!(np->stage_mask & 1) || !(np->stage_mask & 2)) return NULL;
    uint64_t h = 0xcbf29ce484222325ull;
    for (int i = 0; i < 2; i++) h = hash_words(h, &np->stages[i].code_addr, 8);
    h = hash_words(h, vkey, sizeof *vkey);
    h = hash_words(h, fkey->rt_kind, sizeof fkey->rt_kind);
    h = hash_words(h, &fkey->alpha_func, 1);
    if (fkey->alpha_coverage) h = hash_words(h, &fkey->alpha_coverage, 1);  // (MSAA variant)
    if (!h) h = 1;
    uint32_t slot = (uint32_t)(h ^ (h >> 31)) & (PROG_TAB - 1);
    while (prog_tab[slot].key && prog_tab[slot].key != h) slot = (slot + 1) & (PROG_TAB - 1);
    if (prog_tab[slot].key) return prog_tab[slot].prog;
    progcache_init();
    uint64_t code[2], cmax[2], chash[2] = {0, 0}, ck = 0;
    for (int i = 0; i < 2; i++) {
        code[i] = np->stages[i].code_addr;
        cmax[i] = np->stages[i].const_offset ? np->stages[i].const_offset : 0x10000;
        if (progcache_on) chash[i] = code_hash(code[i], cmax[i], i);
    }
    Prog *p = NULL;
    if (progcache_on) {
        ck = prog_content_key(chash, vkey, fkey);
        pthread_mutex_lock(&progcache_lock);
        Prog **c = progcache_slot(ck, true);
        Prog *q = c ? *c : NULL;
        if (c && !q) *c = p = calloc(1, sizeof *p);  // (an identical program at another address is found by its contents)
        pthread_mutex_unlock(&progcache_lock);
        if (q) {  // (translated in an earlier run or by the prewarm: dormant, building, or built)
            prog_tab[slot].key = h;
            prog_tab[slot].prog = q;
            stat_prog_cached++;
            prog_want(q);
            return q;
        }
    }
    if (!p) p = calloc(1, sizeof *p);
    prog_tab[slot].key = h;
    prog_tab[slot].prog = p;
    memcpy(p->code, code, sizeof code);
    memcpy(p->cmax, cmax, sizeof cmax);
    memcpy(p->chash, chash, sizeof chash);
    p->ck = ck;
    p->vkey = *vkey;
    p->fkey = *fkey;
    p->wanted = true;
    p->t_req = plat_now_ns();
    run_translate(translate_program, p);
    return p;
}

// ================================================================== graphics pipelines
// Everything a pipeline bakes in (on the 1.2 baseline: all fixed-function state but viewport, scissor, depth bias,
// blend constants and stencil masks / reference), captured by value so it can be built on a worker thread.
#define ZERO_BINDING 16
typedef struct {
    Prog *prog;
    VkRenderPass rp;
    uint32_t ncolor, samples;
    uint32_t attr_fmt[32], attr_off[32], attr_bind[32];
    uint32_t attr_mask;
    uint32_t stride[17], divisor[17], used_streams;
    uint8_t blend[8][8];
    uint32_t write_mask[8];
    uint8_t blend_on, a2c, topology, poly_fill;
    uint8_t cull, front_ccw, prim_restart, depth_bias, depth_clamp;
    uint8_t depth_test, depth_write, depth_op, stencil_test;
    uint8_t sten_ops[2][4];  // per face: fail, pass, depth fail, compare
} PipeKey;

typedef struct Pipe {
    PipeKey k;
    VkPipeline pipe;
    _Atomic int state;  // 0 building, 1 ready, 2 failed
    uint64_t t_req;
    struct Pipe *next;  // (Prog.prewarm)
} Pipe;
#define PIPE_TAB 8192
static struct { uint64_t key; Pipe *pipe; } pipe_tab[PIPE_TAB];

static Pipe *pipe_slot(const PipeKey *k, bool *created) {
    uint64_t ph = hash_words(0xcbf29ce484222325ull, k, sizeof *k);
    if (!ph) ph = 1;
    uint32_t pslot = (uint32_t)(ph ^ (ph >> 29)) & (PIPE_TAB - 1);
    for (int n = 0; pipe_tab[pslot].key && pipe_tab[pslot].key != ph; n++, pslot = (pslot + 1) & (PIPE_TAB - 1))
        if (n == PIPE_TAB) return NULL;
    *created = !pipe_tab[pslot].pipe;
    if (*created) {
        Pipe *pp = calloc(1, sizeof *pp);
        pp->k = *k;
        pipe_tab[pslot].key = ph;
        pipe_tab[pslot].pipe = pp;
    }
    return pipe_tab[pslot].pipe;
}

// ---- pipeline cache (with the program cache): cache/pipelines.bin records each pipeline's state, its program by
// content key and its render pass by formats; the next run builds them as soon as their (cached) program is built.
// An area's first frame can need over a hundred new pipelines at once: more than the wait budget, even from the
// Vulkan pipeline cache.
typedef struct {
    uint32_t magic, pad;
    uint64_t ck;
    VkFormat color[8], depth;
    PipeKey k;  // (prog, rp: NULL)
} PipeRec;
#define PIPEREC_MAGIC 0x31504950u  // "PIP1"
#define PIPECACHE_PATH "cache/pipelines.bin"
static FILE *pipecache_f;

static void build_pipeline(void *arg);
static void prewarm_pipelines(Prog *p) {
    pthread_mutex_lock(&ready_lock);  // (the render thread adds to the list too: plist_attach)
    Pipe *q = p->prewarm;
    p->prewarm = NULL;
    pthread_mutex_unlock(&ready_lock);
    for (Pipe *next; q; q = next) {
        next = q->next;
        if (atomic_load(&p->state) == 1) run_job(build_pipeline, q);
        else set_state(&q->state, 2, 0);
    }
}
static void plist_export_add(const Prog *p, const PipeRec *r);

static void pipecache_init(const CacheHeader *hdr) {
    CacheHeader fh;
    int n = 0;
    long good = 0;
    FILE *f = fopen(PIPECACHE_PATH, "r+b");
    if (f && fread(&fh, sizeof fh, 1, f) == 1 && !memcmp(fh.magic, "MHGUPRG", 8) &&
        fh.stamp == hash_words(hdr->stamp, (uint64_t[]){sizeof(PipeRec)}, 8)) {
        good = (long)sizeof fh;
        PipeRec r;
        while (fread(&r, sizeof r, 1, f) == 1 && r.magic == PIPEREC_MAGIC && r.k.ncolor <= 8) {
            good = ftell(f);
            Prog **c = progcache_slot(r.ck, false);
            if (!c || !*c) continue;
            plist_export_add(*c, &r);
            r.k.prog = *c;
            r.k.rp = vk_render_pass(r.color, r.k.ncolor, r.depth, r.k.samples);
            bool created;
            Pipe *pp = pipe_slot(&r.k, &created);
            if (!pp || !created) continue;
            pp->next = (*c)->prewarm;
            (*c)->prewarm = pp;
            n++;
        }
        fseek(f, good, SEEK_SET);
        if (ftruncate(fileno(f), good)) {}
    } else {
        if (f) fclose(f);
        f = fopen(PIPECACHE_PATH, "wb");
        fh = *hdr;
        fh.stamp = hash_words(hdr->stamp, (uint64_t[]){sizeof(PipeRec)}, 8);
        if (f) fwrite(&fh, sizeof fh, 1, f);
    }
    pipecache_f = f;
    if (n) LOG("[vk] pipeline cache: %d pipelines to build\n", n);
}

static void pipecache_store(const Pipe *pp) {
    if (!pipecache_f || !pp->k.prog->ck) return;
    PipeRec r;
    memset(&r, 0, sizeof r);
    r.magic = PIPEREC_MAGIC;
    r.ck = pp->k.prog->ck;
    memcpy(r.color, pass_color, sizeof r.color);
    r.depth = pass_depth;
    r.k = pp->k;
    r.k.prog = NULL;
    r.k.rp = VK_NULL_HANDLE;
    fwrite(&r, sizeof r, 1, pipecache_f);
    fflush(pipecache_f);
}

// ---- shipped pipeline list: pipelines.list beside the executable, the pipelines recorded while playing the common
// areas, so a first run builds them in the background before an area needs them (cache/pipelines.bin only knows the
// ones this computer drew). Unlike pipelines.bin it outlives translator changes and other GPUs: a record names its
// program by the game's own shader code (code_hash) and shader keys, not by a translated program, and the formats
// the GPU decides are made again here (DEPTH24_STENCIL8's: D32S8 on AMD; vertex formats it lacks: widened). A record
// whose program is in programs.bin is attached at start-up like pipelines.bin's; the others wait until the prewarm
// meets their shader code (plist_seen, translate thread), then the render thread (plist_drain) makes the program
// variant and its pipelines. Recording: MHGU_PIPELIST_EXPORT=<file> writes the list (pipelines.bin's records and the
// shipped list's, without duplicates) at the first draw. MHGU_PIPELIST=<file> reads another list; MHGU_NO_PIPELIST=1:
// none.
typedef struct {
    uint32_t magic, pad;
    uint64_t chash[2];
    ShaderKey vkey, fkey;
    VkFormat color[8], depth;
    PipeKey k;  // (prog, rp: NULL)
} ListRec;
typedef struct { char magic[8]; uint32_t key_size, pipe_size, rec_size, count; } ListHeader;
#define LISTREC_MAGIC 0x314c5050u  // "PPL1"
static ListRec *plist;
static int plist_n, plist_left;
static uint8_t *plist_done;
static struct { uint64_t code[2], cmax[2], chash[2]; } plist_q[256];  // shader code the prewarm met (plist_seen)
static int plist_qn;
static _Atomic int plist_qpending;
static pthread_mutex_t plist_lock = PTHREAD_MUTEX_INITIALIZER;

// (translate thread, prewarm) the game made a program with this code: the render thread looks for list records
static void plist_seen(const uint64_t code[2], const uint64_t cmax[2], const uint64_t chash[2]) {
    if (!plist_left) return;
    bool any = false;
    for (int i = 0; i < plist_n && !any; i++)
        any = !plist_done[i] && plist[i].chash[0] == chash[0] && plist[i].chash[1] == chash[1];
    if (!any) return;
    for (;;) {
        pthread_mutex_lock(&plist_lock);
        if (plist_qn < (int)(sizeof plist_q / sizeof *plist_q)) {
            memcpy(plist_q[plist_qn].code, code, 16);
            memcpy(plist_q[plist_qn].cmax, cmax, 16);
            memcpy(plist_q[plist_qn].chash, chash, 16);
            plist_qn++;
            atomic_store(&plist_qpending, 1);
            pthread_mutex_unlock(&plist_lock);
            return;
        }
        pthread_mutex_unlock(&plist_lock);
        struct timespec ts = {0, 2000000};  // (full: the render thread empties it at its next command list)
        nanosleep(&ts, NULL);
    }
}

// (render thread) r's pipeline for program p, built once p is (as pipelines.bin's)
static int plist_attach(Prog *p, const ListRec *r) {
    PipeKey k = r->k;
    k.prog = p;
    for (int i = 0; i < 32; i++)
        if (k.attr_mask & (1u << i)) k.attr_fmt[i] = vertex_format_supported(k.attr_fmt[i]);
    VkFormat depth = r->depth;
    if (depth == VK_FORMAT_D24_UNORM_S8_UINT || depth == VK_FORMAT_D32_SFLOAT_S8_UINT) depth = vk_depth24_stencil8();
    k.rp = vk_render_pass(r->color, k.ncolor, depth, k.samples);
    bool created;
    Pipe *pp = pipe_slot(&k, &created);
    if (!pp || !created) return 0;
    pthread_mutex_lock(&ready_lock);
    int s = atomic_load(&p->state);
    if (s != 1 && s != 2) pp->next = p->prewarm, p->prewarm = pp;
    pthread_mutex_unlock(&ready_lock);
    if (s == 1) run_job(build_pipeline, pp);
    else if (s == 2) set_state(&pp->state, 2, 0);
    return 1;
}

static void plist_load(void) {
    if (getenv("MHGU_NO_PIPELIST")) return;
    char path[1100];
    const char *e = getenv("MHGU_PIPELIST");
    if (e && *e) {
        snprintf(path, sizeof path, "%s", e);
    } else {
        char exe[1024];
        if (!plat_exe_path(exe, sizeof exe)) return;
        char *s = strrchr(exe, '/'), *bs = strrchr(exe, '\\');
        if (bs > s) s = bs;
        if (s) s[1] = 0; else exe[0] = 0;
        snprintf(path, sizeof path, "%spipelines.list", exe);
    }
    FILE *f = fopen(path, "rb");
    if (!f) return;
    ListHeader h;
    if (fread(&h, sizeof h, 1, f) != 1 || memcmp(h.magic, "MHGUPL1", 8) || h.key_size != sizeof(ShaderKey) ||
        h.pipe_size != sizeof(PipeKey) || h.rec_size != sizeof(ListRec) || h.count > 1000000) {
        LOG("[vk] %s: not a pipeline list of this version, not used\n", path);
        fclose(f);
        return;
    }
    plist = calloc(h.count ? h.count : 1, sizeof *plist);
    plist_done = calloc(h.count ? h.count : 1, 1);
    plist_n = plist ? (int)fread(plist, sizeof *plist, h.count, f) : 0;
    fclose(f);
    int now = 0, built = 0;
    for (int i = 0; i < plist_n; i++) {
        if (plist[i].magic != LISTREC_MAGIC || plist[i].k.ncolor > 8) { plist_done[i] = 1; continue; }
        uint64_t ck = prog_content_key(plist[i].chash, &plist[i].vkey, &plist[i].fkey);
        Prog **c = progcache_slot(ck, false);
        if (!c || !*c) continue;  // (its program waits for the prewarm: plist_seen)
        plist_done[i] = 1;
        now++;
        built += plist_attach(*c, &plist[i]);
    }
    for (int i = 0; i < plist_n; i++) plist_left += !plist_done[i];
    LOG("[vk] pipeline list %s: %d pipelines, %d new to build now, %d when the prewarm meets their programs\n", path,
        plist_n, built, plist_left);
    (void)now;
}

// (render thread) the programs of the records whose shader code the prewarm met, then their pipelines
static void plist_drain(void) {
    if (!atomic_load(&plist_qpending)) return;
    pthread_mutex_lock(&plist_lock);
    int n = plist_qn;
    static struct { uint64_t code[2], cmax[2], chash[2]; } q[256];
    memcpy(q, plist_q, sizeof *q * (size_t)n);
    plist_qn = 0;
    atomic_store(&plist_qpending, 0);
    pthread_mutex_unlock(&plist_lock);
    for (int j = 0; j < n; j++)
        for (int i = 0; i < plist_n; i++) {
            ListRec *r = &plist[i];
            if (plist_done[i] || r->chash[0] != q[j].chash[0] || r->chash[1] != q[j].chash[1]) continue;
            plist_done[i] = 1;
            plist_left--;
            uint64_t ck = prog_content_key(r->chash, &r->vkey, &r->fkey);
            Prog *p = NULL, *have = NULL;
            pthread_mutex_lock(&progcache_lock);
            Prog **c = progcache_slot(ck, true);
            if (c) { have = *c; if (!have) *c = p = calloc(1, sizeof *p); }
            pthread_mutex_unlock(&progcache_lock);
            if (have) {
                prog_want(have);  // (a dormant one is loaded)
                plist_attach(have, r);
                continue;
            }
            if (!p) continue;
            memcpy(p->code, q[j].code, sizeof p->code);
            memcpy(p->cmax, q[j].cmax, sizeof p->cmax);
            memcpy(p->chash, r->chash, sizeof p->chash);
            p->ck = ck;
            p->vkey = r->vkey;
            p->fkey = r->fkey;
            p->wanted = true;
            plist_attach(p, r);  // (before it is translated: its pipelines follow it)
            run_translate(translate_program, p);
        }
    if (!plist_left) LOG("[vk] pipeline list: every program met\n");
}

// MHGU_PIPELIST_EXPORT: pipelines.bin's records (as pipecache_init reads them) and the shipped list's
static FILE *plist_ex;
static uint64_t *plist_ex_seen;
static int plist_ex_n, plist_ex_cap;
static void plist_export_rec(const ListRec *r) {
    uint64_t h = hash_words(0xcbf29ce484222325ull, r, sizeof *r);
    for (int i = 0; i < plist_ex_n; i++)
        if (plist_ex_seen[i] == h) return;
    if (plist_ex_n == plist_ex_cap) plist_ex_seen = realloc(plist_ex_seen, sizeof *plist_ex_seen * (size_t)(plist_ex_cap = plist_ex_cap ? plist_ex_cap * 2 : 4096));
    plist_ex_seen[plist_ex_n++] = h;
    fwrite(r, sizeof *r, 1, plist_ex);
}
static void plist_export_add(const Prog *p, const PipeRec *pr) {
    static int tried;
    if (!tried) {
        tried = 1;
        const char *e = getenv("MHGU_PIPELIST_EXPORT");
        if (e && *e && (plist_ex = fopen(e, "wb"))) {
            ListHeader h = {"MHGUPL1", sizeof(ShaderKey), sizeof(PipeKey), sizeof(ListRec), 0};
            fwrite(&h, sizeof h, 1, plist_ex);
        }
    }
    if (!plist_ex || !p) return;
    ListRec r;
    memset(&r, 0, sizeof r);
    r.magic = LISTREC_MAGIC;
    memcpy(r.chash, p->chash, sizeof r.chash);
    r.vkey = p->vkey;
    r.fkey = p->fkey;
    memcpy(r.color, pr->color, sizeof r.color);
    r.depth = pr->depth;
    r.k = pr->k;
    r.k.prog = NULL;
    r.k.rp = VK_NULL_HANDLE;
    plist_export_rec(&r);
}
static void plist_export_finish(void) {
    plist_export_add(NULL, NULL);  // (opens the file when pipelines.bin had nothing)
    if (!plist_ex) return;
    for (int i = 0; i < plist_n; i++)
        if (plist[i].magic == LISTREC_MAGIC) plist_export_rec(&plist[i]);
    ListHeader h = {"MHGUPL1", sizeof(ShaderKey), sizeof(PipeKey), sizeof(ListRec), (uint32_t)plist_ex_n};
    fseek(plist_ex, 0, SEEK_SET);
    fwrite(&h, sizeof h, 1, plist_ex);
    fclose(plist_ex);
    plist_ex = NULL;
    LOG("[vk] pipeline list exported: %s (%d pipelines)\n", getenv("MHGU_PIPELIST_EXPORT"), plist_ex_n);
}

static void build_pipeline(void *arg) {
    Pipe *pp = arg;
    uint64_t t0 = plat_now_ns();
    PipeKey *k = &pp->k;
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = k->prog->vs, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = k->prog->fs, .pName = "main"}};
    VkVertexInputAttributeDescription va[32];
    VkVertexInputBindingDescription vb[17];
    VkVertexInputBindingDivisorDescriptionEXT vd[17];
    uint32_t na = 0, nb = 0, nd = 0;
    for (int i = 0; i < 32; i++)
        if (k->attr_mask & (1u << i)) va[na++] = (VkVertexInputAttributeDescription){(uint32_t)i, k->attr_bind[i], k->attr_fmt[i], k->attr_off[i]};
    for (uint32_t s = 0; s < 17; s++) {
        if (!(k->used_streams & (1u << s))) continue;
        bool inst = k->divisor[s] != 0;
        vb[nb++] = (VkVertexInputBindingDescription){s, k->stride[s], inst ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
        if (inst && k->divisor[s] > 1 && g_vk.divisor) vd[nd++] = (VkVertexInputBindingDivisorDescriptionEXT){s, k->divisor[s]};
    }
    VkPipelineVertexInputDivisorStateCreateInfoEXT dvi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT,
                                                          .vertexBindingDivisorCount = nd, .pVertexBindingDivisors = vd};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .pNext = nd ? &dvi : NULL,
                                               .vertexBindingDescriptionCount = nb, .pVertexBindingDescriptions = vb,
                                               .vertexAttributeDescriptionCount = na, .pVertexAttributeDescriptions = va};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = (VkPrimitiveTopology)k->topology, .primitiveRestartEnable = k->prim_restart};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = k->poly_fill ? VK_POLYGON_MODE_FILL : VK_POLYGON_MODE_LINE,
                                                 .cullMode = k->cull == 1 ? VK_CULL_MODE_FRONT_BIT : k->cull == 2 ? VK_CULL_MODE_BACK_BIT
                                                           : k->cull == 3 ? VK_CULL_MODE_FRONT_AND_BACK : VK_CULL_MODE_NONE,
                                                 .frontFace = k->front_ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE,
                                                 .depthClampEnable = k->depth_clamp, .depthBiasEnable = k->depth_bias, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = (VkSampleCountFlagBits)k->samples, .alphaToCoverageEnable = k->a2c};
    VkStencilOpState so[2];
    for (int f = 0; f < 2; f++)
        so[f] = (VkStencilOpState){(VkStencilOp)k->sten_ops[f][0], (VkStencilOp)k->sten_ops[f][1], (VkStencilOp)k->sten_ops[f][2],
                                   (VkCompareOp)k->sten_ops[f][3], 0xFF, 0xFF, 0};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                .depthTestEnable = k->depth_test, .depthWriteEnable = k->depth_write,
                                                .depthCompareOp = (VkCompareOp)k->depth_op, .stencilTestEnable = k->stencil_test,
                                                .front = so[0], .back = so[1]};
    VkPipelineColorBlendAttachmentState cba[8] = {0};
    for (uint32_t i = 0; i < k->ncolor; i++) {
        cba[i].colorWriteMask = k->write_mask[i];
        if ((k->blend_on >> i) & 1) {
            const uint8_t *b = k->blend[i];
            cba[i].blendEnable = VK_TRUE;
            cba[i].srcColorBlendFactor = blend_factor(b[1]);
            cba[i].dstColorBlendFactor = blend_factor(b[2]);
            cba[i].srcAlphaBlendFactor = blend_factor(b[3]);
            cba[i].dstAlphaBlendFactor = blend_factor(b[4]);
            cba[i].colorBlendOp = blend_op(b[5]);
            cba[i].alphaBlendOp = blend_op(b[6]);
        }
    }
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = k->ncolor, .pAttachments = cba};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = NDYN, .pDynamicStates = DYN};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = k->prog->layout, .renderPass = k->rp};
    VkResult r = vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &pp->pipe);
    if (r != VK_SUCCESS) {
        static int n;
        if (n++ < 10) LOG("[vk] pipeline creation failed: %d\n", r);
    }
    atomic_fetch_add(&stat_compiles, 1);
    job_stat(2, t0);
    set_state(&pp->state, r == VK_SUCCESS ? 1 : 2, pp->t_req);
}

// ================================================================== feedback: sampling a texture that is an attachment
// (a feedback loop in Vulkan): the draw samples a copy instead, made when the attachment has changed since.
typedef struct { NvnTexture *key; TexBackend copy; bool valid; } Snap;
static Snap snaps[16];
static int nsnaps;
static NvnTexture *feedback_tex[4];
static VkImageView feedback_view[4];
static int nfeedback;

static NvnTexture *shader_texture_object(int stage, ShaderTexture *t) {
    int slot = ((int)t->handle_offset - 0x20) / 8;
    uint64_t h = (slot >= 0 && slot < 32) ? st.tex[stage][slot] : 0;
    uint32_t tic = h & 0xFFFFF;
    if (!h || !st.tpool || tic >= (uint32_t)st.tpool->count) return NULL;
    return ((NvnTexEntry *)st.tpool->entries)[tic].tex;
}

static bool is_attachment(NvnTexture *t) {
    if (!t) return false;
    if (t == st.rt.depth) return true;
    for (int i = 0; i < st.rt.ncolor; i++) if (t == st.rt.color[i]) return true;
    return false;
}

static void snapshots_invalidate(NvnTexture *t) {
    for (int i = 0; i < nsnaps; i++) if (!t || snaps[i].key == t) snaps[i].valid = false;
}

static void resolve_feedback(Prog *pr) {
    nfeedback = 0;
    ShaderInfo *infos[2] = {pr->vi, pr->fi};
    for (int sg = 0; sg < 2; sg++)
        for (int i = 0; i < infos[sg]->ntex; i++) {
            NvnTexture *t = shader_texture_object(sg, &infos[sg]->tex[i]);
            if (!is_attachment(t)) continue;
            bool have = false;
            for (int k = 0; k < nfeedback; k++) have |= feedback_tex[k] == t;
            if (have || nfeedback == 4) continue;
            TexBackend *src = vk_tex(g_fsr_on ? vk_fsr_map(t, false) : t, true);  // (FSR: an output-size stand-in)
            Snap *s = NULL;
            for (int k = 0; k < nsnaps; k++) if (snaps[k].key == t) s = &snaps[k];
            if (!s) {
                if (nsnaps == 16) { static int rr; s = &snaps[rr++ & 15]; } else s = &snaps[nsnaps++];
                if (s->key != t) s->valid = false;
                s->key = t;
            }
            TexBackend *d = &s->copy;
            if (d->image && (d->width != src->width || d->height != src->height || d->format != src->format || d->samples != src->samples)) {
                vk_views_forget(d);
                vk_defer_image(d->image, d->mem);
                d->image = VK_NULL_HANDLE;
                s->valid = false;
            }
            VkImageAspectFlags sa = src->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : src->aspect;
            if (!d->image) {
                VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = src->format,
                                        .extent = {src->width, src->height, 1}, .mipLevels = 1, .arrayLayers = 1,
                                        .samples = (VkSampleCountFlagBits)src->samples, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
                VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
                VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
                d->format = src->format; d->width = src->width; d->height = src->height; d->depth = 1; d->layers = 1; d->levels = 1;
                d->samples = src->samples; d->aspect = src->aspect; d->base_type = VK_IMAGE_VIEW_TYPE_2D;
                vk_end_pass();
                vk_image_init_layout(vk_cmd(), d->image, d->aspect);
            }
            if (!s->valid) {
                vk_end_pass();
                vk_ms_resolve(src);
                VkCommandBuffer cb = vk_cmd();
                vk_full_barrier(cb);
                VkImageCopy rg = {.srcSubresource = {src->aspect, 0, 0, 1}, .dstSubresource = {src->aspect, 0, 0, 1},
                                  .extent = {src->width, src->height, 1}};
                vkCmdCopyImage(cb, src->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
                s->valid = true;
                st.rt_dirty = true;
                stat_snapshots++;
            }
            static const VkComponentMapping id = {0};
            feedback_tex[nfeedback] = t;
            feedback_view[nfeedback++] = vk_image_view(d, VK_IMAGE_VIEW_TYPE_2D, src->format, 0, 1, 0, 1, id, sa);
        }
}

// ================================================================== resource binding
static VkDescriptorBufferInfo ubo_span(VkSpan s, uint64_t size) {
    uint64_t max = g_vk.props.limits.maxUniformBufferRange;
    return (VkDescriptorBufferInfo){s.buf, s.off, size > max ? max : size};
}

static VkDescriptorBufferInfo zero_buffer(uint64_t size) {
    static const uint8_t zeros[0x10000];
    if (size > sizeof zeros) size = sizeof zeros;
    VkSpan s = vk_transient_copy(zeros, size, 256);
    return (VkDescriptorBufferInfo){s.buf, s.off, size};
}

// Constant buffer k of a stage: 0 the driver's (storage buffer table, compute group counts), 1 the compiler constants
// stored after the shader code, 3.. the game's bound uniform buffers. Copied into transient memory.
static VkDescriptorBufferInfo cbuf_info(int stage, ShaderInfo *si, int k, uint32_t gx, uint32_t gy, uint32_t gz) {
    if (k == 0) {
        uint32_t drv[0x800 / 4] = {0};
        if (stage == 5) {
            drv[0x200 / 4] = gx; drv[0x204 / 4] = gy; drv[0x208 / 4] = gz;
            for (int i = 0; i < 16; i++) {
                uint32_t *u = &drv[(0x210 + 16 * i) / 4];
                u[0] = (uint32_t)st.ubuf[5][i].addr; u[2] = (uint32_t)st.ubuf[5][i].size;
                uint32_t *e = &drv[(0x310 + 16 * i) / 4];
                e[0] = (uint32_t)st.sbuf[5][i].addr; e[2] = (uint32_t)st.sbuf[5][i].size;
            }
        } else {
            uint32_t base = 0x110 + 0x100 * (stage == 1 ? 4 : 0);
            for (int i = 0; i < 16; i++) {
                uint32_t *e = &drv[(base + 16 * i) / 4];
                e[0] = (uint32_t)st.sbuf[stage][i].addr;
                e[2] = (uint32_t)st.sbuf[stage][i].size;
            }
        }
        uint64_t size = si->cbuf_size[0] > sizeof drv ? sizeof drv : (si->cbuf_size[0] ? si->cbuf_size[0] : 256);
        if (size < 0x500 && stage == 5) size = 0x500;
        return ubo_span(vk_transient_copy(drv, size, 256), size);
    }
    if (k == 1) {  // compiler constants stored after the shader code
        NvnShaderStage *ss = &st.program->stages[stage];
        uint64_t size = ss->const_size ? ss->const_size : 16;
        if (size < si->cbuf_size[1]) size = si->cbuf_size[1];
        if (!guest_range_ok(ss->code_addr + ss->const_offset, size)) return zero_buffer(size);
        return ubo_span(guest_copy(ss->code_addr + ss->const_offset, size), size);
    }
    int idx = k - 3;  // cbuf_base
    Range r = (idx >= 0 && idx < 18) ? st.ubuf[stage][idx] : (Range){0, 0};
    uint64_t size = si->cbuf_size[k] ? si->cbuf_size[k] : 256;
    if (!r.addr) return zero_buffer(size);
    if ((si->cbuf_dynamic & (1u << k)) && r.size) size = r.size > 0x10000 ? 0x10000 : r.size;
    if (size > r.size && r.size) size = r.size;
    if (size < 16) size = 16;
    if (!guest_range_ok(r.addr, size)) return zero_buffer(size);
    if (stage == 0 && k == 3) ui_relax_scissor = false, ui_scissor_v = -1;
    if (stage == 0 && k == 3 && !st.ds[0] && (pass_h == g_render_size[1] || pass_sy != 1.0f) &&  // (FSR: stand-ins)
        fabsf(g_render_aspect - 16.0f / 9.0f) > 0.01f) {
        if (size >= 64) {
            float m[16];
            memcpy(m, G2H((uint32_t)r.addr), 64);
            float pos_x = m[12], pos_y = m[13], pos_z = m[14];

            // Ignore fullscreen overlays (e.g. screen fade / full backdrop)
            bool is_fullscreen = (fabsf(pos_x - 640.0f) < 1.0f && fabsf(pos_y - 360.0f) < 1.0f &&
                                  (fabsf(m[0]) >= 1200.0f || fabsf(m[5]) >= 700.0f)) ||
                                 (fabsf(pos_x) <= 1.0f && fabsf(pos_y) <= 1.0f);

            if (is_fullscreen && g_hudtrace_active)
                LOG("[hud] z %.1f pos %.1f,%.1f m %.2f %.2f %.2f %.2f: whole screen, kept\n", pos_z, pos_x, pos_y, m[0], m[1], m[4], m[5]);
            if (!is_fullscreen) {
                // The HUD keeps the 16:9 shape it is laid out in: on a wider picture its parts are narrowed (s) and moved
                // toward their side, on a narrower one (16:10) flattened (sy) and moved toward the top or bottom.
                float s = g_render_aspect > 16.0f / 9.0f ? (16.0f / 9.0f) / g_render_aspect : 1.0f;
                float sy = g_render_aspect < 16.0f / 9.0f ? g_render_aspect / (16.0f / 9.0f) : 1.0f;
                // Layer 110 is the loading screen's scrolling backdrop: strips at y 168 / 433 / 570 that must stay one picture.
                // The generic rule anchored each strip to a different edge and flattened it, leaving gaps between them.
                // They keep their height (as on a stretched 16:9 canvas), so nothing moves vertically.
                if (fabsf(pos_z - 110.0f) < 0.5f) {
                    sy = 1.0f;
                    // (only the strips: the lock-on markers and other effects use layer 110 too, mid-hunt, and made
                    // everything after them center-anchored, so the HUD jumped down)
                    if (fabsf(pos_y - 168.0f) < 0.3f || fabsf(pos_y - 433.0f) < 0.3f || fabsf(pos_y - 570.0f) < 0.3f) ui_loading_pass = true;
                }
                float anchor_aspect = g_ui_anchor_aspect;  // (config.c: 16:9 .. the screen's)
                if (anchor_aspect > g_render_aspect) anchor_aspect = g_render_aspect;
                float a_ratio = anchor_aspect / g_render_aspect;
                float anchor_left = 640.0f * (1.0f - a_ratio);
                float anchor_right = 1280.0f - anchor_left;

                bool is_3d_overlay = (pos_z > 180.0f);
                float new_pos_x = pos_x, new_pos_y = pos_y;
                bool grouped = false;
                int vgroup = 1;

                if (is_3d_overlay && g_hudtrace_active)
                    LOG("[hud] z %.1f pos %.1f,%.1f m %.2f %.2f %.2f %.2f: 3D overlay, not moved\n", pos_z, pos_x, pos_y, m[0], m[1], m[4], m[5]);
                if (!is_3d_overlay) {
                    enum { ANCHOR_LEFT, ANCHOR_RIGHT, ANCHOR_CENTER } anchor = ANCHOR_CENTER;
                    // vertically one anchor per layer (or per part of one), so a window drawn in pieces stays together
                    enum { V_TOP, V_BOTTOM, V_CENTER } vanchor = V_CENTER;

                    // Title screen: its layout (layer 108) is placed at the canvas centre and carries the copyright line at
                    // its bottom (y ~698), which the centre anchor moved up into "PRESS A" (layer 100, y 653 / 671, bottom
                    // anchor). Both scale about the centre, so they keep their spacing.
                    if (fabsf(pos_z - 108.0f) < 0.5f && fabsf(pos_x - 640.0f) < 0.5f && fabsf(pos_y - 360.0f) < 0.5f) ui_title_pass = true;
                    if (ui_title_pass && fabsf(pos_z - 100.0f) < 0.5f) {
                        anchor = ANCHOR_CENTER;
                        vanchor = V_CENTER;
                    } else if (ui_loading_pass && pos_z >= 94.0f && pos_z < 109.5f) {
                        // Loading screen: the quest panel's headers (layer 100), its text (101) and the title (105) are one
                        // picture. The generic rules moved them by different amounts (bottom, center, top) and the
                        // "Summary" header landed on the text. All of it scales about the center instead.
                        anchor = ANCHOR_CENTER;
                        vanchor = V_CENTER;
                    } else if (pos_z < 95.0f) {
                        // 94.0: Central notifications & banners ("Time limit: 50 min.")
                        // 60.0: the Guild Card's name plate (top right, under its page counter): by which third it is in
                        anchor = ANCHOR_CENTER;
                        vanchor = pos_y < 240.0f ? V_TOP : pos_y > 480.0f ? V_BOTTOM : V_CENTER;
                    } else if (fabsf(pos_z - 96.0f) < 0.5f) {
                        // 96.0: Dialogue & Help text box (bottom left), and the button guides of the village / Hub (top
                        // left and right)
                        anchor = pos_x >= 650.0f ? ANCHOR_RIGHT : ANCHOR_LEFT;
                        vanchor = pos_y < 360.0f ? V_TOP : V_BOTTOM;
                    } else if (fabsf(pos_z - 97.0f) < 0.5f || fabsf(pos_z - 98.0f) < 0.5f) {
                        // 97.0: Menus and Submenus
                        // 98.0: text drawn over them (the Qty. label of the quantity box): it used the generic rule, which
                        // centered it 36 px below its top-anchored box
                        // Left window (Quest Info: x <= 430) -> Left anchor
                        // Submenu & Pause Menu (Combine, Items, Menu 1/2: x > 430) -> Right anchor
                        if (pos_x > 430.0f) {
                            anchor = ANCHOR_RIGHT;
                        } else {
                            anchor = ANCHOR_LEFT;
                        }
                        // The Quest Complete banner's lower border slides in from y 739: it belongs to the bottom edge.
                        vanchor = pos_y > 650.0f ? V_BOTTOM : V_TOP;
                        if (fabsf(pos_z - 97.0f) < 0.5f) ui_menu_pass = true;
                    } else if (fabsf(pos_z - 100.0f) < 0.5f) {
                        // 100.0: Hunter Arts HUD and bottom right prompts (y > 600) -> Bottom anchor
                        // Target cam selector (right edge, y 42..82: x > 1000, y < 360) -> Top anchor, so it stays by the minimap.
                        // (Windows centered on this layer, x 640, must stay one piece: they keep the Bottom anchor.)
                        anchor = ANCHOR_RIGHT;
                        // The black bar at the top of a cinematic (y 0, its twin at y 720 is Bottom) must stay at the top edge, or the scene shows above it.
                        // The left windows (Request Log info panel, quest card: x < 630, y < 560) are drawn here too, beside the Top-anchored menus
                        // of layer 97: one anchor for the whole window, or it splits. Without a menu in the pass (the results screen, whose
                        // labels on the left and numbers on the right are one table) they stay Bottom like the rest of the layer.
                        vanchor = ((pos_x > 1000.0f && pos_y < 360.0f) || pos_y < 10.0f || (ui_menu_pass && pos_x < 630.0f && pos_y < 560.0f)) ? V_TOP : V_BOTTOM;
                    } else if (fabsf(pos_z - 103.0f) < 0.5f) {
                        // 103.0: In-hunt Status HUD & Item Selector
                        // Status HUD (Clock, HP bar, Stamina, gauges, palico status: y < 500) -> Left anchor
                        // Item Selector wheel (y >= 500) -> Right anchor
                        if (pos_y < 500.0f) {
                            anchor = ANCHOR_LEFT;
                            vanchor = V_TOP;
                        } else {
                            anchor = ANCHOR_RIGHT;
                            vanchor = V_BOTTOM;
                            // the item bar is drawn through a scissor sized for the original layout; it cut the moved bar
                            ui_relax_scissor = true;
                        }
                    } else if (fabsf(pos_z - 105.0f) < 0.5f) {
                        // 105.0: Minimap & Target Cam
                        anchor = ANCHOR_RIGHT;
                        vanchor = V_TOP;
                    } else {
                        // General fallback for other layers
                        if (pos_x <= 430.0f) anchor = ANCHOR_LEFT;
                        else if (pos_x >= 650.0f) anchor = ANCHOR_RIGHT;
                        else anchor = ANCHOR_CENTER;
                        vanchor = pos_y < 240.0f ? V_TOP : pos_y > 480.0f ? V_BOTTOM : V_CENTER;
                        // 99.0: the results screen's rule between its rows (y 266) is drawn here, beside rows of layer 100 that sit
                        // Bottom: no center band, or the line cuts through the numbers.
                        if (fabsf(pos_z - 99.0f) < 0.5f && vanchor == V_CENTER) vanchor = V_BOTTOM;
                    }

                    // Quest results screen: its header and page counter at the top, the Time Left bar at the bottom (layer
                    // 99, its ends at x 80 / 1180, y 643: what marks the screen), and in the middle the Quest Complete
                    // stamp, the reward table, the item lists' parchment (101), its lines (99) and rows (100). The layers'
                    // rules sent those different ways (the rows top, the parchment centre: the first row sat above the
                    // parchment, the header went down). The bar is drawn after the stamp, so the last pass also tells.
                    if (fabsf(pos_z - 99.0f) < 0.5f && fabsf(pos_y - 643.0f) < 0.5f && (fabsf(pos_x - 80.0f) < 0.5f || fabsf(pos_x - 1180.0f) < 0.5f))
                        ui_results_pass = true;
                    if ((ui_results_pass || ui_results_last) && pos_z > 98.5f && pos_z < 101.5f)
                        vanchor = pos_y < 60.0f ? V_TOP : pos_y > 600.0f ? V_BOTTOM : V_CENTER;

                    if (anchor == ANCHOR_LEFT) {
                        new_pos_x = anchor_left + pos_x * s;
                    } else if (anchor == ANCHOR_RIGHT) {
                        new_pos_x = anchor_right - (1280.0f - pos_x) * s;
                    } else {
                        new_pos_x = 640.0f + (pos_x - 640.0f) * s;
                    }
                    int v = vanchor == V_TOP ? 0 : vanchor == V_BOTTOM ? 2 : 1;
                    ui_recent[ui_nrecent % UI_RECENT].x = pos_x, ui_recent[ui_nrecent % UI_RECENT].y = pos_y;
                    ui_recent[ui_nrecent % UI_RECENT].vanchor = (uint8_t)v;
                    ui_nrecent++;
                    if (vanchor == V_TOP) new_pos_y = pos_y * sy;
                    else if (vanchor == V_BOTTOM) new_pos_y = 720.0f - (720.0f - pos_y) * sy;
                    else new_pos_y = 360.0f + (pos_y - 360.0f) * sy;
                    // One anchor per window (ui_group_end), on a picture narrower than 16:9. Not for the hunt's status HUD
                    // and item bar (103), the minimap (105), the loading screen, or the bars that belong to an edge (a
                    // cinematic's at y 0 / 720, the Quest Complete border sliding in from y 739).
                    grouped = sy < 1.0f && !(ui_loading_pass && pos_z >= 94.0f && pos_z < 109.5f) && fabsf(pos_z - 103.0f) >= 0.5f &&
                              fabsf(pos_z - 105.0f) >= 0.5f && pos_y >= 10.0f && pos_y <= 712.0f;
                    vgroup = v;
                    if (g_hudtrace_active)
                        LOG("[hud] z %.1f pos %.1f,%.1f m %.2f %.2f %.2f %.2f -> %c%c %.1f,%.1f%s sc %d,%d %dx%d\n", pos_z, pos_x, pos_y, m[0], m[1], m[4],
                            m[5], "LRC"[anchor], "TBC"[vanchor], new_pos_x, new_pos_y, ui_menu_pass ? " menu" : "", st.scissor[0][0],
                            st.scissor[0][1], st.scissor[0][2], st.scissor[0][3]);
                }

                m[0] *= s; m[4] *= s; m[8] *= s;
                m[1] *= sy; m[5] *= sy; m[9] *= sy;
                m[12] = new_pos_x;
                m[13] = new_pos_y;

                VkSpan sp = vk_transient_copy(G2H((uint32_t)r.addr), size, 256);  // (all of it, the matrix changed)
                memcpy(sp.ptr, m, 64);
                if (grouped) ui_group_add((float *)sp.ptr, pos_z, pos_x, pos_y, vgroup);
                // The game clips some windows (a list, the help windows' body) with a scissor laid out for the 16:9 window:
                // it moves with the piece, as its group was anchored last frame (the group is not complete yet), or for a
                // group not seen before as the pieces so far are.
                if (sy < 1.0f && !is_3d_overlay) {
                    int gv = !grouped ? -1 : ui_group.predict >= 0 ? ui_group.predict : ui_group_vanchor();
                    ui_scissor_v = gv >= 0 ? gv : vgroup;
                }
                return ubo_span(sp, size);
            }
        }
    }
    return ubo_span(guest_copy(r.addr, size), size);
}

static int32_t fsr_cap_base;  // (FSR capture draw: its FSR_CAP_BASE_BINDING value, stage_set)

static void texture_info(int stage, ShaderInfo *si, int i, VkDescriptorImageInfo *out) {
    ShaderTexture *stx = &si->tex[i];
    int slot = ((int)stx->handle_offset - 0x20) / 8;  // 64-bit handles from offset 0x20
    uint64_t h = (slot >= 0 && slot < 32) ? st.tex[stage][slot] : 0;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler smp = VK_NULL_HANDLE;
    uint32_t tic = h & 0xFFFFF, tsc = (uint32_t)(h >> 20) & 0xFFF;
    if (h && st.tpool && tic < (uint32_t)st.tpool->count) {
        NvnTexEntry *e = &((NvnTexEntry *)st.tpool->entries)[tic];
        if (e->tex) {
            view = vk_view(g_fsr_on ? vk_fsr_map(e->tex, false) : e->tex, &e->view, e->has_view, false, stx->type);  // (FSR)
            for (int k = 0; k < nfeedback; k++)
                if (e->tex == feedback_tex[k] && (stx->type == TEXTYPE_2D || stx->type == TEXTYPE_2D_ARRAY))
                    view = stx->type == TEXTYPE_2D ? feedback_view[k] : vk_dummy_view(TEXTYPE_2D_ARRAY);
        }
    }
    if (h && st.spool && tsc < (uint32_t)st.spool->count) smp = vk_sampler(&((NvnSamplerDesc *)st.spool->entries)[tsc]);
    if (!view) view = vk_dummy_view(stx->type);
    if (!smp) smp = vk_default_sampler(true);
    *out = (VkDescriptorImageInfo){smp, view, VK_IMAGE_LAYOUT_GENERAL};
}

// Writes the descriptor set of one stage. gx..gz: compute group counts (driver constants).
static VkDescriptorSet stage_set(int nbinds, const VkBindingInfo *binds, VkDescriptorSetLayout layout, uint32_t set, int stage,
                                 ShaderInfo *si, uint32_t gx, uint32_t gy, uint32_t gz) {
    VkDescriptorSet ds = vk_alloc_set(layout);
    VkWriteDescriptorSet w[MAX_BIND];
    VkDescriptorBufferInfo bi[MAX_BIND];
    VkDescriptorImageInfo ii[MAX_BIND];
    int n = 0;
    for (int i = 0; i < nbinds; i++) {
        const VkBindingInfo *b = &binds[i];
        if (b->set != set) continue;
        w[n] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = b->binding,
                                      .descriptorCount = 1, .descriptorType = b->type};
        if (b->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && b->binding >= 32) {
            int ti = (int)b->binding - 32;
            if (ti < si->ntex) texture_info(stage, si, ti, &ii[n]);
            else ii[n] = (VkDescriptorImageInfo){vk_default_sampler(true), vk_dummy_view(TEXTYPE_2D), VK_IMAGE_LAYOUT_GENERAL};
            w[n].pImageInfo = &ii[n];
        } else if (b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && b->binding < 17) {
            bi[n] = cbuf_info(stage, si, (int)b->binding, gx, gy, gz);
            w[n].pBufferInfo = &bi[n];
        } else if (b->binding == FSR_CAP_OUT_BINDING && b->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {  // (FSR capture)
            bi[n] = vk_fsr_cap_buffer();
            w[n].pBufferInfo = &bi[n];
        } else if (b->binding == FSR_CAP_BASE_BINDING && b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
            int32_t base[4] = {fsr_cap_base, 0, 0, 0};
            bi[n] = ubo_span(vk_transient_copy(base, sizeof base, 256), sizeof base);
            w[n].pBufferInfo = &bi[n];
        } else if (b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && b->binding == 18) {  // rscale: no scaling here
            float rs[66][4];
            for (int k = 0; k < 66; k++) rs[k][0] = rs[k][1] = 1, rs[k][2] = rs[k][3] = 0;
            bi[n] = ubo_span(vk_transient_copy(rs, sizeof rs, 256), sizeof rs);
            w[n].pBufferInfo = &bi[n];
        } else if (b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && b->binding == ALPHA_REF_BUFFER_INDEX) {
            float ref[4] = {st.alpha_ref, 0, 0, 0};
            bi[n] = ubo_span(vk_transient_copy(ref, sizeof ref, 256), sizeof ref);
            w[n].pBufferInfo = &bi[n];
        } else if (b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {  // tint (19), gmap (20): zeros
            bi[n] = zero_buffer(0x200);
            w[n].pBufferInfo = &bi[n];
        } else if (b->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {  // global memory: not available (zeros)
            bi[n] = zero_buffer(0x1000);
            w[n].pBufferInfo = &bi[n];
        } else {
            continue;
        }
        n++;
    }
    if (n) vkUpdateDescriptorSets(g_vk.dev, (uint32_t)n, w, 0, NULL);
    return ds;
}

// ================================================================== compute
typedef struct { Prog p; ShaderInfo *si; VkPipeline pipe; uint32_t local[3]; } CsProg;

static void build_compute(void *arg) {
    CsProg *cp = arg;
    size_t n = 0;
    uint32_t *spv = vk_glsl_to_spirv(cp->si->msl, STAGE_COMPUTE, "cs", &n);
    if (!spv) { set_state(&cp->p.state, 2, 0); return; }
    cp->p.nbinds = vk_reflect(spv, n, cp->p.binds, MAX_BIND);
    VkShaderModule m = vk_module(spv, n);
    free(spv);
    cp->p.layout = make_layout(&cp->p, 1, VK_SHADER_STAGE_COMPUTE_BIT, 0);
    VkSpecializationMapEntry me[3] = {{0, 0, 4}, {1, 4, 4}, {2, 8, 4}};
    VkSpecializationInfo spec = {3, me, 12, cp->local};
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main",
                                                .pSpecializationInfo = &spec},
                                      .layout = cp->p.layout};
    VkResult r = vkCreateComputePipelines(g_vk.dev, g_vk.pcache, 1, &ci, NULL, &cp->pipe);
    atomic_fetch_add(&stat_compiles, 1);
    set_state(&cp->p.state, r == VK_SUCCESS ? 1 : 2, 0);
    if (r != VK_SUCCESS) LOG("[compute] pipeline failed: %d\n", r);
}

static struct { uint64_t addr; CsProg *cp; } g_cs_tab[256];
static int g_cs_n;

static CsProg *compute_program(NvnProgram *np) {
    NvnShaderStage *s5 = &np->stages[5];
    if (!s5->code_addr) return NULL;
    for (int i = 0; i < g_cs_n; i++) if (g_cs_tab[i].addr == s5->code_addr) return g_cs_tab[i].cp;
    CsProg *cp = calloc(1, sizeof *cp);
    if (g_cs_n < 256) { g_cs_tab[g_cs_n].addr = s5->code_addr; g_cs_tab[g_cs_n++].cp = cp; }
    maxwell_gmem_base = HEAP_BASE;
    maxwell_gmem_size = HEAP_MAX;
    ShaderKey k = {0};
    k.glsl = 1;
    pthread_mutex_lock(&translate_lock);
    cp->si = maxwell_translate((const uint8_t *)G2H((uint32_t)s5->code_addr), s5->const_offset ? s5->const_offset : 0x10000, STAGE_COMPUTE, &k);
    pthread_mutex_unlock(&translate_lock);
    cp->p.vi = cp->si;
    if (!cp->si->msl) {
        LOG("[compute] translate failed: %s (code %llx)\n", cp->si->error, (unsigned long long)s5->code_addr);
        set_state(&cp->p.state, 2, 0);
        return cp;
    }
    if (cp->si->uses_gmem) LOG("[compute] %llx uses global memory (not available: zeros)\n", (unsigned long long)s5->code_addr);
    for (int i = 0; i < 3; i++) {
        cp->local[i] = s5->control ? *(const uint32_t *)(s5->control + 0x748 + 4 * i) : 1;
        if (!cp->local[i]) cp->local[i] = 1;
    }
    save_glsl(cp->si->msl, s5->code_addr, "cs");
    run_job(build_compute, cp);
    return cp;
}

static void dispatch_compute(uint32_t gx, uint32_t gy, uint32_t gz) {
    snapshots_invalidate(NULL);
    if (!st.program || !gx || !gy || !gz) return;
    CsProg *cp = compute_program(st.program);
    if (!cp) { stat_skipped++; return; }
    if (!await_build(&cp->p.state, cp, 0)) { skip_draw(&stat_skip_pipe); return; }
    vk_end_pass();
    st.rt_dirty = true;
    ms_resolve_sampled(5, cp->si);
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    nfeedback = 0;
    VkDescriptorSet ds = stage_set(cp->p.nbinds, cp->p.binds, cp->p.set_layout[0], 0, 5, cp->si, gx, gy, gz);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cp->pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cp->p.layout, 0, 1, &ds, 0, NULL);
    vkCmdDispatch(cb, gx, gy, gz);
    stat_dispatches++;
}

// ================================================================== draws
// Quads / fans / polygons as a 32-bit triangle index list.
static uint32_t *expand_indices(uint32_t mode, uint32_t count, const void *indices, int itype, uint32_t first, uint32_t *out_count) {
    uint32_t tris = mode == 7 ? count / 4 * 2 : count >= 3 ? count - 2 : 0;
    uint32_t *out = malloc(tris * 3 * 4 + 4);
    uint32_t n = 0;
#define IDX(i) (indices ? (itype == 0 ? ((const uint8_t *)indices)[i] : itype == 1 ? ((const uint16_t *)indices)[i] : ((const uint32_t *)indices)[i]) : first + (i))
    if (mode == 7) {
        for (uint32_t q = 0; q + 3 < count; q += 4) {
            out[n++] = IDX(q); out[n++] = IDX(q + 1); out[n++] = IDX(q + 2);
            out[n++] = IDX(q); out[n++] = IDX(q + 2); out[n++] = IDX(q + 3);
        }
    } else {
        for (uint32_t i = 1; i + 1 < count; i++) { out[n++] = IDX(0); out[n++] = IDX(i); out[n++] = IDX(i + 1); }
    }
#undef IDX
    *out_count = n;
    return out;
}

static bool topology(uint32_t mode, VkPrimitiveTopology *t) {
    switch (mode) {
    case 0: *t = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; return true;
    case 1: *t = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; return true;
    case 3: *t = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; return true;
    case 4: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; return true;
    case 5: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; return true;
    default: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; return false;
    }
}

// The fragment code's content hash: the same in every run and area (its address is not).
static uint64_t prog_fs_hash(Prog *p) {
    if (!p->chash[1]) p->chash[1] = code_hash(p->code[1], p->cmax[1], 1);
    return p->chash[1];
}

// [Anti-aliasing] AntiAliasing: the game's FXAA pass (recognised by its shader) draws as it is (Game), copies its
// input instead (Off), or is FXAA 3.11 Quality (FXAA: fxaa_311) or SMAA 1x (SMAA: smaa.c) over the same input. The
// game's is an early FXAA 3 Quality with an edge threshold of 1/6, its sub-pixel blur capped at 0.75 and 6 looks per
// side in 2-pixel steps (about 13 pixels).
#define FXAA_FS_HASH 0x8530ded68f4407dfull
static void copy_texture(const TexRef *src, const TexRef *dst, uint64_t flags);

// FXAA 3.11's quality presets as [Anti-aliasing] FXAAQuality 1..5 picks them (UE5's r.FXAA.Quality): 10, 13, 15,
// 29, 39. The edge search's steps, in pixels: the first look, then each further one (the last is taken without a look).
static const struct { int n; float step[12]; } fxaa_presets[5] = {
    {3, {1.5f, 3.0f, 12.0f}},
    {6, {1.0f, 1.5f, 2.0f, 2.0f, 4.0f, 12.0f}},
    {8, {1.0f, 1.5f, 2.0f, 2.0f, 2.0f, 2.0f, 4.0f, 12.0f}},
    {12, {1.0f, 1.5f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 4.0f, 8.0f}},
    {12, {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.5f, 2.0f, 2.0f, 2.0f, 2.0f, 4.0f, 8.0f}}};
static VkDescriptorSetLayout fxaa_set_layout;
static VkPipelineLayout fxaa_layout;

static VkPipeline fxaa_pipeline(VkRenderPass rp) {
    static struct { VkRenderPass rp; VkPipeline p; } tab[8];
    static int n;
    for (int i = 0; i < n; i++) if (tab[i].rp == rp) return tab[i].p;
    if (!fxaa_layout) {
        VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &fxaa_set_layout));
        VkPushConstantRange r = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                         .pSetLayouts = &fxaa_set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &fxaa_layout));
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("resolve.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader("fxaa.frag"), .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1,
                                            .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState att = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = 1, .pAttachments = &att};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2,
                                           .pDynamicStates = dyn};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
                                       .pViewportState = &vs, .pRasterizationState = &rs, .pMultisampleState = &ms,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = fxaa_layout, .renderPass = rp};
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p) != VK_SUCCESS) LOG("[vk] FXAA pipeline failed\n");
    if (n < 8) tab[n].rp = rp, tab[n++].p = p;
    return p;
}

// The game's FXAA pass about to be replaced: the pass ended, src and the colour target resolved, their views and the
// target's size (false: not possible, the game's own pass draws instead). post_aa_drawn after a replacement drew.
static bool post_aa_target(NvnTexture *src, VkPostAa *t) {
    NvnTexture *dt = st.rt.color[0];
    TexBackend *s = vk_tex(src, false), *d = vk_tex(dt, true);
    if (s->samples != 1 || d->samples != 1) return false;
    vk_end_pass();
    st.rt_dirty = true;
    vk_ms_resolve(s);
    vk_ms_resolve(d);
    bool has = st.rt.has_cview[0];
    t->src = vk_view(src, NULL, false, false, TEXTYPE_2D);
    t->dst = vk_view(dt, &st.rt.cview[0], has, true, TEXTYPE_2D);
    t->dst_image = d->image;
    t->format = has && (st.rt.cview[0].setmask & 4) && vk_format(dt->format) == d->format ? vk_format(st.rt.cview[0].format)
                                                                                         : d->format;
    uint32_t lv = has && (st.rt.cview[0].setmask & 1) ? st.rt.cview[0].base_level : 0;
    t->w = level_extent(d->width, lv), t->h = level_extent(d->height, lv);
    return t->src && t->dst;
}
static void post_aa_drawn(void) {
    vk_tex(st.rt.color[0], true)->ms_valid = false;
    renderer_mark_gpu_owned(st.rt.color[0]);
    ss_forget();
    stat_draws++;
}

// FXAA 3.11 from the game's FXAA input into its target, pixel for pixel as Off's copy; false: not drawn.
static bool fxaa_311(const VkPostAa *t) {
    VkRenderPass rp = vk_render_pass(&t->format, 1, VK_FORMAT_UNDEFINED, 1);
    VkPipeline p = fxaa_pipeline(rp);
    if (!p) return false;
    VkImageView sv = t->src, dv = t->dst;
    uint32_t w = t->w, h = t->h;
    int q = g_config.fxaa_quality >= 1 && g_config.fxaa_quality <= 5 ? g_config.fxaa_quality - 1 : 4;
    float pc[16] = {g_config.fxaa_subpix, g_config.fxaa_edge_threshold, g_config.fxaa_edge_threshold_min, (float)fxaa_presets[q].n};
    memcpy(pc + 4, fxaa_presets[q].step, sizeof fxaa_presets[q].step);
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkDescriptorSet set = vk_alloc_set(fxaa_set_layout);
    VkDescriptorImageInfo ii = {vk_default_sampler(true), sv, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    vkUpdateDescriptorSets(g_vk.dev, 1, &wr, 0, NULL);
    VkRect2D area = {{0, 0}, {w, h}};
    VkRenderPassBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp,
                                .framebuffer = vk_framebuffer(rp, &dv, 1, w, h), .renderArea = area};
    vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, fxaa_layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(cb, fxaa_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc);
    VkViewport vp = {0, 0, (float)w, (float)h, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &area);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRenderPass(cb);
    static bool logged;
    if (!logged) logged = true, LOG("[vk] FXAA 3.11 (quality %d) draws in place of the game's FXAA, %ux%u\n", q + 1, w, h);
    return true;
}

static bool fxaa_replaced(Prog *pr) {
    int mode = config_live_aa();
    if (mode == AA_GAME || prog_fs_hash(pr) != FXAA_FS_HASH || pr->fi->ntex != 1 || !st.rt.ncolor || !st.rt.color[0])
        return false;
    NvnTexture *src = shader_texture_object(1, &pr->fi->tex[0]);
    if (!src || src == st.rt.color[0]) return false;
    if (mode == AA_SMAA_T2X && !g_temporal_on) mode = AA_SMAA;  // (until T2x runs, from the next frame on: SMAA 1x)
    if (mode == AA_FXAA || mode == AA_SMAA) {
        VkPostAa t;
        if (!post_aa_target(src, &t) || !(mode == AA_SMAA ? vk_smaa(&t, config_live_smaa_quality()) : fxaa_311(&t)))
            return false;
        post_aa_drawn();
        return true;
    }
    TexRef s = {.tex = (uint64_t)(uintptr_t)src}, d = {.tex = (uint64_t)(uintptr_t)st.rt.color[0]};
    if (st.rt.has_cview[0]) d.view = st.rt.cview[0], d.has_view = 1;
    copy_texture(&s, &d, 0);
    stat_draws++;
    return true;
}

// A movie frame: the game draws the decoder's NV12 planes (hle/movie.c), an R8 picture and its half-size RG8 colour.
// Its picture (NULL: not a movie frame).
static NvnTexture *movie_picture(Prog *pr) {
    NvnTexture *y = NULL, *uv = NULL;
    for (int i = 0; i < pr->fi->ntex; i++) {
        NvnTexture *t = shader_texture_object(1, &pr->fi->tex[i]);
        if (t && t->format == 0x1) y = t;        // R8
        else if (t && t->format == 0xd) uv = t;  // RG8
    }
    if (!y || !uv || uv->width * 2 != y->width || uv->height * 2 != y->height || !y->height) return NULL;
    // The game writes each frame into one of a few texture pairs it takes turns with. Until a texture was seen to
    // change, its contents were checked only every 8th frame (textures.c upload_texture): for the first second of a
    // movie, some frames showed a pair's old contents, black or a dark Y under the new colour (black flickers).
    renderer_mark_dynamic(y);
    renderer_mark_dynamic(uv);
    return y;
}

// A movie frame keeps the movie's shape. The game draws it over the whole target, which on a picture of another shape
// (16:10, 21:9) stretched it: drawn instead in the largest rectangle of the movie's shape, centred, the pass's clear
// showing as bars.
static bool movie_viewport(NvnTexture *y, const float *vp, float out[4]) {
    float pw, ph;
    pass_guest_size(&pw, &ph);
    if (vp[0] != 0 || vp[1] != 0 || vp[2] != pw || vp[3] != ph || !pass_h) return false;
    float movie = (float)y->width / (float)y->height, screen = vp[2] / vp[3];
    if (fabsf(movie / screen - 1.0f) < 0.01f) return false;
    float w = vp[2], h = vp[3];
    if (screen > movie) w = h * movie;
    else h = w / movie;
    out[0] = (vp[2] - w) * 0.5f, out[1] = (vp[3] - h) * 0.5f, out[2] = w, out[3] = h;
    return true;
}

static void draw(CmdRec *r, bool indexed) {
    if (!st.program) { stat_skipped++; return; }
    ShaderKey vkey = {0}, fkey = {0};
    vkey.glsl = fkey.glsl = 1;
    vkey.flip_y = fkey.flip_y = nvn_window_origin() == 0;
    vkey.depth_minus_one = nvn_depth_mode() == 0;
    for (int i = 0; i < st.nattrib; i++) vkey.attr_kind[i] = (uint8_t)attr_kind(st.attrib[i] & 0xFF);
    for (int i = 0; i < st.rt.ncolor; i++) if (st.rt.color[i]) fkey.rt_kind[i] = (uint8_t)rt_kind(st.rt.color[i]->format);
    if (st.color_state[2] >= 1 && st.color_state[2] <= 7) fkey.alpha_func = st.color_state[2];  // (NVN alpha test)
    Prog *pr = get_program(st.program, &vkey, &fkey);
    if (!pr) { stat_skipped++; return; }
    if (!await_build(&pr->state, pr, 0)) { skip_draw(&stat_skip_prog); return; }
    if (fxaa_replaced(pr)) return;
    NvnTexture *movie = movie_picture(pr);
    uint32_t mode = (uint32_t)r->x[0];
    VkPrimitiveTopology topo;
    bool native = topology(mode, &topo);
    if (!native && mode != 6 && mode != 7 && mode != 9) { stat_skipped++; return; }
    if (g_temporal_on) {  // FSR / SMAA T2x: the frame's structure; at the start of the HUD they run before this draw (fsr.c)
        FsrDraw fd = {.color0 = st.rt.ncolor ? st.rt.color[0] : NULL, .depth = st.rt.depth, .count = (uint32_t)r->x[2],
                      .depth_write = st.ds[0] && st.ds[1], .vubuf = &st.ubuf[0][0].addr, .nvubuf = 18};
        for (int i = 0; i < pr->fi->ntex && fd.ntex < 16; i++) fd.tex[fd.ntex++] = shader_texture_object(1, &pr->fi->tex[i]);
        vk_fsr_draw(&fd);
    }

    if (st.rt_dirty) vk_end_pass();  // (MSAA: the ending pass marks its targets for resolving before they are read)
    ms_resolve_sampled(0, pr->vi);
    ms_resolve_sampled(1, pr->fi);
    resolve_feedback(pr);
    if (st.rt.depth && st.ds[0] && st.ds[1]) snapshots_invalidate(st.rt.depth);
    for (int i = 0; i < st.rt.ncolor; i++)
        if (st.rt.color[i] && ((st.channel_mask >> (i * 4)) & 0xF)) snapshots_invalidate(st.rt.color[i]);
    begin_pass();
    VkCommandBuffer cb = vk_cmd();
    int af = fkey.alpha_func;
    if (pass_samples > 1 && (af == 2 || af == 4 || af == 5 || af == 7)) {  // MSAA: the alpha test as sample coverage
        fkey.alpha_coverage = (uint8_t)pass_samples;
        pr = get_program(st.program, &vkey, &fkey);
        if (!pr) { stat_skipped++; return; }
        if (!await_build(&pr->state, pr, 0)) { skip_draw(&stat_skip_prog); return; }
    }

    // ---- pipeline
    PipeKey k;
    memset(&k, 0, sizeof k);
    k.prog = pr;
    k.rp = pass_rp;
    k.ncolor = pass_ncolor;
    k.samples = pass_samples;
    for (int i = 0; i < 32; i++) {
        if (!(pr->vi->attr_mask & (1u << i))) continue;
        k.attr_mask |= 1u << i;
        uint32_t a = i < st.nattrib ? st.attrib[i] : 0x2E;
        uint32_t fmt = a & 0xFF, stream = (a >> 8) & 0x1F, off = a >> 13;
        if (i >= st.nattrib || fmt == 0 || stream >= 16) {  // unbound attribute: (0, 0, 0, 1) from a constant stream
            k.attr_fmt[i] = VK_FORMAT_R32G32B32A32_SFLOAT;
            k.attr_bind[i] = ZERO_BINDING;
            k.used_streams |= 1u << ZERO_BINDING;
            continue;
        }
        k.attr_fmt[i] = vertex_format_supported(vertex_format(fmt));
        k.attr_off[i] = off;
        k.attr_bind[i] = stream;
        k.used_streams |= 1u << stream;
    }
    for (int s = 0; s < 16; s++) {
        if (!(k.used_streams & (1u << s))) continue;
        k.stride[s] = s < st.nstream ? st.stream[s][0] : 0;
        k.divisor[s] = s < st.nstream && k.stride[s] ? st.stream[s][1] : 0;
        if (k.divisor[s] > 1 && !g_vk.divisor) k.divisor[s] = 1;
    }
    for (uint32_t i = 0; i < k.ncolor && i < 8; i++) {
        if (!st.rt.color[i] || pass_color[i] == VK_FORMAT_UNDEFINED) continue;
        // targets the fragment shader doesn't write keep their contents (undefined in Vulkan otherwise): masked
        k.write_mask[i] = ((pr->fi->out_mask >> (i * 4)) & 0xF) ? (st.channel_mask >> (i * 4)) & 0xF : 0;
        if (((st.color_state[0] >> i) & 1) && !rt_kind(st.rt.color[i]->format)) {
            k.blend_on |= (uint8_t)(1u << i);
            memcpy(k.blend[i], st.blend[i], 8);
        }
    }
    {  // (debug) MHGU_FORCE_WMASK=<fragment code address>: that shader writes all channels of target 0 (override shaders)
        static uint64_t fw = ~0ull;
        if (fw == ~0ull) { const char *e = getenv("MHGU_FORCE_WMASK"); fw = e ? strtoull(e, NULL, 16) : 0; }
        if (fw && st.program->stages[1].code_addr == fw && k.ncolor) k.write_mask[0] = 0xF;
    }
    k.a2c = st.ms[0xC] != 0;
    k.topology = (uint8_t)(native ? topo : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    k.poly_fill = st.poly[2] != 1;
    k.cull = st.poly[0];
    bool ccw = st.poly[1] == 1;
    k.front_ccw = ccw != vkey.flip_y;
    k.prim_restart = st.prim_restart && native && (mode == 3 || mode == 5);
    k.depth_clamp = st.depth_clamp && g_vk.depth_clamp;  // (shadow casters beyond the light's near / far planes)
    k.depth_bias = (st.poly[3] & 4) != 0;
    bool has_depth = pass_depth != VK_FORMAT_UNDEFINED && pass_depth != VK_FORMAT_S8_UINT;
    bool has_stencil = pass_depth == VK_FORMAT_D24_UNORM_S8_UINT || pass_depth == VK_FORMAT_D32_SFLOAT_S8_UINT || pass_depth == VK_FORMAT_S8_UINT;
    k.depth_test = st.ds[0] && has_depth;
    k.depth_write = k.depth_test && st.ds[1];
    k.depth_op = (uint8_t)(k.depth_test ? compare_fn(st.ds[2]) : VK_COMPARE_OP_ALWAYS);
    k.stencil_test = st.ds[3] && has_stencil;
    if (k.stencil_test) {
        uint32_t ops = st.ds[5] | st.ds[6] << 8 | st.ds[7] << 16;
        for (int f = 0; f < 2; f++) {
            uint32_t o = f ? ops >> 12 : ops;
            k.sten_ops[f][0] = (uint8_t)stencil_op(o & 0xF);
            k.sten_ops[f][1] = (uint8_t)stencil_op((o >> 8) & 0xF);
            k.sten_ops[f][2] = (uint8_t)stencil_op((o >> 4) & 0xF);
            k.sten_ops[f][3] = (uint8_t)compare_fn(f ? st.ds[4] >> 4 : st.ds[4] & 0xF);
        }
    }
    bool created;
    Pipe *pp = pipe_slot(&k, &created);
    if (!pp) { stat_skipped++; return; }
    if (created) {
        pp->t_req = plat_now_ns();
        pipecache_store(pp);
        run_job(build_pipeline, pp);
    }
    // A movie frame is the whole picture: skipped, the screen showed the pass's black clear for a few frames at the
    // start of the first movie after an update (its pipeline not in cache/pipelines.bin yet, ~2 frames to build). It
    // waits for its pipeline, so the picture before it stays up that long instead.
    if (!await_build(&pp->state, pp, movie ? 2000000000ll : 0)) { skip_draw(&stat_skip_pipe); return; }
    if (ss_need(SS_PIPE, ss.pipe == pp->pipe)) { ss.pipe = pp->pipe; vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pp->pipe); }

    // ---- dynamic state
    if (k.stencil_test) {
        for (int f = 0; f < 2; f++) {
            uint32_t v[3] = {st.stencil_vmask[f] & 0xFF, st.stencil_mask[f] & 0xFF, st.stencil_ref[f]};
            if (!ss_need(SS_STEN << (f * 8), !memcmp(v, ss.sten[f], sizeof v))) continue;
            memcpy(ss.sten[f], v, sizeof v);
            VkStencilFaceFlags face = f ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT;
            vkCmdSetStencilCompareMask(cb, face, v[0]);
            vkCmdSetStencilWriteMask(cb, face, v[1]);
            vkCmdSetStencilReference(cb, face, v[2]);
        }
    }
    if (ss_need(SS_BC, !memcmp(ss.bc, st.blend_color, sizeof ss.bc))) { memcpy(ss.bc, st.blend_color, sizeof ss.bc); vkCmdSetBlendConstants(cb, st.blend_color); }
    {
        float f[3] = {0, 0, 0};
        if (k.depth_bias) memcpy(f, st.poly_offset, sizeof f);
        if (ss_need(SS_DB, !memcmp(ss.db, f, sizeof f))) { memcpy(ss.db, f, sizeof f); vkCmdSetDepthBias(cb, f[1], f[2], f[0]); }
    }
    float *vp = st.viewport[0], movie_vp[4], v[4];
    PreviewMap pm;
    if (movie && movie_viewport(movie, vp, movie_vp)) vp = movie_vp;
    memcpy(v, vp, sizeof v);
    if (vp != movie_vp && ui_preview_map(&pm)) {  // (a model preview: the viewport keeps its size, its centre moves)
        v[0] = pm.sx + (vp[0] + vp[2] * 0.5f) * pm.kx - vp[2] * 0.5f;
        v[1] = pm.sy + (vp[1] + vp[3] * 0.5f) * pm.ky - vp[3] * 0.5f;
    }
    set_viewport(cb, v[0] * pass_sx, v[1] * pass_sy, v[2] * pass_sx, v[3] * pass_sy, st.depth_range[0][0],
                 st.depth_range[0][1]);  // (FSR: pass_s* scale onto output-size stand-ins, else 1)
    apply_scissor(cb);

    // ---- descriptor sets
    VkDescriptorSet sets[2] = {stage_set(pr->nbinds, pr->binds, pr->set_layout[0], 0, 0, pr->vi, 0, 0, 0),
                               stage_set(pr->nbinds, pr->binds, pr->set_layout[1], 1, 1, pr->fi, 0, 0, 0)};
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pr->layout, 0, 2, sets, 0, NULL);
    if (ui_relax_scissor || ui_scissor_v >= 0) {  // (set while the constants above were built: for this draw only; the
        apply_scissor(cb);                         // next draw and any clear before it get the game's scissor again)
        ui_relax_scissor = false, ui_scissor_v = -1;
    }

    // ---- indices (copied; the largest index bounds the vertex data copied below)
    uint32_t first = 0, count = (uint32_t)r->x[2], inst, base_inst, max_index = 0;
    int32_t base_vertex = 0;
    VkSpan ib = {0};
    VkIndexType itype_vk = VK_INDEX_TYPE_UINT32;
    uint32_t icount = 0;
    bool use_index = indexed || !native;
    if (!indexed) {
        first = (uint32_t)r->x[1]; base_inst = (uint32_t)r->x[3]; inst = (uint32_t)r->x[4] ? (uint32_t)r->x[4] : 1;
        max_index = first + count;
        if (!native) {
            uint32_t *e = expand_indices(mode, count, NULL, 0, first, &icount);
            ib = vk_transient_copy(e, (uint64_t)icount * 4 + 4, 4);
            free(e);
        }
    } else {
        uint32_t itype = (uint32_t)r->x[1];
        uint64_t iaddr = r->x[3];
        base_vertex = (int32_t)r->x[4];
        base_inst = (uint32_t)r->x[5];
        inst = (uint32_t)r->x[6] ? (uint32_t)r->x[6] : 1;
        uint32_t isz = itype == 0 ? 1 : itype == 1 ? 2 : 4;
        if (!guest_range_ok(iaddr, (uint64_t)count * isz)) { stat_skipped++; return; }
        const void *src = G2H((uint32_t)iaddr);
        uint32_t restart = st.prim_restart ? st.restart_index : 0xFFFFFFFFu;
        if (native && itype != 0) {
            itype_vk = itype == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
            icount = count;
            ib = vk_transient_copy(src, (uint64_t)count * isz + 4, 4);
            if (isz == 2) { const uint16_t *s = src; for (uint32_t i = 0; i < count; i++) if (s[i] > max_index && s[i] != (uint16_t)restart) max_index = s[i]; }
            else { const uint32_t *s = src; for (uint32_t i = 0; i < count; i++) if (s[i] > max_index && s[i] != restart) max_index = s[i]; }
        } else if (native) {  // 8-bit indices: widened to 16
            ib = vk_transient((uint64_t)count * 2 + 4, 4);
            uint16_t *w = ib.ptr;
            const uint8_t *s = src;
            for (uint32_t i = 0; i < count; i++) { w[i] = s[i] == 0xFF && st.prim_restart ? 0xFFFF : s[i]; if (s[i] > max_index && w[i] != 0xFFFF) max_index = s[i]; }
            itype_vk = VK_INDEX_TYPE_UINT16;
            icount = count;
        } else {
            uint32_t *e = expand_indices(mode, count, src, (int)itype, 0, &icount);
            for (uint32_t i = 0; i < icount; i++) if (e[i] > max_index) max_index = e[i];
            ib = vk_transient_copy(e, (uint64_t)icount * 4 + 4, 4);
            free(e);
        }
        max_index += 1;
        if (base_vertex > 0) max_index += (uint32_t)base_vertex;
    }
    if (use_index && ss_need(SS_IB, ss.ib == ib.buf && ss.io == ib.off && ss.it == itype_vk)) {
        ss.ib = ib.buf; ss.io = ib.off; ss.it = itype_vk;
        vkCmdBindIndexBuffer(cb, ib.buf, ib.off, itype_vk);
    }

    // ---- vertex streams: the bound range (once per submission) when small, else what this draw can read
    for (int s = 0; s < 17; s++) {
        if (!(k.used_streams & (1u << s))) continue;
        VkSpan sp;
        if (s == ZERO_BINDING) {
            static const float zero[4] = {0, 0, 0, 1};
            sp = vk_transient_copy(zero, 16, 16);
        } else {
            Range v = st.vbuf[s];
            uint64_t stride = k.stride[s], need;
            if (!stride) need = 256;
            else if (k.divisor[s]) need = ((uint64_t)base_inst + inst + k.divisor[s] - 1) / k.divisor[s] * stride + 256;
            else need = (uint64_t)max_index * stride + 256;
            if (v.size && (need > v.size || v.size <= (4u << 20))) need = v.size;
            VkBuffer cached;
            if (!v.addr || !guest_range_ok(v.addr, need)) { static const uint8_t zero[256]; sp = vk_transient_copy(zero, 256, 16); }
            else if (v.size && guest_range_ok(v.addr, v.size) && vcache(v.addr, v.size, &cached)) sp = (VkSpan){cached, 0, NULL};
            else sp = guest_copy(v.addr, need ? need : 16);
        }
        VkDeviceSize off = sp.off;
        if ((ss.vb_known >> s & 1) && ss.vb[s] == sp.buf && ss.vo[s] == off) continue;
        ss.vb[s] = sp.buf; ss.vo[s] = off; ss.vb_known |= 1u << s;
        vkCmdBindVertexBuffers(cb, (uint32_t)s, 1, &sp.buf, &off);
    }

    if (g_dump_active) {
        int n = snprintf(dump_draw_info, sizeof dump_draw_info, "vs %llx fs %llx vp %.0f,%.0f %.0fx%.0f tex:",
                         (unsigned long long)st.program->stages[0].code_addr, (unsigned long long)st.program->stages[1].code_addr,
                         vp[0], vp[1], vp[2], vp[3]);
        for (int i = 0; i < pr->fi->ntex && n < 3600; i++) {
            NvnTexture *tx = shader_texture_object(1, &pr->fi->tex[i]);
            n += snprintf(dump_draw_info + n, sizeof dump_draw_info - n, " [%d:%s%ux%u f%#x lv%u @%p]", i,
                          tx && tx == st.rt.color[0] ? "SELF " : "", tx ? tx->width : 0, tx ? tx->height : 0, tx ? tx->format : 0,
                          tx ? tx->levels : 0, (void *)tx);
            if (tx && tx->backend && getenv("MHGU_DUMP_VERBOSE")) {  // image mip count, sampler filter / LOD clamp
                ShaderTexture *stx = &pr->fi->tex[i];
                int slot = ((int)stx->handle_offset - 0x20) / 8;
                uint64_t h = (slot >= 0 && slot < 32) ? st.tex[1][slot] : 0;
                uint32_t tsc = (uint32_t)(h >> 20) & 0xFFF;
                NvnSamplerDesc *sd = st.spool && tsc < (uint32_t)st.spool->count ? &((NvnSamplerDesc *)st.spool->entries)[tsc] : NULL;
                n += snprintf(dump_draw_info + n, sizeof dump_draw_info - n, "{img lv %u, min %u lod %g..%g wrap %u,%u aniso %g border %g,%g,%g,%g}",
                              ((TexBackend *)tx->backend)->levels, sd ? sd->min_filter : 99, sd ? sd->lod_min : 0, sd ? sd->lod_max : 0,
                              sd ? sd->wrap[0] : 99, sd ? sd->wrap[1] : 99, sd ? sd->max_aniso : 0, sd ? sd->border[0] : 0,
                              sd ? sd->border[1] : 0, sd ? sd->border[2] : 0, sd ? sd->border[3] : 0);
            }
        }
        if (getenv("MHGU_DUMP_VERBOSE") && n < 3600) {
            n += snprintf(dump_draw_info + n, sizeof dump_draw_info - n,
                          "\n    rt @%p wmask %x blend %x mode %u count %u cull %u a0 fmt %d off %u stride %u",
                          (void *)st.rt.color[0], k.write_mask[0], k.blend_on, mode, count, k.cull, k.attr_fmt[0],
                          k.attr_off[0], k.stride[k.attr_bind[0]]);
            n += snprintf(dump_draw_info + n, sizeof dump_draw_info - n, " | depth %p fmt %d %ux%u test %d write %d op %d stencil %d | ncolor %u fmts %d,%d,%d samples %u a2c %d alphatest %d"
                          " fill %d restart %d ccw %d",
                          (void *)st.rt.depth, pass_depth, pass_w, pass_h, k.depth_test, k.depth_write, k.depth_op, k.stencil_test,
                          k.ncolor, pass_color[0], pass_ncolor > 1 ? pass_color[1] : 0, pass_ncolor > 2 ? pass_color[2] : 0, k.samples,
                          k.a2c, fkey.alpha_func, k.poly_fill, k.prim_restart, k.front_ccw);
            {  // compiler constants (cbuf 1) stored after the fragment code
                NvnShaderStage *fs = &st.program->stages[1];
                if (fs->code_addr && guest_range_ok(fs->code_addr + fs->const_offset, 16)) {
                    const float *f = (const float *)G2H((uint32_t)(fs->code_addr + fs->const_offset));
                    n += snprintf(dump_draw_info + n, sizeof dump_draw_info - n, " cb1@+%x size %x (%g %g %g %g | %g %g %g %g)",
                                  (unsigned)fs->const_offset, (unsigned)fs->const_size, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
                }
            }
            for (int u = 0; u < 3 && n < 3600; u++) {  // the fragment stage's first uniform buffers
                Range ub = st.ubuf[1][u];
                if (!ub.addr || !guest_range_ok(ub.addr, 64)) continue;
                const float *f = (const float *)G2H((uint32_t)ub.addr);
                n += snprintf(dump_draw_info + n, sizeof dump_draw_info - n, " cb%d@%llx(", u + 3, (unsigned long long)ub.addr);
                for (int q = 0; q < 16 && n < 3900; q++) n += snprintf(dump_draw_info + n, sizeof dump_draw_info - n, q ? " %g" : "%g", f[q]);
                n += snprintf(dump_draw_info + n, sizeof dump_draw_info - n, ")");
            }
            Range v = st.vbuf[k.attr_bind[0] < 16 ? k.attr_bind[0] : 0];
            if (v.addr && guest_range_ok(v.addr + k.attr_off[0], 64) && k.attr_fmt[0] == VK_FORMAT_R32G32B32A32_SFLOAT)
                for (uint32_t q = 0; q < 4 && n < 3800; q++) {
                    const float *f = (const float *)G2H((uint32_t)(v.addr + k.attr_off[0] + (uint64_t)(first + q) * k.stride[k.attr_bind[0]]));
                    n += snprintf(dump_draw_info + n, sizeof dump_draw_info - n, " (%.3g %.3g %.3g %.3g)", f[0], f[1], f[2], f[3]);
                }
        }
    }
    if (g_dump_active && getenv("MHGU_DUMP_DRAWS"))  // (debug) every draw's program and depth / stencil use
        LOG("[dump]   cdraw vs %llx fs %llx (code hash %016llx) count %u inst %u mode %u | z test %d write %d op %d stencil %d\n",
            (unsigned long long)st.program->stages[0].code_addr, (unsigned long long)st.program->stages[1].code_addr,
            (unsigned long long)prog_fs_hash(pr), count, inst, mode, k.depth_test, k.depth_write, k.depth_op, k.stencil_test);
    if (g_dump_active && !st.rt.ncolor && getenv("MHGU_DUMP_VERBOSE")) {  // (debug) depth-only passes: every draw
        Range v0 = st.vbuf[k.attr_bind[0] < 16 ? k.attr_bind[0] : 0];
        LOG("[dump]   draw vs %llx vp %.0f,%.0f %.0fx%.0f sc %d,%d %dx%d count %u inst %u vb %llx clamp %d\n",
            (unsigned long long)st.program->stages[0].code_addr, vp[0], vp[1], vp[2], vp[3], st.scissor[0][0], st.scissor[0][1],
            st.scissor[0][2], st.scissor[0][3], count, inst, (unsigned long long)v0.addr, k.depth_clamp);
    }
    {  // (debug) MHGU_CB_WATCH=<fragment code address>: that shader's first fragment uniform buffer, logged every 30 draws
        static uint64_t watch = ~0ull, nwatch;
        if (watch == ~0ull) { const char *e = getenv("MHGU_CB_WATCH"); watch = e ? strtoull(e, NULL, 16) : 0; }
        if (watch && st.program->stages[1].code_addr == watch && nwatch++ % 30 == 0) {
            Range ub = st.ubuf[1][0];
            if (ub.addr && guest_range_ok(ub.addr, 32)) {
                const float *f = (const float *)G2H((uint32_t)ub.addr);
                LOG("[cbwatch] %llx: %g %g %g %g | %g %g %g %g\n", (unsigned long long)ub.addr, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
            }
        }
    }
    if (!use_index) vkCmdDraw(cb, count, inst, first, base_inst);
    else vkCmdDrawIndexed(cb, icount, inst, 0, base_vertex, base_inst);
    stat_draws++;
    if (g_fsr_objvel && g_temporal_on && (pr->fsrcap || capprog_later(pr)) &&
        (k.depth_write || (k.depth_test && pr->vi->cbuf_dynamic)) && inst == 1 && pass_samples == 1) {
        // FSR object motion: the same draw again through the capture variant of its vertex shader (rasterizer
        // discard), which stores each vertex's clip position for the velocity pass (fsr.c)
        FsrCapture fc = {.color0 = st.rt.ncolor ? st.rt.color[0] : NULL, .depth = st.rt.depth, .vs_code = st.program->stages[0].code_addr,
                         .used_streams = k.used_streams, .iaddr = indexed ? r->x[3] : 0, .mode = mode, .count = count,
                         .first = first, .base_vertex = base_vertex, .idx = use_index ? ib.ptr : NULL, .itype = itype_vk,
                         .gidx = indexed && native && r->x[1] != 0 ? G2H((uint32_t)r->x[3]) : NULL,
                         .icount = icount, .attr_mask = k.attr_mask, .topology = k.topology, .restart = k.prim_restart,
                         .rp = pass_rp, .depth_write = k.depth_write, .skinned = pr->vi->cbuf_dynamic != 0,
                         .vubuf = &st.ubuf[0][0].addr};
        for (int s = 0; s < 16; s++) fc.vbuf[s] = (k.used_streams >> s & 1) ? st.vbuf[s].addr : 0;
        memcpy(fc.attr_fmt, k.attr_fmt, sizeof fc.attr_fmt), memcpy(fc.attr_off, k.attr_off, sizeof fc.attr_off);
        memcpy(fc.attr_bind, k.attr_bind, sizeof fc.attr_bind), memcpy(fc.stride, k.stride, sizeof fc.stride);
        memcpy(fc.divisor, k.divisor, sizeof fc.divisor);
        VkPipeline cpipe;
        if (vk_fsr_capture(&fc, pr->fsrcap, &cpipe, &fsr_cap_base)) {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, cpipe);
            ss.known &= SS_IB;  // (a pipeline without dynamic state: the next draw sets it all again)
            VkDescriptorSet s0 = stage_set(pr->fsrcap->nbinds, pr->fsrcap->binds, pr->fsrcap->set0, 0, 0, pr->vi, 0, 0, 0);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pr->fsrcap->layout, 0, 1, &s0, 0, NULL);
            if (!use_index) vkCmdDraw(cb, count, 1, first, base_inst);
            else vkCmdDrawIndexed(cb, icount, 1, 0, base_vertex, base_inst);
        }
    }
}

// ================================================================== texture copies
static void copy_region_dims(NvnTexture *t, const TexRef *ref, const FormatInfo **fi, int32_t rg[6], int *level,
                             uint64_t *row, uint64_t *img) {
    *fi = nvn_format_info(t->format);
    memcpy(rg, ref->region, sizeof ref->region);
    if (!rg[3]) rg[3] = (int32_t)t->width;
    if (!rg[4]) rg[4] = (int32_t)t->height;
    if (!rg[5]) rg[5] = 1;
    *level = ref->has_view && (ref->view.setmask & 1) ? ref->view.base_level : 0;
    if (!*fi || !(*fi)->bpb) return;
    uint32_t wb = ((uint32_t)rg[3] + (*fi)->bw - 1) / (*fi)->bw, hb = ((uint32_t)rg[4] + (*fi)->bh - 1) / (*fi)->bh;
    *row = st.copy_row_stride ? st.copy_row_stride : (uint64_t)wb * (*fi)->bpb;
    *img = st.copy_image_stride ? st.copy_image_stride : *row * hb;
}

static void copy_buffer_to_texture(uint64_t src, const TexRef *dst) {
    snapshots_invalidate(NULL);
    NvnTexture *t = (NvnTexture *)(uintptr_t)dst->tex;
    if (!t || !src) return;
    vk_end_pass();
    st.rt_dirty = true;
    const FormatInfo *fi;
    int32_t rg[6];
    int level;
    uint64_t row = 0, img = 0;
    copy_region_dims(t, dst, &fi, rg, &level, &row, &img);
    if (!fi || !fi->bpb) return;
    if (getenv("MHGU_TEX_LOG")) {
        static int nlog;
        if (nlog++ < 40) LOG("[copy] buffer %llx -> tex fmt %#x %ux%u region %d,%d %dx%d lv %d row %llu img %llu (strides set %llu/%llu)\n",
                             (unsigned long long)src, t->format, t->width, t->height, rg[0], rg[1], rg[3], rg[4], level,
                             (unsigned long long)row, (unsigned long long)img, (unsigned long long)st.copy_row_stride,
                             (unsigned long long)st.copy_image_stride);
    }
    TexBackend *b = vk_tex(t, false);  // prior CPU contents uploaded first
    vk_ms_resolve(b);
    b->ms_valid = false;
    renderer_mark_gpu_owned(t);
    if (b->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) || b->astc_bw) return;
    bool array = b->layers > 1 && b->depth == 1;
    uint32_t hb = ((uint32_t)rg[4] + fi->bh - 1) / fi->bh, wb = ((uint32_t)rg[3] + fi->bw - 1) / fi->bw;
    uint32_t out_bpb = fi->bpb == 3 ? 4 : fi->bpb;
    uint32_t nz = (uint32_t)rg[5];
    if (!guest_range_ok(src, img * nz)) return;
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    uint64_t out_row = (uint64_t)wb * out_bpb;
    VkSpan s = vk_transient(out_row * hb * nz, 16);
    for (uint32_t z = 0; z < nz; z++)
        for (uint32_t y = 0; y < hb; y++) {
            const uint8_t *sr = (const uint8_t *)G2H((uint32_t)(src + (uint64_t)z * img + (uint64_t)y * row));
            uint8_t *dr = (uint8_t *)s.ptr + ((uint64_t)z * hb + y) * out_row;
            if (out_bpb == fi->bpb) memcpy(dr, sr, out_row);
            else for (uint32_t x = 0; x < wb; x++) { memcpy(dr + x * 4, sr + x * 3, 3); dr[x * 4 + 3] = 255; }
        }
    VkBufferImageCopy c = {.bufferOffset = s.off, .bufferRowLength = wb * fi->bw, .bufferImageHeight = hb * fi->bh,
                           .imageSubresource = {b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect,
                                                (uint32_t)level, array ? (uint32_t)rg[2] : 0, array ? nz : 1},
                           .imageOffset = {rg[0], rg[1], array ? 0 : rg[2]},
                           .imageExtent = {(uint32_t)rg[3], (uint32_t)rg[4], array ? 1 : nz}};
    vkCmdCopyBufferToImage(cb, s.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
}

static void copy_texture_to_buffer(const TexRef *src, uint64_t dst) {
    NvnTexture *t = (NvnTexture *)(uintptr_t)src->tex;
    if (!t || !dst) return;
    vk_end_pass();
    st.rt_dirty = true;
    if (t->backend) vk_ms_resolve(t->backend);
    const FormatInfo *fi;
    int32_t rg[6];
    int level;
    uint64_t row = 0, img = 0;
    copy_region_dims(t, src, &fi, rg, &level, &row, &img);
    if (!fi || !fi->bpb || fi->bpb == 3 || row % fi->bpb) return;
    TexBackend *b = vk_tex(t, false);
    bool array = b->layers > 1 && b->depth == 1;
    uint32_t nz = (uint32_t)rg[5];
    uint64_t total = img * nz;
    if (!guest_range_ok(dst, total)) return;
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkSpan out = vk_readback(total);
    VkBufferImageCopy c = {.bufferOffset = out.off, .bufferRowLength = (uint32_t)(row / fi->bpb) * fi->bw,
                           .bufferImageHeight = (uint32_t)(img / row) * fi->bh,
                           .imageSubresource = {b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect,
                                                (uint32_t)level, array ? (uint32_t)rg[2] : 0, array ? nz : 1},
                           .imageOffset = {rg[0], rg[1], array ? 0 : rg[2]},
                           .imageExtent = {(uint32_t)rg[3], (uint32_t)rg[4], array ? 1 : nz}};
    vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
    renderer_submit();  // the CPU may read the result right after the fence
    vk_wait_seq(submit_seq);
    memcpy(G2H((uint32_t)dst), out.ptr, total);
}

static void copy_texture(const TexRef *src, const TexRef *dst, uint64_t flags) {
    snapshots_invalidate(NULL);
    NvnTexture *st_ = (NvnTexture *)(uintptr_t)src->tex, *dt = (NvnTexture *)(uintptr_t)dst->tex;
    if (!st_ || !dt) return;
    if (g_dump_active) LOG("[dump] copy %p -> %p\n", (void *)st_, (void *)dt);
    vk_end_pass();
    st.rt_dirty = true;
    TexBackend *s = vk_tex(st_, false), *d = vk_tex(dt, true);
    vk_ms_resolve(s);
    vk_ms_resolve(d);
    d->ms_valid = false;
    renderer_mark_gpu_owned(dt);
    const int32_t *sr = src->region, *dr = dst->region;
    int sx0 = sr[0], sy0 = sr[1], dx0 = dr[0], dy0 = dr[1];
    int sw = sr[3] ? sr[3] : (int)s->width, sh = sr[4] ? sr[4] : (int)s->height;
    int dw = dr[3] ? dr[3] : (int)d->width, dh = dr[4] ? dr[4] : (int)d->height;
    uint32_t slv = src->has_view && (src->view.setmask & 1) ? src->view.base_level : 0;
    uint32_t dlv = dst->has_view && (dst->view.setmask & 1) ? dst->view.base_level : 0;
    uint32_t sly = src->has_view && (src->view.setmask & 2) ? src->view.min_layer : 0;
    uint32_t dly = dst->has_view && (dst->view.setmask & 2) ? dst->view.min_layer : 0;
    if (slv >= s->levels || dlv >= d->levels || sly >= s->layers || dly >= d->layers) return;
    {   // regions stay inside their levels (out-of-bounds copies / blits are undefined in Vulkan)
        int slw = (int)level_extent(s->width, slv), slh = (int)level_extent(s->height, slv);
        int dlw = (int)level_extent(d->width, dlv), dlh = (int)level_extent(d->height, dlv);
        if (!sr[3]) sw = slw - sx0;
        if (!sr[4]) sh = slh - sy0;
        if (!dr[3]) dw = dlw - dx0;
        if (!dr[4]) dh = dlh - dy0;
        if (sx0 + sw > slw) sw = slw - sx0;
        if (sy0 + sh > slh) sh = slh - sy0;
        if (dx0 + dw > dlw) dw = dlw - dx0;
        if (dy0 + dh > dlh) dh = dlh - dy0;
        if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    }
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkImageAspectFlags sa = s->aspect, da = d->aspect;
    if (s->samples > 1 && d->samples == 1 && sw == dw && sh == dh && s->format == d->format && !(sa & VK_IMAGE_ASPECT_DEPTH_BIT)) {
        VkImageResolve rv = {.srcSubresource = {sa, slv, sly, 1}, .srcOffset = {sx0, sy0, 0}, .dstSubresource = {da, dlv, dly, 1},
                             .dstOffset = {dx0, dy0, 0}, .extent = {(uint32_t)sw, (uint32_t)sh, 1}};
        vkCmdResolveImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rv);
        return;
    }
    if (s->samples != d->samples) return;
    if (sw == dw && sh == dh && s->format == d->format) {
        VkImageCopy c = {.srcSubresource = {sa, slv, sly, 1}, .srcOffset = {sx0, sy0, 0}, .dstSubresource = {da, dlv, dly, 1},
                         .dstOffset = {dx0, dy0, 0}, .extent = {(uint32_t)sw, (uint32_t)sh, 1}};
        vkCmdCopyImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        return;
    }
    bool depth = (sa | da) & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    if (depth && s->format != d->format) return;
    VkImageBlit bl = {.srcSubresource = {depth ? (sa & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : sa) : sa, slv, sly, 1},
                      .srcOffsets = {{sx0, sy0, 0}, {sx0 + sw, sy0 + sh, 1}},
                      .dstSubresource = {depth ? (da & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : da) : da, dlv, dly, 1},
                      .dstOffsets = {{dx0, dy0, 0}, {dx0 + dw, dy0 + dh, 1}}};
    bool linear = (flags & 1) && !depth && !rt_kind(st_->format);
    vkCmdBlitImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
}

// ================================================================== presentation helper (device.c)
VkRenderPass vk_window_pass(void);
static VkPipelineLayout present_layout;
static VkDescriptorSetLayout present_set_layout;

static VkPipeline present_pipeline(void) {
    static VkPipeline p;
    static VkRenderPass made_for;
    VkRenderPass rp = vk_window_pass();
    if (p && made_for == rp) return p;
    if (!present_layout) {
        VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &present_set_layout));
        VkPushConstantRange r = {VK_SHADER_STAGE_VERTEX_BIT, 0, 32};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                         .pSetLayouts = &present_set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &present_layout));
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("present.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader("present.frag"), .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1,
                                              .pAttachments = &cba};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = present_layout, .renderPass = rp};
    VK_CHECK(vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p));
    made_for = rp;
    return p;
}

// Scanout shows the bytes the game wrote: sRGB images are read through a UNORM view (no decode).
static VkImageView raw_view(TexBackend *b) {
    VkFormat f = b->format == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM
               : b->format == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_B8G8R8A8_UNORM : b->format;
    static const VkComponentMapping id = {0};
    return vk_image_view(b, VK_IMAGE_VIEW_TYPE_2D, f, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
}

// Inside the window image's render pass (device.c): the game image into (vx, vy, vw, vh).
void vk_present_draw(VkCommandBuffer cb, TexBackend *b, float vx, float vy, float vw, float vh) {
    if (!b || !b->image || (b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)) return;
    VkPipeline p = present_pipeline();
    VkDescriptorSet ds = vk_alloc_set(present_set_layout);
    VkDescriptorImageInfo ii = {vk_default_sampler(true), raw_view(b), VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
                              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    vkUpdateDescriptorSets(g_vk.dev, 1, &w, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, present_layout, 0, 1, &ds, 0, NULL);
    VkViewport vp = {vx, vy, vw, vh, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &vp);
    float pv[8] = {nvn_window_origin() == 0 ? 1.0f : 0.0f, 0, 0, 0, 0, 0, 1, 1};  // (flip, crop origin / size)
    vkCmdPushConstants(cb, present_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 32, pv);
    vkCmdDraw(cb, 3, 1, 0, 0);
}

// ================================================================== executor
void nvn_sync_set_seq(uint64_t guest, uint64_t seq);

static void exec_list(uint64_t handle, int depth) {
    uint64_t p = handle;
    for (int guard = 0; guard < 10000000; guard++) {
        CmdRec *r = (CmdRec *)G2H((uint32_t)p);
        void *x = r->extra;
        cur_rec = p;
        switch (r->op) {
        case CMD_END: return;
        case CMD_JUMP: p = r->x[0]; continue;
        case CMD_CALL: {
            const uint64_t *h = x;
            for (uint64_t i = 0; i < r->x[0] && depth < 8; i++) exec_list(h[i], depth + 1);
            break;
        }
        case CMD_BIND_PROGRAM: st.program = (NvnProgram *)(uintptr_t)r->x[0]; break;
        case CMD_BIND_BLEND: { uint8_t *b = x; memcpy(st.blend[b[0] & 7], b, 8); break; }
        case CMD_BIND_CHANNEL_MASK: memcpy(&st.channel_mask, x, 4); break;
        case CMD_BIND_COLOR: memcpy(st.color_state, x, 4); break;
        case CMD_BIND_DEPTH_STENCIL: memcpy(st.ds, x, 8); break;
        case CMD_BIND_MULTISAMPLE: memcpy(st.ms, x, 0x18); break;
        case CMD_BIND_POLYGON: memcpy(st.poly, x, 4); break;
        case CMD_BIND_VERTEX_ATTRIB:
            st.nattrib = (int)r->x[0] > 16 ? 16 : (int)r->x[0];
            memcpy(st.attrib, x, st.nattrib * 4);
            break;
        case CMD_BIND_VERTEX_STREAM:
            st.nstream = (int)r->x[0] > 16 ? 16 : (int)r->x[0];
            memcpy(st.stream, x, st.nstream * 8);
            break;
        case CMD_BIND_VERTEX_BUFFERS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(&st.vbuf[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_BIND_UNIFORM_BUFFERS:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 18 && r->x[0] < 6; i++)
                memcpy(&st.ubuf[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_BIND_STORAGE_BUFFERS:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 16 && r->x[0] < 6; i++)
                memcpy(&st.sbuf[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_BIND_TEXTURES:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 32 && r->x[0] < 6; i++)
                memcpy(&st.tex[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 8, 8);
            break;
        case CMD_BIND_IMAGES:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 8 && r->x[0] < 6; i++)
                memcpy(&st.img[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 8, 8);
            break;
        case CMD_SET_TEXTURE_POOL: st.tpool = (NvnDescPool *)(uintptr_t)r->x[0]; break;
        case CMD_SET_SAMPLER_POOL: st.spool = (NvnDescPool *)(uintptr_t)r->x[0]; break;
        case CMD_SET_RENDER_TARGETS: {
            const RtRec *rt = x;
            RtState prev = st.rt;  // the same targets again: keep the pass
            memset(&st.rt, 0, sizeof st.rt);
            st.rt.ncolor = (int)r->x[0] > 8 ? 8 : (int)r->x[0];
            for (int i = 0; i < st.rt.ncolor; i++) {
                st.rt.color[i] = (NvnTexture *)(uintptr_t)rt->tex[i];
                st.rt.cview[i] = rt->views[i];
                st.rt.has_cview[i] = rt->has_view[i];
            }
            st.rt.depth = (NvnTexture *)(uintptr_t)rt->depth;
            st.rt.dview = rt->dview;
            st.rt.has_dview = rt->has_dview;
            if (memcmp(&prev, &st.rt, sizeof prev)) st.rt_dirty = true;
            break;
        }
        case CMD_SET_VIEWPORTS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(st.viewport[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_SET_SCISSORS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(st.scissor[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_SET_DEPTH_RANGES:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(st.depth_range[r->x[0] + i], (uint8_t *)x + i * 8, 8);
            break;
        case CMD_SET_BLEND_COLOR: memcpy(st.blend_color, x, 16); break;
        case CMD_SET_STENCIL: {
            int face = (int)r->x[0];
            for (int f = 0; f < 2; f++) {
                if (!(face & (1 << f))) continue;
                if (r->x[6] == 0) st.stencil_ref[f] = (uint32_t)r->x[1];
                else if (r->x[6] == 1) st.stencil_mask[f] = (uint32_t)r->x[1];
                else st.stencil_vmask[f] = (uint32_t)r->x[1];
            }
            break;
        }
        case CMD_SET_POLYGON_OFFSET: memcpy(st.poly_offset, r->f, sizeof st.poly_offset); break;
        case CMD_SET_ALPHA_REF: st.alpha_ref = r->f[0]; break;
        case CMD_SET_PRIMITIVE_RESTART: st.prim_restart = r->x[0] != 0; st.restart_index = (uint32_t)r->x[1]; break;
        case CMD_SET_DEPTH_CLAMP:
            st.depth_clamp = r->x[0] != 0;
            if (getenv("MHGU_DUMP_VERBOSE")) {
                static int n, last = -1;
                if (st.depth_clamp != last && n++ < 20) LOG("[vk] depth clamp -> %d\n", st.depth_clamp);
                last = st.depth_clamp;
            }
            break;
        case CMD_SET_COPY_STRIDES: st.copy_row_stride = r->x[0]; st.copy_image_stride = r->x[1]; break;
        case CMD_CLEAR_COLOR: {
            snapshots_invalidate(NULL);
            int index = (int)r->x[0];
            if (index < st.rt.ncolor && st.rt.color[index]) clear(index, (int)r->x[5], x, (uint32_t)r->x[2], false, 0, false, 0, 0);
            break;
        }
        case CMD_CLEAR_TEXTURE: {  // a render-target clear of the texture alone
            const ClearTexRec *ct = x;
            NvnTexture *t = (NvnTexture *)(uintptr_t)ct->tex;
            if (!t) break;
            snapshots_invalidate(NULL);
            RtState saved = st.rt;
            int32_t sc[4];
            memcpy(sc, st.scissor[0], sizeof sc);
            vk_end_pass();
            memset(&st.rt, 0, sizeof st.rt);
            st.rt.ncolor = 1;
            st.rt.color[0] = t;
            if (ct->has_view) { st.rt.cview[0] = ct->view; st.rt.has_cview[0] = 1; }
            st.rt_dirty = true;
            const int32_t *rg = ct->region;
            if (rg[3] > 0 && rg[4] > 0) { st.scissor[0][0] = rg[0]; st.scissor[0][1] = rg[1]; st.scissor[0][2] = rg[3]; st.scissor[0][3] = rg[4]; }
            else { st.scissor[0][0] = st.scissor[0][1] = 0; st.scissor[0][2] = st.scissor[0][3] = 16384; }
            clear(0, (int)r->x[6], ct->color, (uint32_t)r->x[4] ? (uint32_t)r->x[4] : 0xF, false, 0, false, 0, 0);
            vk_end_pass();
            st.rt = saved;
            memcpy(st.scissor[0], sc, sizeof sc);
            st.rt_dirty = true;
            break;
        }
        case CMD_CLEAR_DEPTH_STENCIL:
            snapshots_invalidate(NULL);
            if (st.rt.depth) {
                bool stencil = renderer_format_has_stencil(st.rt.depth->format);
                clear(-1, 0, NULL, 0, r->x[0] != 0, r->f[0], stencil && r->x[2] != 0, (uint32_t)r->x[1], (uint32_t)r->x[2]);
            }
            break;
        case CMD_CLEAR_BUFFER: {  // (address, size, 32-bit value): CPU-side, in order with the list
            uint64_t a = r->x[0], n = r->x[1] & ~3ull;
            uint32_t v = (uint32_t)r->x[2];
            if (guest_range_ok(a, n)) for (uint64_t i = 0; i < n; i += 4) ST32((uint32_t)(a + i), v);
            gcopy_gen++;
            break;
        }
        case CMD_DISPATCH: dispatch_compute((uint32_t)r->x[0], (uint32_t)r->x[1], (uint32_t)r->x[2]); break;
        case CMD_DRAW_ARRAYS: draw(r, false); break;
        case CMD_DRAW_ELEMENTS: draw(r, true); break;
        case CMD_UPDATE_UNIFORM_BUFFER:
            if (guest_range_ok(r->x[0] + r->x[2], r->x[3])) memcpy(G2H((uint32_t)(r->x[0] + r->x[2])), x, r->x[3]);
            gcopy_gen++;  // (copies shared within the submission are stale now)
            break;
        case CMD_FENCE_SYNC:
            if (r->x[1]) nvn_sync_set_seq(r->x[1], renderer_fence_seq());
            break;
        case CMD_REPORT_COUNTER: {  // (type, address): {0, timestamp in GPU ticks (614.4 MHz)}
            {
                static uint32_t seen;
                uint32_t ty = (uint32_t)r->x[0];
                if (ty < 32 && !(seen & (1u << ty))) { seen |= 1u << ty; LOG("[nvn] ReportCounter type %u\n", ty); }
            }
            if (!r->x[1] || !guest_range_ok(r->x[1], 16)) break;
            uint64_t now = plat_now_ns();
            uint64_t ticks = (uint64_t)((unsigned __int128)now * 384 / 625);
            ST64((uint32_t)r->x[1], 0);
            ST64((uint32_t)r->x[1] + 8, ticks);
            break;
        }
        case CMD_COPY_TEXTURE_TO_TEXTURE: { const TexRef *t = x; copy_texture(&t[0], &t[1], r->x[6]); break; }
        case CMD_DOWNSAMPLE: { const TexRef *t = x; copy_texture(&t[0], &t[1], 1); break; }
        case CMD_COPY_BUFFER_TO_TEXTURE: copy_buffer_to_texture(r->x[0], x); break;
        case CMD_COPY_TEXTURE_TO_BUFFER: copy_texture_to_buffer(x, r->x[3]); break;
        case CMD_COPY_BUFFER_TO_BUFFER:  // (src, dst, size): guest memory only the CPU writes here
            if (guest_range_ok(r->x[0], r->x[2]) && guest_range_ok(r->x[1], r->x[2]))
                memmove(G2H((uint32_t)r->x[1]), G2H((uint32_t)r->x[0]), r->x[2]);
            gcopy_gen++;
            break;
        default: break;
        }
        p += r->size;
    }
}

void backend_execute(uint64_t handle) {
    vk_lock();
    plist_drain();  // (the pipeline list's programs the prewarm met)
    uint64_t t0 = plat_now_ns();
    exec_list(handle, 0);
    stat_exec_ns += plat_now_ns() - t0;
    renderer_submit();
    vk_unlock();
    static int stats = -1;
    if (stats < 0) stats = getenv("MHGU_VK_STATS") != NULL;
    static uint64_t last, frame, frame_t;
    uint64_t now = plat_now_ns();
    if (vk_frame_counter() != frame) {  // (the first submission of each frame: the longest gap is a stutter)
        if (frame_t && now - frame_t > stat_frame_max) stat_frame_max = now - frame_t;
        frame = vk_frame_counter(), frame_t = now;
    }
    if (stats && now - last > 2000000000ull) {
        last = now;
        LOG("[vk] 2s: vcache %llu hits %llu copies | %llu draws, %llu skipped, %llu passes, %llu dispatches, %llu snapshots, %d compiles | exec %.0f ms (uploads %.0f) gpu wait %.0f ms, copied %.1f MB\n",
            (unsigned long long)stat_vc_hits, (unsigned long long)stat_vc_refresh,
            (unsigned long long)stat_draws, (unsigned long long)stat_skipped, (unsigned long long)stat_passes,
            (unsigned long long)stat_dispatches, (unsigned long long)stat_snapshots, atomic_exchange(&stat_compiles, 0),
            stat_exec_ns / 1e6, stat_upload_ns / 1e6, stat_wait_ns / 1e6, stat_copy_bytes / 1048576.0);
        if (stat_skipped || stat_waits || stat_prog_cached || atomic_load(&stat_job_n[0]) || atomic_load(&stat_job_n[2])) {
            double ms[3], mx[3];
            uint64_t n[3];
            for (int i = 0; i < 3; i++) {
                n[i] = atomic_exchange(&stat_job_n[i], 0);
                ms[i] = n[i] ? atomic_exchange(&stat_job_ns[i], 0) / 1e6 / n[i] : 0;
                mx[i] = atomic_exchange(&stat_job_max[i], 0) / 1e6;
            }
            uint64_t ln = atomic_exchange(&stat_lat_n, 0);
            double lat = ln ? atomic_exchange(&stat_lat_ns, 0) / 1e6 / ln : 0, lmax = atomic_exchange(&stat_lat_max, 0) / 1e6;
            LOG("[vk] 2s builds: %llu programs from the cache | skipped %llu (program %llu, pipeline %llu) in %llu frames | waited %.0f ms in %llu waits, %llu "
                "ready in time | translate %llu x %.1f ms (max %.0f), spirv %llu x %.1f (max %.0f), pipeline %llu x %.1f "
                "(max %.0f) | needed -> ready %.0f ms avg, %.0f max | slowest frame %.1f ms | prewarm %llu new, %llu cached, %llu queued\n",
                (unsigned long long)stat_prog_cached, (unsigned long long)stat_skipped, (unsigned long long)stat_skip_prog,
                (unsigned long long)stat_skip_pipe, (unsigned long long)stat_skip_frames, stat_waited_ns / 1e6, (unsigned long long)stat_waits,
                (unsigned long long)stat_rescued, (unsigned long long)n[0], ms[0], mx[0], (unsigned long long)n[1], ms[1],
                mx[1], (unsigned long long)n[2], ms[2], mx[2], lat, lmax, stat_frame_max / 1e6,
                (unsigned long long)atomic_exchange(&stat_pw_new, 0), (unsigned long long)atomic_exchange(&stat_pw_known, 0),
                (unsigned long long)atomic_load(&stat_pw_left));
        }
        stat_prog_cached = stat_skip_prog = stat_skip_pipe = stat_skip_frames = stat_waited_ns = stat_waits = stat_rescued = stat_frame_max = 0;
        stat_draws = stat_skipped = stat_passes = stat_dispatches = stat_snapshots = 0;
        stat_exec_ns = stat_wait_ns = stat_copy_bytes = stat_upload_ns = stat_vc_hits = stat_vc_refresh = 0;
    }
}
