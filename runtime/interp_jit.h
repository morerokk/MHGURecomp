// JIT for interpreted code (x86-64 and AArch64 hosts), included at the end of runtime/interp.c, whose decoder, block
// rules and run loop it shares. Guest code that was not recompiled ahead of time (a code mod's subsdk, Forge plugins,
// game functions a mod or cheat patched) runs through interp_run; with the JIT each block the interpreter would decode
// (block_build's rules: up to an instruction that may branch, the end of the interpreted range, or JIT_MAX) becomes
// host code instead, and the run loop (jit_run, interp_run_inner's logic) enters it. This file is the part both hosts
// share (arena, blocks, the run loop); interp_jit_x64.h and interp_jit_a64.h emit the code, each with the same
// translations: jit_header (the arena's enter / exit / bad-access thunks), jit_emit_block, jit_patch_site (chaining an
// exit), jit_redirect (a stale block on to its new one) and jit_code_written (the instruction cache).
//
// - State stays in the Cpu: every guest register, flag (0 / 1 words, as the lifted code keeps them) and the rest are
//   read and written there by each instruction, so anything the JIT does not translate runs the interpreter's own op
//   for it (a call from the block: VFP, NEON, SVC, exclusives, MRS / MSR ...) on the same state.
// - Translated: data processing (every shift form; register-specified shifts unless their carry is needed), MOVW /
//   MOVT, LDR / STR / LDRB / STRB / LDRH / STRH / LDRSB / LDRSH / LDRD / STRD in every addressing mode, LDM / STM,
//   B / BL / BX / BLX, MUL / MLA / MLS / UMULL / SMULL / UMLAL / SMLAL, CLZ, SXTB / SXTH / UXTB / UXTH (+ add), REV,
//   UBFX / SBFX / BFI / BFC, conditional execution.
// - A block leaves with the next pc (eax / w0): a direct branch's exit is patched into a jump to the target's block once
//   the run loop has found it interpreted (chaining), after checks that keep the run loop's rules: the run's return
//   address ends the run (Cpu field JO_RET), and a backward jump goes back to the loop when the code generation moved
//   (a hook set: g_code_gen). Indirect branches always go back to the loop.
// - Changed code: each block compares its guest words with the ones it was made from on entry (as the interpreter
//   compares each op's) and is rebuilt when one differs.
// - Bad accesses (interp.c gchk): the same checks, except for sp-based ones, with g_ip_pc set first; g_ip_last is
//   the block's first instruction.
// - Blocks live in a per-thread arena (no locks); a full arena, or a new code generation, starts it over, but only
//   while no block of the thread is running (in_jit 0); until then a nested run falls back to the interpreter.
// MHGU_JIT=0: the interpreter alone. tools/test_lift.py --jit checks the translations against Unicorn (x86-64);
// tools/test_jit.py checks the JIT against the interpreter (both hosts; --a64 under qemu-aarch64).
// (Apple Silicon: the arena is MAP_JIT memory, which a thread writes or runs but never both: jit_write_begin /
// jit_write_end switch the thread to writing around everything that changes the arena. Not on Intel Macs.)
#if (defined(__x86_64__) || defined(__aarch64__)) && (!defined(__APPLE__) || defined(__aarch64__)) && !defined(MHGU_NO_JIT)
#define HAVE_JIT 1
#include <pthread.h>
#include <stddef.h>

#if defined(__APPLE__) && defined(__aarch64__)
static inline void jit_write_begin(void) { pthread_jit_write_protect_np(0); }
static inline void jit_write_end(void) { pthread_jit_write_protect_np(1); }
#else
static inline void jit_write_begin(void) {}
static inline void jit_write_end(void) {}
#endif

void *jit_mem_alloc(size_t n);
int jit_mem_register(void *base, void *rf);
void jit_mem_free(void *base, size_t n, void *rf);

// ---- the JIT's fields in Cpu._pad
#define JO_RET   128  // the run's return address (a direct branch to it goes back to the loop)
#define JO_LINK  132  // set by an exit through a call (BL / BLX)
#define JO_SITE  136  // the exit's patchable jump (offset in the arena; 0: none); JO_EXIT 1: the stale block's entry
#define JO_RETPC 140  // a call's return address (the instruction after it)
#define JO_EXIT  144  // 1: the block at eax changed and must be rebuilt
#define JO_GEN   148  // g_code_gen the arena's blocks were made for
#define JO_INSNS 152  // (u64) instructions run (MHGU_INTERP_STATS)
#define JO_TLINK 160  // link out of an interpreter op the block called
#define JO_END   164
_Static_assert(offsetof(Cpu, _pad) == 128 && sizeof(((Cpu *)0)->_pad) >= JO_END - 128, "jit fields");
#define JF(c, o) (*(uint32_t *)((uint8_t *)(c) + (o)))

