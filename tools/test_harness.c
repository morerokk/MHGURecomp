// Runs lifted test instructions (tests.c from tools/test_lift.py) on the vectors in <dir>/vectors.bin and writes
// the resulting state to stdout. Guest memory: the 4 GB window at GUEST_BASE, committed on first touch.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include "cpu.h"

typedef void (*T)(Cpu *);
extern T tests[];
extern int ntests;
volatile int g_fnprof;
void fnprof_hit(uint32_t a, uint32_t lr, uint32_t r0) { (void)a; (void)lr; (void)r0; }
void rt_dispatch(Cpu *c) { (void)c; abort(); }
void rt_svc(Cpu *c, uint32_t imm, uint32_t pc) { (void)c; (void)imm; (void)pc; abort(); }
void rt_bkpt(Cpu *c, uint32_t imm, uint32_t pc) { (void)c; (void)imm; (void)pc; abort(); }
void rt_udf(Cpu *c, uint32_t pc) { (void)c; (void)pc; abort(); }
void rt_thumb(Cpu *c, uint32_t pc) { (void)c; (void)pc; abort(); }
uint64_t rt_ticks(void) { return 0; }
void rt_fatal_trace(void) { abort(); }  // (runtime/interp.c, --interp)

#define DATA 0x40000000u
#define DATA_SIZE 0x2000u

static unsigned char *readall(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(3); }
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *p = malloc(*n);
    if (fread(p, 1, *n, f) != *n) exit(3);
    fclose(f);
    return p;
}

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

int main(int argc, char **argv) {
    char path[1024];
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
    if (!VirtualAlloc((void *)(uintptr_t)GUEST_BASE, 0x100000000ull, MEM_RESERVE, PAGE_READWRITE)) return 4;
    AddVectoredExceptionHandler(1, commit_on_touch);
#elif defined(__APPLE__)
    // no MAP_FIXED_NOREPLACE: ask for the address as a hint and refuse anything else
    if (mmap((void *)(uintptr_t)GUEST_BASE, 0x100000000ull, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0) != (void *)(uintptr_t)GUEST_BASE) return 4;
#else
    if (mmap((void *)(uintptr_t)GUEST_BASE, 0x100000000ull, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_NORESERVE, -1, 0) == MAP_FAILED) return 4;
#endif
    size_t n, imgn, patn;
    snprintf(path, sizeof path, "%s/vectors.bin", argv[1]);
    unsigned char *v = readall(path, &n);
    snprintf(path, sizeof path, "%s/image.bin", argv[1]);
    unsigned char *img = readall(path, &imgn);
    snprintf(path, sizeof path, "%s/pattern.bin", argv[1]);
    unsigned char *pat = readall(path, &patn);
    uint32_t count, base;
    memcpy(&count, v, 4);
    memcpy(&base, v + 4, 4);
    memcpy(G2H(base), img, imgn);
    unsigned char *p = v + 8;
    static Cpu cpu;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t ti, regs[15], fl[4];
        uint64_t d[32];
        memcpy(&ti, p, 4); p += 4;
        memcpy(regs, p, 60); p += 60;
        memcpy(fl, p, 16); p += 16;
        memcpy(d, p, 256); p += 256;
        memset(&cpu, 0, sizeof cpu);
        memcpy(cpu.r, regs, 60);
        cpu.nf = fl[0] >> 3; cpu.zf = (fl[0] >> 2) & 1; cpu.cf = (fl[0] >> 1) & 1; cpu.vf = fl[0] & 1;
        cpu.qf = fl[1]; cpu.ge = fl[2]; cpu.fpscr = fl[3];
        cpu.excl_addr = ~0u;
        memcpy(cpu.v.d, d, 256);
        memcpy(G2H(DATA), pat, DATA_SIZE);
        tests[ti](&cpu);
        uint32_t out[19];
        memcpy(out, cpu.r, 60);
        out[15] = (cpu.nf << 3) | (cpu.zf << 2) | (cpu.cf << 1) | cpu.vf;
        out[16] = cpu.qf; out[17] = cpu.ge; out[18] = cpu.fpscr;
        fwrite(out, 4, 19, stdout);
        fwrite(cpu.v.d, 8, 32, stdout);
        uint32_t h = 2166136261u;
        const unsigned char *m = G2H(DATA);
        for (uint32_t k = 0; k < DATA_SIZE; k++) h = (h ^ m[k]) * 16777619u;
        fwrite(&h, 4, 1, stdout);
    }
    return 0;
}
