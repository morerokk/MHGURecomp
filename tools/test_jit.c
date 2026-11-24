// The interpreter's JIT against the interpreter itself (tools/test_jit.py builds and runs this; runtime/interp_jit.h).
//   test_jit words <file>                 "pc word" lines: each instruction as a block of its own on random states,
//                                         through interp_exec_op and interp_exec_jit, registers / flags / VFP / memory
//                                         compared (Windows and Linux)
//   test_jit prog <image> <funcs> <n>     whole runs (Linux: fork): the module image at 0x200000, all of it
//                                         interpreted; from each function entry in <funcs> (hex lines) a run on random
//                                         registers and memory, once with MHGU_JIT=0 and once with the JIT, each in a
//                                         child with a time limit; calls out return r0 = 0, the run ends at a magic
//                                         return address; the results (registers, flags, a hash of the data and stack
//                                         regions, calls out, how it ended) must match
// One known difference in prog runs: a store into the block running itself (random pointers walking over the code):
// the interpreter compares each word before it runs, the JIT a block's words when it is entered.
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "cpu.h"
#include "interp.h"

volatile int g_fnprof;
void fnprof_hit(uint32_t a, uint32_t lr, uint32_t r0) { (void)a; (void)lr; (void)r0; }
static unsigned long n_out;
void rt_dispatch(Cpu *c) { n_out++; c->r[0] = 0; c->r[1] = (uint32_t)n_out; }
void rt_svc(Cpu *c, uint32_t imm, uint32_t pc) { (void)c; (void)imm; (void)pc; fflush(stdout); _exit(40); }
void rt_bkpt(Cpu *c, uint32_t imm, uint32_t pc) { (void)c; (void)imm; (void)pc; fflush(stdout); _exit(41); }
void rt_udf(Cpu *c, uint32_t pc) { (void)c; (void)pc; fflush(stdout); _exit(42); }
void rt_thumb(Cpu *c, uint32_t pc) { (void)c; (void)pc; fflush(stdout); _exit(43); }
uint64_t rt_ticks(void) { return 0; }
void rt_fatal_trace(void) { fflush(stdout); fflush(stderr); _exit(44); }
uint8_t g_fnhook[0x03000000u >> 5];
bool fnhook_entry_word(uint32_t pc, uint32_t *w) { (void)pc; (void)w; return false; }
bool kernel_mapped(uint64_t addr, uint32_t size) { (void)addr; (void)size; return true; }
uint32_t kernel_heap_size(void) { return 0; }
void forge_guest_fault(uint32_t pc, uint32_t addr, int size, int write, const char *why) {
    (void)pc, (void)addr, (void)size, (void)write, (void)why;
}
uint64_t plat_now_ns(void) { return 0; }

static uint64_t rs = 88172645463325252ull;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)rs; }

#ifdef _WIN32
static LONG CALLBACK commit_on_touch(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    uint64_t a = er->ExceptionInformation[1];
    if (a < GUEST_BASE || a >= GUEST_BASE + 0x100000000ull) return EXCEPTION_CONTINUE_SEARCH;
    VirtualAlloc((void *)(uintptr_t)(a & ~0xFFFFull), 0x10000, MEM_COMMIT, PAGE_READWRITE);
    return EXCEPTION_CONTINUE_EXECUTION;
}
#endif

static int guest_memory(void) {  // the 4 GB window, committed on touch
#ifdef _WIN32
    if (!VirtualAlloc((void *)(uintptr_t)GUEST_BASE, 0x100000000ull, MEM_RESERVE, PAGE_READWRITE)) return 0;
    AddVectoredExceptionHandler(1, commit_on_touch);
    return 1;
#else
    return mmap((void *)(uintptr_t)GUEST_BASE, 0x100000000ull, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_NORESERVE, -1, 0) != MAP_FAILED;
#endif
}

// ================================================================ words
#define WDATA 0x50000000u
#define WSZ 0x4000u
static int words(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 3;
    static uint8_t m0[3 * WSZ], m1[3 * WSZ];
    unsigned pc, w;
    int n = 0, bad = 0;
    static Cpu a, b;
    uint8_t *g = (uint8_t *)G2H(WDATA - WSZ);
    while (fscanf(f, "%x %x", &pc, &w) == 2) {
        for (int v = 0; v < 6; v++) {
            memset(&a, 0, sizeof a);
            for (int r = 0; r < 15; r++) a.r[r] = (v & 1) ? rnd() : WDATA + (rnd() & 0xFFC);
            a.r[13] = WDATA + 0x800;
            a.nf = rnd() & 1, a.zf = rnd() & 1, a.cf = rnd() & 1, a.vf = rnd() & 1;
            for (int i = 0; i < 64; i++) a.v.s[i] = rnd();
            for (unsigned i = 0; i < sizeof m0; i++) m0[i] = (uint8_t)rnd();
            if (v & 1) {  // random values, but a memory instruction's base in the data
                uint32_t rn = (w >> 16) & 15, rm = w & 15;
                a.r[rn] = WDATA + (rnd() & 0xFFC);
                if ((w & 0x0E000000u) == 0x06000000u || (w & 0x0E400090u) == 0x00000090u) a.r[rm] &= 0xFC;
            }
            b = a;
            memcpy(g, m0, sizeof m0);
            int la = 0, lb = 0;
            uint32_t na = interp_exec_op(&a, pc, w, &la);
            memcpy(m1, g, sizeof m1);
            memcpy(g, m0, sizeof m0);
            uint32_t nb = interp_exec_jit(&b, pc, w, &lb);
            int diff = na != nb || la != lb || memcmp(a.r, b.r, 60) || a.nf != b.nf || a.zf != b.zf || a.cf != b.cf ||
                       a.vf != b.vf || a.qf != b.qf || a.ge != b.ge || memcmp(&a.v, &b.v, sizeof a.v) ||
                       memcmp(m1, g, sizeof m1);
            if (diff && bad++ < 20) printf("DIFF %08x %08x (vector %d): next %x / %x\n", pc, w, v, na, nb);
            n++;
        }
    }
    fclose(f);
    printf("words: %d runs, %d different\n", n, bad);
    return bad != 0;
}