#define JIT_ARENA (8u << 20)
#define JIT_SLOTS 8192
#define JIT_MAX 48
#define JIT_GUARD (64u << 10)  // room a block may need (code and its ops), at most

typedef uint32_t (*JitEnter)(Cpu *c, void *code, void *const *ptrs);
typedef struct JitTls {
    uint8_t *base, *code0, *cur, *dcur;  // arena; first block; code grows up from cur, op copies down from dcur
    JitEnter enter;
    uint8_t *exit;                       // common exit (edx / w1: the site)
    uint8_t *thunk[4][2];                // bad-access checks' calls, by log2(size) and write
    void *ptrs[3];                       // &g_ip_pc, &g_ip_last, &g_code_gen (r14 r13 r12 / x21 x22 x23 in blocks)
    void *rf;                            // the function table entry (Windows)
    uint32_t gen, epoch;
    int in_jit;                          // blocks of this thread running now (calls out of them)
    struct { uint32_t pc, end; uint8_t *code; } slot[JIT_SLOTS];
} JitTls;
static __thread JitTls *t_jit;
static __thread int t_jit_failed;
static int jit_mode = -1;  // 0 off, 1 on
// (debug) MHGU_JIT_DUMP=<file>: every block made, as {u64 host address, u32 guest pc, u32 instructions, u32 bytes,
// the code} records, for a disassembler (with a sampling profile: where JIT code spends its time)
static FILE *jit_dump;
static pthread_mutex_t jit_dump_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_key_t jit_key;
static pthread_once_t jit_once = PTHREAD_ONCE_INIT;

// ================================================================ code buffer
typedef struct { uint8_t *p, *end; int bad; } Em;
static void b1(Em *e, uint32_t v) {
    if (e->p < e->end) *e->p++ = (uint8_t)v;
    else e->bad = 1;
}
static void b4(Em *e, uint32_t v) { b1(e, v), b1(e, v >> 8), b1(e, v >> 16), b1(e, v >> 24); }
static void b8(Em *e, uint64_t v) { b4(e, (uint32_t)v), b4(e, (uint32_t)(v >> 32)); }

typedef struct {
    Em e;
    JitTls *t;
    uint32_t pc0;
    Op *ops;          // op copies for the calls into the interpreter (below t->dcur)
    int nops, maxops;
    int test;         // (tools/test_lift.py --jit) one instruction, no checks
    struct { uint8_t *at; uint64_t v; int wide; } lit[2 * JIT_MAX + 8];  // (AArch64) literals the block loads
    int nlit;
} J;

#if defined(__x86_64__)
#define JIT_ARCH "x86-64"
#include "interp_jit_x64.h"
#else
#define JIT_ARCH "AArch64"
#include "interp_jit_a64.h"
#endif

// ================================================================ arena
static void jit_flush(JitTls *t) {
    t->cur = t->code0;
    t->dcur = t->base + JIT_ARENA;
    memset(t->slot, 0, sizeof t->slot);
    t->epoch++;
}

// The arena's fixed part: unwind data, enter, exit, the bad-access thunks
static void jit_free(void *p) {
    JitTls *t = p;
    if (!t) return;
    jit_mem_free(t->base, JIT_ARENA, t->rf);
    free(t);
}
static void jit_key_init(void) { pthread_key_create(&jit_key, jit_free); }

static JitTls *jit_tls(void) {
    if (t_jit) return t_jit;
    if (t_jit_failed) return NULL;
    pthread_once(&jit_once, jit_key_init);
    JitTls *t = calloc(1, sizeof *t);
    if (t) t->base = jit_mem_alloc(JIT_ARENA);
    int made = 0;
    if (t && t->base) {
        jit_write_begin();
        made = jit_header(t);
        jit_write_end();
    }
    if (!made) {
        if (t && t->base) jit_mem_free(t->base, JIT_ARENA, NULL);
        free(t);
        t_jit_failed = 1;
        LOG("[interp] JIT: no executable memory for this thread: the interpreter runs its code\n");
        return NULL;
    }
    t->ptrs[0] = (void *)&g_ip_pc;
    t->ptrs[1] = (void *)&g_ip_last;
    t->ptrs[2] = (void *)&g_code_gen;
    t->gen = __atomic_load_n(&g_code_gen, __ATOMIC_ACQUIRE);
    jit_flush(t);
    pthread_setspecific(jit_key, t);
    return t_jit = t;
}

