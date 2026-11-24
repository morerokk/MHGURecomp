// Executable memory for the interpreter's JIT (runtime/interp_jit.h): one arena per thread that runs interpreted
// code, readable, writable and executable (its blocks are patched as they get linked). On Windows the arena's code
// also gets a function table entry (one frame layout for all of it), so a longjmp out of guest code that runs in it
// (ExitThread, runtime/kernel.c) can unwind through its frame. (AArch64 hosts: Android, Linux; not Apple's, which want MAP_JIT.)
#if (defined(__x86_64__) || defined(__aarch64__)) && !defined(__APPLE__) && !defined(MHGU_NO_JIT)
#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>

void *jit_mem_alloc(size_t n) { return VirtualAlloc(NULL, n, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE); }

// rf: one RUNTIME_FUNCTION (3 offsets from base) inside the arena
int jit_mem_register(void *base, void *rf) {
    return RtlAddFunctionTable((PRUNTIME_FUNCTION)rf, 1, (DWORD64)(uintptr_t)base) ? 1 : 0;
}

void jit_mem_free(void *base, size_t n, void *rf) {
    (void)n;
    if (rf) RtlDeleteFunctionTable((PRUNTIME_FUNCTION)rf);
    VirtualFree(base, 0, MEM_RELEASE);
}
#else
#include <sys/mman.h>

void *jit_mem_alloc(size_t n) {
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

int jit_mem_register(void *base, void *rf) {
    (void)base, (void)rf;
    return 1;
}

void jit_mem_free(void *base, size_t n, void *rf) {
    (void)rf;
    munmap(base, n);
}
#endif
#endif
