// The game's texture / sampler descriptor slots, taken and given back under one lock.
//
// MT Framework hands out NVN texture pool slots from a bitmap (the object at device +0x188 +0x34100: +4 the first
// texture id, +0xc 256 words of texture bits, +0x40c sampler bits, +0x50c / +0x510 the word the last search ended
// in). The texture allocator 0xa6c41c starts at the word after the last one, takes its lowest free bit and registers
// the texture there (nvnTexturePoolRegisterTexture, the `blx r6` at 0xa6c4ec); 0xa6c3ac clears a bit again. Neither
// takes a lock: each is a plain load / modify / store of a bitmap word. Textures are made and freed on more than one
// thread (the render thread's targets, the loader's resources), so two of them in the same word at once can lose a
// bit: a slot taken while another thread frees its neighbour reads as free again while its texture still uses it.
// The cursor comes back to that word some 256 allocations later and gives the slot to a new texture, whose
// descriptor then replaces the old one's: whatever drew the old texture (a post effect's render target, made once
// and kept for the whole session) now draws the new one. That is the rare "a scene texture half-transparent over
// the whole screen" (also on the Switch in MHXX, on the same engine: an eye texture sheet over the screen at quest
// start), seen after hours of play and at quest loads, when the most textures are made and freed.
//
// Here the four functions run under one host mutex. Two threads meeting there are logged (the race this closes).
// MHGU_TEXSLOT_LOCK=0 leaves them as they are (to compare).
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../kernel.h"
#include "../nvn/nvn.h"

#define FN_TEX_ALLOC  0xa6c41cu  // r0 the slot object, r1 the NVNtexture: returns the slot (0: none free)
#define FN_SMP_ALLOC  0xa6c4fcu  // the same for samplers
#define FN_TEX_FREE   0xa6c3acu  // r0 the slot object, r1 the slot
#define FN_SMP_FREE   0xa6c3e4u

static pthread_mutex_t g_slot_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_keep = true;   // keep_live_slots (MHGU_TEXSLOT_KEEP=0: off, to compare)
static bool g_free_stats;    // (debug) MHGU_TEXSLOT_STATS=1
static _Atomic uint32_t g_inside_fn, g_inside_thread;  // who holds it (for the log)
static atomic_int g_met;                               // times two threads met here

static const char *fn_name(uint32_t fn) {
    return fn == FN_TEX_ALLOC ? "texture slot taken" : fn == FN_SMP_ALLOC ? "sampler slot taken"
         : fn == FN_TEX_FREE ? "texture slot freed" : "sampler slot freed";
}

#define SLOT_POOL     0x540u     // (the slot object's NVNtexturePool)
#define SLOT_FIRST    0x4u       // the first texture slot
#define SLOT_TEXBITS  0xcu       // u32[256]: a bit per texture slot, set = taken
#define SLOT_TEXLAST  0x50cu     // the word the last texture search ended in

NvnTexture *nvn_texture_slot(uint32_t guest_pool, int id);  // (nvn/objects.c)

// Before the game's texture search runs (under the lock, so it finds what this finds): the slot it is about to
// take, if a living texture is still registered there, is a bit the bitmap lost: the bit is set again (the slot
// stays its texture's) and the search looked at once more. Whatever lost it (two threads, or a slot freed twice),
// a texture in use never loses its slot to a new one.
static void keep_live_slots(Cpu *c) {
    uint32_t obj = c->r[0], first = LD32(obj + SLOT_FIRST), cur = LD32(obj + SLOT_TEXLAST) & 255;
    for (int guard = 0; guard < 64; guard++) {
        uint32_t w = (cur + 1) & 255;
        while (w != cur && LD32(obj + SLOT_TEXBITS + 4 * w) == 0xffffffffu) w = (w + 1) & 255;
        if (w == cur) return;  // (none free: the game's own answer)
        uint32_t word = LD32(obj + SLOT_TEXBITS + 4 * w), bit = (uint32_t)__builtin_ctz(~word);
        int slot = (int)(first + w * 32 + bit);
        NvnTexture *t = nvn_texture_slot(obj + SLOT_POOL, slot);
        if (!t) return;
        static int nlogged;
        if (nlogged++ < 8)
            LOG("[texslots] slot %d shows free but its %ux%u texture (format %#x) is still registered: kept it, the new "
                "texture gets another slot\n", slot, t->width, t->height, t->format);
        ST32(obj + SLOT_TEXBITS + 4 * w, word | 1u << bit);
    }
}