// The block at pc (stop: an address it must not run into, the run's return address; 0 none). NULL: no room now (a
// block of this thread is running) or a test word that did not fit.
static uint8_t *jit_build_in(JitTls *t, uint32_t pc, uint32_t stop, int test, uint32_t tw) {
    for (int tries = 0; tries < 2; tries++) {
        if ((size_t)(t->dcur - t->cur) < JIT_GUARD) {
            if (t->in_jit) return NULL;
            jit_flush(t);
        }
        Op ops[JIT_MAX];
        uint32_t mem[JIT_MAX], n = 0;
        int ended = 0;
        if (test) {
            bool end;
            decode_op(&ops[0], tw, &end);
            mem[0] = tw, n = 1;
        } else {
            for (uint32_t a = pc;;) {
                bool end;
                decode_op(&ops[n], code_word(a), &end);
                mem[n++] = *(const uint32_t *)G2H(a);
                a += 4;
                if (end) ended = 1;
                if (end || n == JIT_MAX || !interp_owns(a) || a == stop) break;
            }
        }
        (void)ended;
        J j = {.e = {t->cur, t->dcur - n * sizeof(Op) - 256, 0}, .t = t, .pc0 = pc, .ops = (Op *)t->dcur,
               .maxops = (int)n, .test = test};
        Em *e = &j.e;
        uint8_t *entry = e->p;
        jit_emit_block(&j, ops, mem, n);
        if (e->bad) {
            if (t->in_jit) return NULL;
            jit_flush(t);
            continue;
        }
        if (jit_dump && !test) {
            pthread_mutex_lock(&jit_dump_lock);
            uint64_t h = (uint64_t)(uintptr_t)entry;
            uint32_t hd[3] = {pc, n, (uint32_t)(e->p - entry)};
            fwrite(&h, 8, 1, jit_dump), fwrite(hd, 4, 3, jit_dump), fwrite(entry, 1, hd[2], jit_dump);
            fflush(jit_dump);
            pthread_mutex_unlock(&jit_dump_lock);
        }
        jit_code_written(entry, (size_t)(e->p - entry));
        t->cur = (uint8_t *)(((uintptr_t)e->p + 15) & ~(uintptr_t)15);
        t->dcur -= (size_t)j.nops * sizeof(Op);
        t->dcur = (uint8_t *)((uintptr_t)t->dcur & ~(uintptr_t)15);
        if (!test) {
            uint32_t k = (pc >> 2) & (JIT_SLOTS - 1);
            t->slot[k].pc = pc, t->slot[k].end = pc + 4 * n, t->slot[k].code = entry;
        }
        return entry;
    }
    return NULL;
}
static uint8_t *jit_build(JitTls *t, uint32_t pc, uint32_t stop, int test, uint32_t tw) {
    jit_write_begin();
    uint8_t *r = jit_build_in(t, pc, stop, test, tw);
    jit_write_end();
    return r;
}

static uint8_t *jit_get(JitTls *t, uint32_t pc, uint32_t ret) {
    uint32_t g = __atomic_load_n(&g_code_gen, __ATOMIC_ACQUIRE);
    if (t->gen != g) {
        if (t->in_jit) return NULL;
        jit_flush(t);
        t->gen = g;
    }
    uint32_t k = (pc >> 2) & (JIT_SLOTS - 1);
    if (t->slot[k].code && t->slot[k].pc == pc) {
        if (!(ret > pc && ret < t->slot[k].end)) return t->slot[k].code;
        uint8_t *old = t->slot[k].code;
        uint32_t ep = t->epoch;
        uint8_t *nb = jit_build(t, pc, ret, 0, 0);  // (shorter: it stops at ret)
        if (nb && t->epoch == ep) jit_redirect(old, nb);
        return nb;
    }
    return jit_build(t, pc, 0, 0, 0);
}

// One interpreter block from pc (no JIT code for it now): interp_run_inner's inner loop
static uint32_t interp_block_once(Cpu *c, uint32_t pc, uint32_t ret, int *link, uint32_t *last, uint64_t *n) {
    Block *b = block_at(pc);
    const Op *o = b->ops, *end = o + b->n;
    uint32_t next;
    for (;;) {
        g_ip_pc = g_ip_last = pc;
        if (__builtin_expect(*(const uint32_t *)G2H(pc) != o->mem, 0)) {
            b = block_build(t_blocks, pc);
            o = b->ops, end = o + b->n;
        }
        (*n)++;
        next = o->cond != 14 && !cond_ok(c, o->cond) ? pc + 4 : o->fn(c, o, pc, link);
        if (next != pc + 4 || *link || ++o == end || next == ret) break;
        pc = next;
    }
    *last = pc;
    return next;
}