// ================================================================ prog
#ifndef _WIN32
#define BASE 0x200000u
#define DATA 0x60000000u
#define DSZ 0x20000u
#define STK 0x70000000u
#define SSZ 0x20000u
#define MAGIC 0xFFFFFF00u

static uint64_t fnv(const uint8_t *p, size_t n, uint64_t h) {
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

typedef struct { uint32_t r[16], flags; uint64_t h; unsigned long outs; } Res;

static void run_one(uint32_t fn, uint64_t seed, int jit, int fd) {
    if (!jit) setenv("MHGU_JIT", "0", 1);
    alarm(3);
    rs = seed;
    uint8_t *d = (uint8_t *)G2H(DATA), *s = (uint8_t *)G2H(STK);
    for (uint32_t i = 0; i < DSZ; i += 4) {  // words, half of them pointers into the data
        uint32_t v = rnd();
        if (v & 1) v = DATA + (v & (DSZ - 4));
        memcpy(d + i, &v, 4);
    }
    memset(s, 0, SSZ);
    static Cpu c;
    memset(&c, 0, sizeof c);
    for (int r = 0; r < 13; r++) {
        uint32_t v = rnd();
        c.r[r] = (v & 3) ? DATA + (v & (DSZ / 2 - 4)) : v & 0xFF;
    }
    c.r[13] = STK + SSZ - 0x100;
    c.r[14] = MAGIC;
    c.pc = fn;
    interp_run(&c);
    Res res = {0};
    memcpy(res.r, c.r, 64);
    res.flags = c.nf | c.zf << 1 | c.cf << 2 | c.vf << 3;
    res.h = fnv(s, SSZ, fnv(d, DSZ, 1469598103934665603ull));
    res.outs = n_out;
    if (write(fd, &res, sizeof res) != sizeof res) _exit(50);
    _exit(0);
}

// 0: returned (*out), -1: timed out, else how it ended (an exit code + 100, a signal + 1000)
static int child(uint32_t fn, uint64_t seed, int jit, Res *out) {
    int p[2];
    if (pipe(p)) return -2;
    fflush(stdout);
    pid_t pid = fork();
    if (!pid) {
        close(p[0]);
        int null = open("/dev/null", O_WRONLY);  // (the FATALs of code run off into data)
        if (null >= 0) dup2(null, 2);
        run_one(fn, seed, jit, p[1]);
    }
    close(p[1]);
    memset(out, 0, sizeof *out);
    ssize_t n = read(p[0], out, sizeof *out);
    close(p[0]);
    int st;
    waitpid(pid, &st, 0);
    if (WIFSIGNALED(st)) return WTERMSIG(st) == SIGALRM ? -1 : 1000 + WTERMSIG(st);
    if (n != (ssize_t)sizeof *out) return 100 + WEXITSTATUS(st);
    return 0;
}

static int prog(const char *image, const char *funcs, int limit) {
    FILE *f = fopen(image, "rb");
    if (!f) return 3;
    size_t n = fread(G2H(BASE), 1, 0x8000000, f);
    fclose(f);
    interp_add_range(BASE, BASE + (uint32_t)n);
    if (!(f = fopen(funcs, "r"))) return 3;
    int done = 0, same = 0, ret = 0, diff = 0, timeouts = 0;
    unsigned x;
    while (done < limit && fscanf(f, "%x", &x) == 1) {
        for (int v = 0; v < 2 && done < limit; v++, done++) {
            uint64_t seed = 0x9E3779B97F4A7C15ull * (x + 1) + (uint64_t)v;
            Res a, b;
            int sa = child(x, seed, 0, &a), sb = child(x, seed, 1, &b);
            if (sa == -1 || sb == -1) { timeouts++; continue; }
            int eq = sa == sb &&
                     (sa || (!memcmp(a.r, b.r, 60) && a.flags == b.flags && a.h == b.h && a.outs == b.outs));
            if (eq) { same++, ret += !sa; continue; }
            if (diff++ < 25) {
                printf("DIFF fn %x vector %d: ended %d / %d", x, v, sa, sb);
                if (!sa && !sb) {
                    for (int r = 0; r < 15; r++)
                        if (a.r[r] != b.r[r]) printf("  r%d %x / %x", r, a.r[r], b.r[r]);
                    if (a.flags != b.flags) printf("  nzcv %x / %x", a.flags, b.flags);
                    if (a.h != b.h) printf("  memory");
                    if (a.outs != b.outs) printf("  calls out %lu / %lu", a.outs, b.outs);
                }
                printf("\n");
            }
        }
    }
    fclose(f);
    printf("prog: %d runs: %d the same (%d returned, %d ended the same other way), %d different, %d timed out\n",
           done, same, ret, same - ret, diff, timeouts);
    return diff != 0;
}
#endif

int main(int argc, char **argv) {
    if (!guest_memory()) return 4;
    if (argc >= 3 && !strcmp(argv[1], "words")) return words(argv[2]);
#ifndef _WIN32
    if (argc >= 5 && !strcmp(argv[1], "prog")) return prog(argv[2], argv[3], atoi(argv[4]));
#endif
    fprintf(stderr, "usage: test_jit words <file> | prog <image> <funcs> <n> (Linux)\n");
    return 2;
}
