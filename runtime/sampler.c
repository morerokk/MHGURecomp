// (debug, Windows) MHGU_SAMPLE=<file>: a sampling profiler. A thread stops every other thread of the process about
// every millisecond, notes its call stack (up to 16 return addresses) and the CPU cycles it used since the last look,
// and writes that to <file>; tools/samples.py reads it back: which game functions (guest addresses) and host
// functions the time went to, per thread, over a span of time. Sleeping threads are left out (no cycles used).
// Next to it, <file>.map: the modules and the recompiled functions' host addresses.
// Records: 'S' t(QPC) tid cycles n frames[n] (u64s); 'F' t(QPC) presented frames.
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gamelib.h"
#include "rt.h"

#define MAXT 256
#define DEPTH 16
static struct { DWORD tid; HANDLE h; ULONG64 cycles; } th[MAXT];
static int nth;
static FILE *out;

static void refresh_threads(void) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te = {.dwSize = sizeof te};
    DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        int k = 0;
        while (k < nth && th[k].tid != te.th32ThreadID) k++;
        if (k < nth || nth == MAXT) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        th[nth].tid = te.th32ThreadID, th[nth].h = h;
        QueryThreadCycleTime(h, &th[nth].cycles);
        nth++;
    }
    CloseHandle(snap);
}

static int walk(CONTEXT *ctx, uint64_t *frames) {
    int n = 0;
    while (n < DEPTH && ctx->Rip) {
        frames[n++] = ctx->Rip;
        DWORD64 base;
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(ctx->Rip, &base, NULL);
        if (fe) {
            PVOID hd;
            DWORD64 est;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx->Rip, fe, ctx, &hd, &est, NULL);
        } else {  // (a leaf function)
            if (!ctx->Rsp || (ctx->Rsp & 7)) break;
            ctx->Rip = *(DWORD64 *)ctx->Rsp;
            ctx->Rsp += 8;
        }
    }
    return n;
}

static DWORD WINAPI sampler(void *arg) {
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, 0x2 /* high resolution */, TIMER_ALL_ACCESS);
    uint64_t round = 0;
    static CONTEXT ctx __attribute__((aligned(16)));
    for (;;) {
        if (round % 256 == 0) refresh_threads();
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (round % 50 == 0) {
            uint64_t rec[2] = {(uint64_t)now.QuadPart, g_present_count};
            fputc('F', out);
            fwrite(rec, 8, 2, out);
        }
        for (int i = 0; i < nth; i++) {
            ULONG64 cyc;
            if (!QueryThreadCycleTime(th[i].h, &cyc)) continue;
            uint64_t used = cyc - th[i].cycles;
            th[i].cycles = cyc;
            if (used < 20000 && round % 16) continue;  // (asleep: only every 16th round, to see where it waits)
            if (SuspendThread(th[i].h) == (DWORD)-1) continue;
            ctx.ContextFlags = CONTEXT_FULL;
            uint64_t rec[3 + 1 + DEPTH];
            int n = 0;
            if (GetThreadContext(th[i].h, &ctx)) n = walk(&ctx, rec + 4);
            ResumeThread(th[i].h);
            rec[0] = (uint64_t)now.QuadPart, rec[1] = th[i].tid, rec[2] = used, rec[3] = (uint64_t)n;
            fputc('S', out);
            fwrite(rec, 8, 4 + n, out);
        }
        if (++round % 256 == 0) fflush(out);
        LARGE_INTEGER due = {.QuadPart = -10000};  // 1 ms
        if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(timer, INFINITE);
        else Sleep(1);
    }
    return 0;
}

static void write_map(const char *path) {
    char p[1100];
    snprintf(p, sizeof p, "%s.map", path);
    FILE *f = fopen(p, "w");
    if (!f) return;
    LARGE_INTEGER qpf;
    QueryPerformanceFrequency(&qpf);
    fprintf(f, "qpf %lld\n", (long long)qpf.QuadPart);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    MODULEENTRY32 me = {.dwSize = sizeof me};
    for (BOOL ok = Module32First(snap, &me); ok; ok = Module32Next(snap, &me))
        fprintf(f, "module %llx %lx %s\n", (unsigned long long)(uintptr_t)me.modBaseAddr, (unsigned long)me.modBaseSize,
                me.szExePath);
    CloseHandle(snap);
    const GameLibTables *t = gamelib_tables();
    if (t) {
        for (uint32_t i = 0; i < *t->main_count; i++)
            fprintf(f, "fn %llx main %x\n", (unsigned long long)(uintptr_t)t->main_fns[i], t->main_addrs[i]);
        for (uint32_t i = 0; i < *t->sdk_count; i++)
            fprintf(f, "fn %llx sdk %x\n", (unsigned long long)(uintptr_t)t->sdk_fns[i], t->sdk_addrs[i]);
    }
    fclose(f);
}

void sampler_init(void) {
    const char *path = getenv("MHGU_SAMPLE");
    if (!path || !*path) return;
    out = fopen(path, "wb");
    if (!out) { LOG("[sampler] cannot write %s\n", path); return; }
    setvbuf(out, NULL, _IOFBF, 1 << 20);
    write_map(path);
    LOG("[sampler] sampling to %s\n", path);
    CreateThread(NULL, 0, sampler, NULL, 0, NULL);
}
#else
void sampler_init(void) {}
#endif