// interp_run_inner with JIT blocks
static void jit_run(Cpu *c, JitTls *t) {
    // (a run nested in an interpreter op an outer block called, on this Cpu: the outer block's fields come back)
    uint8_t save[JO_END - JO_RET];
    memcpy(save, (uint8_t *)c + JO_RET, sizeof save);
    g_ip_cpu = c;
    uint32_t ret = c->r[14], pc = c->pc, sp = c->r[13];
    uint64_t n = 0;
    for (;;) {
        int link = 0;
        uint32_t next, retpc, site = 0, ep = t->epoch;
        uint8_t *code = jit_get(t, pc, ret);
        if (code) {
            ep = t->epoch;
            JF(c, JO_RET) = ret, JF(c, JO_GEN) = t->gen, JF(c, JO_LINK) = 0, JF(c, JO_EXIT) = 0;
            uint64_t i0 = *(uint64_t *)((uint8_t *)c + JO_INSNS);
            t->in_jit++;
            next = t->enter(c, code, t->ptrs);
            t->in_jit--;
            n += *(uint64_t *)((uint8_t *)c + JO_INSNS) - i0;
            if (JF(c, JO_EXIT)) {  // the block's code changed: a new one, and the old one's callers sent to it
                uint8_t *old = t->base + JF(c, JO_SITE);
                uint32_t k = (next >> 2) & (JIT_SLOTS - 1);
                if (t->slot[k].code == old) t->slot[k].code = NULL;
                uint8_t *nb = jit_get(t, next, ret);
                if (nb && t->epoch == ep) jit_redirect(old, nb);
                pc = next;
                continue;
            }
            link = (int)JF(c, JO_LINK);
            retpc = JF(c, JO_RETPC);
            site = JF(c, JO_SITE);
        } else {
            uint32_t last;
            next = interp_block_once(c, pc, ret, &link, &last, &n);
            retpc = last + 4;
        }
        if (next == ret && !link && c->r[13] >= sp) break;
        if (interp_owns(next)) {
            if (site && t->epoch == ep) {  // chain the exit to the next block
                uint8_t *nb = jit_get(t, next, ret);
                if (nb && t->epoch == ep) jit_patch_site(t, site, nb);
            }
            pc = next;
            continue;
        }
        if (link) {
            c->pc = next;
            g_ip_pc = 0;
            rt_dispatch(c);
            pc = retpc;
            continue;
        }
        if (next == ret) break;
        uint32_t lr = c->r[14];
        c->pc = next;
        g_ip_pc = 0;
        rt_dispatch(c);
        if (lr == ret || !interp_owns(lr)) break;
        pc = lr;
    }
    memcpy((uint8_t *)c + JO_RET, save, sizeof save);
    if (stats_on > 0) t_insns += n;
}

static int jit_enabled(void) {
    if (jit_mode < 0) {
        const char *s = getenv("MHGU_JIT");
        jit_mode = !(s && (!strcmp(s, "0") || !strcmp(s, "off")));
        const char *d = getenv("MHGU_JIT_DUMP");
        if (jit_mode && d && *d) jit_dump = fopen(d, "wb");
        LOG("[interp] JIT %s\n", jit_mode ? "on (" JIT_ARCH "): interpreted code runs as host code (MHGU_JIT=0: off)"
                                          : "off (MHGU_JIT=0)");
    }
    return jit_mode;
}

// (tools/test_lift.py --jit) One instruction as a block of its own
uint32_t interp_exec_jit(Cpu *c, uint32_t pc, uint32_t w, int *link) {
    JitTls *t = jit_tls();
    if (!t) FATAL("jit test: no arena\n");
    uint8_t *code = jit_build(t, pc, 0, 1, w);
    if (!code) FATAL("jit test: no block\n");
    JF(c, JO_RET) = 0, JF(c, JO_GEN) = t->gen, JF(c, JO_LINK) = 0, JF(c, JO_EXIT) = 0;
    uint32_t next = t->enter(c, code, t->ptrs);
    *link = (int)JF(c, JO_LINK);
    return next;
}
#endif