static void run_locked(Cpu *c, uint32_t fn) {
    uint32_t me = kernel_current_thread_handle();
    {  // each thread's first visit, logged (which threads make and free textures)
        static _Atomic uint32_t seen[16];
        bool known = false;
        for (int i = 0; i < 16 && !known; i++) {
            uint32_t s = atomic_load(&seen[i]);
            if (s == me) known = true;
            else if (!s) {
                uint32_t zero = 0;
                if (atomic_compare_exchange_strong(&seen[i], &zero, me)) {
                    LOG("[texslots] thread %x uses the game's descriptor slots (first: %s, lr %08x)\n", me, fn_name(fn), c->r[14]);
                    known = true;
                } else if (zero == me) known = true;
            }
        }
    }
    if (pthread_mutex_trylock(&g_slot_lock) != 0) {
        uint32_t other_fn = atomic_load(&g_inside_fn), other = atomic_load(&g_inside_thread);
        int n = atomic_fetch_add(&g_met, 1) + 1;
        if (n <= 8 || (n & (n - 1)) == 0)  // (the first few, then 16, 32, 64...)
            LOG("[texslots] two threads at the game's descriptor slots at once (%d so far): thread %x (%s, r1 %08x) "
                "waits for thread %x (%s)\n", n, me, fn_name(fn), c->r[1], other, other_fn ? fn_name(other_fn) : "?");
        pthread_mutex_lock(&g_slot_lock);
    }
    atomic_store(&g_inside_fn, fn);
    atomic_store(&g_inside_thread, me);
    if (fn == FN_TEX_ALLOC && g_keep) keep_live_slots(c);
    if (fn == FN_TEX_FREE && c->r[1] && g_free_stats) {  // (debug) is the texture still registered when its slot is freed?
        static int live, dead;
        NvnTexture *t = nvn_texture_slot(c->r[0] + SLOT_POOL, (int)c->r[1]);
        if (t) live++; else dead++;
        if ((live + dead) % 200 == 0) LOG("[texslots] slots freed: %d with their texture registered, %d after it was finalized\n", live, dead);
    }
    fnhook_call_original(c, fn);
    atomic_store(&g_inside_fn, 0);
    pthread_mutex_unlock(&g_slot_lock);
}

// (debug) MHGU_TEXSLOT_DROP=<slot>@<n>: at the n-th texture slot taken, that slot's bit is cleared while its texture
// lives on: what the lost update above does, on demand (the next sweep of the cursor gives the slot away again).
static int g_drop_slot = -1, g_drop_at;
static int g_allocs;
static void tex_alloc(Cpu *c) {
    if (g_drop_slot >= 0 && ++g_allocs == g_drop_at) {
        uint32_t obj = c->r[0], first = LD32(obj + 4), rel = (uint32_t)g_drop_slot - first;
        uint32_t wa = obj + 0xc + (rel >> 5) * 4;
        LOG("[texslots] test: slot %d's bit cleared at slot %d taken (word %u was %08x)\n", g_drop_slot, g_allocs, rel >> 5, LD32(wa));
        ST32(wa, LD32(wa) & ~(1u << (rel & 31)));
    }
    run_locked(c, FN_TEX_ALLOC);
}
static void smp_alloc(Cpu *c) { run_locked(c, FN_SMP_ALLOC); }
static void tex_free(Cpu *c) { run_locked(c, FN_TEX_FREE); }
static void smp_free(Cpu *c) { run_locked(c, FN_SMP_FREE); }

void texslots_init(void) {
    const char *d = getenv("MHGU_TEXSLOT_DROP");
    if (d && sscanf(d, "%d@%d", &g_drop_slot, &g_drop_at) != 2) g_drop_slot = -1;
    const char *k = getenv("MHGU_TEXSLOT_KEEP");
    g_keep = !(k && !strcmp(k, "0"));
    g_free_stats = getenv("MHGU_TEXSLOT_STATS") != NULL;
    const char *e = getenv("MHGU_TEXSLOT_LOCK");
    if (e && !strcmp(e, "0")) { LOG("[texslots] the game's descriptor slots without a lock (MHGU_TEXSLOT_LOCK=0)\n"); return; }
    fnhook_set(FN_TEX_ALLOC, hle_register("texslots:tex_alloc", tex_alloc));
    fnhook_set(FN_SMP_ALLOC, hle_register("texslots:smp_alloc", smp_alloc));
    fnhook_set(FN_TEX_FREE, hle_register("texslots:tex_free", tex_free));
    fnhook_set(FN_SMP_FREE, hle_register("texslots:smp_free", smp_free));
}
