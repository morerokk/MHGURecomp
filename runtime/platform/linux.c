// Linux host layer (runtime/platform.h).
//
// The guest window is reserved PROT_NONE up front and pages are backed on their first touch by the SIGSEGV handler
// below (64 KB at a time), exactly as windows.c does with MEM_RESERVE + MEM_COMMIT.
//
// Differences from Windows worth knowing:
// - No GetWriteWatch equivalent: plat_written_pages() reports "no write tracking" (-1), so the renderer checks
//   texture / buffer contents itself instead of trusting a dirty-page list (runtime/nvn/vulkan/textures.c).
// - Signal handlers are process-wide, so one SIGSEGV handler does both jobs: back a guest page, or report the fault.
//   It is installed by plat_reserve_guest(), i.e. from main.c's constructor, before any guest code can run.
// - No console to keep or free: with stderr not a terminal nobody is watching, so the log goes to log.txt.
// Android (bionic) uses this file too, with runtime/platform/android.c for the app's folder and its message box.
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/ucontext.h>
#include <time.h>
#include <unistd.h>

#include "../platform.h"

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#ifdef __ANDROID__
const char *android_home(void);  // runtime/platform/android.c
void android_load_env(void);
void android_unpack_assets(void);
void android_error_box(const char *msg);
// (bionic has execinfo's backtrace() only from Android 13 on: the unwinder's)
#include <unwind.h>
typedef struct { void **bt; int n, max; } Unwound;
static _Unwind_Reason_Code unwind_step(struct _Unwind_Context *uc, void *arg) {
    Unwound *u = arg;
    uintptr_t pc = _Unwind_GetIP(uc);
    if (pc && u->n < u->max) u->bt[u->n++] = (void *)pc;
    return u->n < u->max ? _URC_NO_REASON : _URC_END_OF_STACK;
}
static int backtrace(void **bt, int max) {
    Unwound u = {bt, 0, max};
    _Unwind_Backtrace(unwind_step, &u);
    return u.n;
}
#else
#include <execinfo.h>
#endif

// The interrupted thread's registers in a signal handler, and whether the fault was a write (1 / 0, -1 unknown).
#if defined(__x86_64__)
static uint64_t ctx_pc(const ucontext_t *c) { return (uint64_t)c->uc_mcontext.gregs[REG_RIP]; }
static uint64_t ctx_sp(const ucontext_t *c) { return (uint64_t)c->uc_mcontext.gregs[REG_RSP]; }
static int ctx_write(const ucontext_t *c) { return (c->uc_mcontext.gregs[REG_ERR] & 2) != 0; }
#elif defined(__aarch64__)
static uint64_t ctx_pc(const ucontext_t *c) { return c->uc_mcontext.pc; }
static uint64_t ctx_sp(const ucontext_t *c) { return c->uc_mcontext.sp; }
static int ctx_write(const ucontext_t *c) {  // (the data abort's ESR, in the records after the registers: bit 6 WnR)
    const uint8_t *p = c->uc_mcontext.__reserved, *end = p + sizeof c->uc_mcontext.__reserved;
    while (p + sizeof(struct _aarch64_ctx) <= end) {
        const struct _aarch64_ctx *h = (const struct _aarch64_ctx *)p;
        if (!h->magic || h->size < sizeof *h) break;
        if (h->magic == ESR_MAGIC) return (((const struct esr_context *)h)->esr >> 6) & 1;
        p += h->size;
    }
    return -1;
}
#endif

// ---- guest address space
static uint64_t g_lo, g_hi;
#define COMMIT_CHUNK 0x10000ull
#define MAX_CHUNKS ((0x100000000ull + COMMIT_CHUNK - 1) / COMMIT_CHUNK)

// Per-chunk state, so the fault handler can tell "not backed yet" (back it and resume) from a real fault in backed
// memory (report it) without a lock: a signal handler may not take one, and several guest threads faulting into the
// same fresh chunk is the normal case. 0 not backed, 1 somebody is backing it, 2 backed.
enum { CHUNK_FREE = 0, CHUNK_BUSY, CHUNK_READY };
static _Atomic unsigned char g_chunk[MAX_CHUNKS];

static bool chunk_back(uint64_t base) {
    void *p = mmap((void *)(uintptr_t)base, COMMIT_CHUNK, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
    return p == (void *)(uintptr_t)base;
}

// The faulting address is inside the window and its chunk is not backed: back it. True = resume the instruction.
static bool lazy_commit(uint64_t a) {
    if (a < g_lo || a >= g_hi) return false;
    uint64_t idx = (a - g_lo) / COMMIT_CHUNK;
    if (idx >= MAX_CHUNKS) return false;
    unsigned char expect = CHUNK_FREE;
    if (atomic_compare_exchange_strong(&g_chunk[idx], &expect, CHUNK_BUSY)) {
        bool ok = chunk_back(g_lo + idx * COMMIT_CHUNK);
        atomic_store(&g_chunk[idx], ok ? CHUNK_READY : CHUNK_FREE);
        return ok;
    }
    while (atomic_load(&g_chunk[idx]) == CHUNK_BUSY) atomic_thread_fence(memory_order_acquire);
    return atomic_load(&g_chunk[idx]) == CHUNK_READY;  // (whoever won it backed the page: resume)
}

void (*plat_touch_hook)(uint64_t guest_addr, int write);
static void fault_handler(int sig, siginfo_t *si, void *uc);

bool plat_reserve_guest(uint64_t base, uint64_t size) {
    void *p = mmap((void *)(uintptr_t)base, size, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_NORESERVE, -1, 0);
    if (p != (void *)(uintptr_t)base) return false;
    g_lo = base;
    g_hi = base + size;
    struct sigaction sa = {0};
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    return true;
}

void plat_commit(uint64_t addr, uint64_t size) {
    if (!size) return;
    uint64_t a = addr & ~(COMMIT_CHUNK - 1), e = (addr + size + COMMIT_CHUNK - 1) & ~(COMMIT_CHUNK - 1);
    for (uint64_t c = a; c < e && c < g_hi; c += COMMIT_CHUNK) lazy_commit(c);
    if (e > g_hi)
        fprintf(stderr, "[plat] commit %llx+%llx past the guest window\n", (unsigned long long)addr,
                (unsigned long long)size);
}

int plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max, uint64_t *page) {
    (void)addr; (void)size; (void)pages; (void)max; (void)page;
    return -1;  // no write watching here: the renderer compares contents
}

bool plat_committed(uint64_t addr, uint64_t *end) {
    if (!g_lo || addr < g_lo || addr >= g_hi) { *end = addr + PLAT_PAGE; return false; }
    uint64_t idx = (addr - g_lo) / COMMIT_CHUNK;
    if (atomic_load(&g_chunk[idx]) != CHUNK_READY) {
        *end = g_lo + (idx + 1) * COMMIT_CHUNK;
        return false;
    }
    uint64_t n = idx;
    while (n + 1 < MAX_CHUNKS && n - idx < 4096 && atomic_load(&g_chunk[n + 1]) == CHUNK_READY) n++;
    *end = g_lo + (n + 1) * COMMIT_CHUNK;
    return true;
}

int plat_protect(uint64_t addr, uint64_t size, int prot) {
    int p = prot == PLAT_PROT_NONE ? PROT_NONE : prot == PLAT_PROT_READ ? PROT_READ : PROT_READ | PROT_WRITE;
    if (addr >= g_lo && addr < g_hi) plat_commit(addr, size);
    return mprotect((void *)(uintptr_t)addr, size, p) == 0 ? 0 : -1;
}

// ---- time
uint64_t plat_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void plat_sleep_until_ns(uint64_t t) {
    for (;;) {
        uint64_t now = plat_now_ns();
        if (now >= t) return;
        uint64_t left = t - now;
        if (left < 200000) { sched_yield(); continue; }  // (the last bit: spinning beats a syscall)
        // Asleep until 200 us before t (an absolute time on plat_now_ns's clock). (It was given the time left as if it
        // were absolute: long past, so it returned at once and every sleep was a busy loop of syscalls.)
        uint64_t wake = t - 200000;
        struct timespec due = {(time_t)(wake / 1000000000ull), (long)(wake % 1000000000ull)};
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &due, NULL);  // (EINTR: the loop re-reads the clock)
    }
}

// ---- process
void plat_init(void) {
    // send() on a dropped Local Play connection gives EPIPE here instead of killing the process: Windows reports
    // WSAEPIPE to the caller, which is what nn::socket expects.
    signal(SIGPIPE, SIG_IGN);
#ifdef __ANDROID__
    // Android starts apps with umask 077: folders made here (save, mods, cheats...) would be the app's alone, and
    // nothing could put files in them over USB or adb (group ext_data_rw: the files folder's)
    umask(007);
    android_load_env();
#endif
}

unsigned long plat_process_id(void) { return (unsigned long)getpid(); }
bool plat_wait_process(unsigned long pid) {
    struct timespec pause = {0, 10000000};
    for (int i = 0; i < 3000; ++i) {
        if (kill((pid_t)pid, 0) < 0) return errno == ESRCH;
        nanosleep(&pause, NULL);
    }
    return false;
}

bool plat_exe_path(char *buf, size_t size) {
    ssize_t n = readlink("/proc/self/exe", buf, size - 1);
    if (n <= 0 || (size_t)n >= size - 1) return false;
    buf[n] = 0;
    return true;
}

// Started on its own: run in the executable's folder (MHGU_HOME: in that folder instead); with nobody watching stderr (a launcher, a .desktop entry, `&`)
// the log goes to log.txt and plat_fatal_notice then points the player at it.
static bool g_own_log;

void plat_standalone(void) {
    char exe[PATH_MAX];
    const char *home = getenv("MHGU_HOME");
#ifdef __ANDROID__
    if (!home || !*home) home = android_home();
#endif
    if (home && *home) {
        mkdir(home, 0755);
        if (chdir(home) != 0) { /* the folder we are in will do */ }
    } else if (plat_exe_path(exe, sizeof exe)) {
        char *s = strrchr(exe, '/');
        if (s) { *s = 0; if (chdir(exe) != 0) { /* the folder we are in will do */ } }
    }
#ifdef __ANDROID__
    android_unpack_assets();  // (pipelines.list and bonus/system, which a PC release has beside the exe)
#endif
    if (!isatty(STDERR_FILENO)) {
        if (freopen("log.txt", "w", stderr)) { setvbuf(stderr, NULL, _IONBF, 0); g_own_log = true; }
    }
}

// $XDG_DATA_HOME/MHGURecomp, else ~/.local/share/MHGURecomp (docs/STATUS.md "Save data", runtime/platform.h).
static bool mkdir_p(const char *path) {
    char buf[PATH_MAX];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *s = buf + 1; *s; s++) {
        if (*s != '/') continue;
        *s = 0;
        if (mkdir(buf, 0777) != 0 && errno != EEXIST) return false;
        *s = '/';
    }
    return mkdir(buf, 0777) == 0 || errno == EEXIST;
}

bool plat_user_dir(char *buf, size_t size) {
#ifdef __ANDROID__
    snprintf(buf, size, "%s", android_home());  // (one folder for everything: the app's own)
    return true;
#endif
    const char *base = getenv("XDG_DATA_HOME");
    char fallback[PATH_MAX];
    if (!base || !*base) {
        const char *home = getenv("HOME");
        if (!home || !*home) return false;
        snprintf(fallback, sizeof fallback, "%s/.local/share", home);
        base = fallback;
    }
    char dir[PATH_MAX];
    if (snprintf(dir, sizeof dir, "%s/MHGURecomp", base) >= (int)sizeof dir) return false;
    if (!mkdir_p(dir)) return false;
    struct stat st;
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) return false;
    snprintf(buf, size, "%s", dir);
    return true;
}

static int count_files(const char *dir, int depth) {
    DIR *dd = depth > 32 ? NULL : opendir(dir);
    if (!dd) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(dd))) {
        if (e->d_name[0] == '.') continue;
        char sub[PATH_MAX];
        if (snprintf(sub, sizeof sub, "%s/%s", dir, e->d_name) >= (int)sizeof sub) continue;
        int type = e->d_type;
        if (type == DT_UNKNOWN || type == DT_LNK) {  // (file systems without types; links: what they point at)
            struct stat st;
            type = stat(sub, &st) != 0 ? DT_UNKNOWN : S_ISDIR(st.st_mode) ? DT_DIR : S_ISREG(st.st_mode) ? DT_REG : DT_UNKNOWN;
        }
        if (type == DT_DIR) n += count_files(sub, depth + 1);
        else if (type == DT_REG) n++;
    }
    closedir(dd);
    return n;
}
int plat_count_files(const char *dir) { return count_files(dir, 0); }

void plat_error_box(const char *msg) {
    fprintf(stderr, "\n*** %s\n", msg);
#ifdef __ANDROID__
    android_error_box(msg);
    return;
#endif
    // Best effort, and only when there is a display server to ask. Children are reaped by SIG_IGN, so this can't
    // leave a zombie or hold the game up when neither helper is installed.
    bool x11 = getenv("DISPLAY") != NULL;
    if (!x11 && !getenv("WAYLAND_DISPLAY")) return;
    static bool reaps;
    if (!reaps) { signal(SIGCHLD, SIG_IGN); reaps = true; }
    extern char **environ;
    char *argv[8];
    pid_t pid;
    if (x11) {
        argv[0] = (char *)"zenity";
        argv[1] = (char *)"--error";
        argv[2] = (char *)"--title";
        argv[3] = (char *)"MHGU Recomp";
        argv[4] = (char *)"--text";
        argv[5] = (char *)msg;
        argv[6] = NULL;
    } else {
        argv[0] = (char *)"kdialog";
        argv[1] = (char *)"--title";
        argv[2] = (char *)"MHGU Recomp";
        argv[3] = (char *)"--error";
        argv[4] = (char *)msg;
        argv[5] = NULL;
    }
    posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ);  // (detached: nobody waits for the box to close)
}

void plat_fatal_notice(void) {
    if (g_own_log)
        plat_error_box("The game stopped because of an error. The details are in log.txt, next to mhgurecomp.");
}

// ---- diagnostics
const char *plat_symbol(uint64_t pc, uint64_t *offset) {
    static __thread char name[PATH_MAX], exe[PATH_MAX];
    Dl_info di;
    if (!dladdr((void *)(uintptr_t)pc, &di) || !di.dli_fname) return NULL;
    if (!exe[0] && !plat_exe_path(exe, sizeof exe)) snprintf(exe, sizeof exe, "-");
    if (!strcmp(di.dli_fname, exe)) return NULL;  // (the caller prints "mhgurecomp+<off>" for the executable itself)
    const char *slash = strrchr(di.dli_fname, '/');
    snprintf(name, sizeof name, "%s", slash ? slash + 1 : di.dli_fname);
    if (offset) *offset = pc - (uint64_t)(uintptr_t)di.dli_fbase;
    return name;
}

const char *rt_host_fn_name(uint64_t pc, uint64_t *offset);  // runtime/dispatch.c

void plat_print_backtrace(void) {
    void *bt[64];
    int n = backtrace(bt, 64);
    Dl_info self;
    uint64_t exe = dladdr((void *)(uintptr_t)&plat_print_backtrace, &self) ? (uint64_t)(uintptr_t)self.dli_fbase : 0;
    for (int i = 0; i < n; i++) {
        uint64_t pc = (uint64_t)(uintptr_t)bt[i], off = 0;
        const char *s = rt_host_fn_name(pc, &off);
        if (!s) s = plat_symbol(pc, &off);
        if (s) fprintf(stderr, "  #%-2d %016llx %s+%llx\n", i, (unsigned long long)pc, s, (unsigned long long)off);
        else fprintf(stderr, "  #%-2d %016llx mhgurecomp+%llx\n", i, (unsigned long long)pc,
                     (unsigned long long)(exe ? pc - exe : 0));
    }
}

static const char *sig_name(int sig) {
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGFPE: return "SIGFPE";
    case SIGILL: return "SIGILL";
    default: return "signal";
    }
}

static void fault_handler(int sig, siginfo_t *si, void *uc) {
    ucontext_t *ctx = (ucontext_t *)uc;
    uint64_t fa = si && si->si_addr ? (uint64_t)(uintptr_t)si->si_addr : 0;
    if ((sig == SIGSEGV || sig == SIGBUS) && lazy_commit(fa)) {  // (a guest page first touched: back it, resume)
        if (plat_touch_hook) plat_touch_hook(fa - g_lo, ctx_write(ctx));
        return;
    }
    uint64_t pc = ctx_pc(ctx), sp = ctx_sp(ctx), lr = 0, end = 0;
#ifdef __aarch64__
    lr = ctx->uc_mcontext.regs[30];  // (the link register; x86-64: the return address a call left on the stack)
    (void)sp, (void)end;
#else
    if (plat_committed(sp, &end) && end - sp >= 8) lr = *(const uint64_t *)(uintptr_t)sp;
#endif
    char what[80];
    snprintf(what, sizeof what, "%s (code %d, addr %llx)", sig_name(sig), si ? si->si_code : 0,
             (unsigned long long)fa);
    rt_fault_report(what, fa, pc, lr);
    _exit(139);  // (rt_fault_report does not return)
}

void plat_crash_handler_install(void) {
    // A guard page under an exhausted stack faults with no room left to run a handler, so run it on its own stack.
    // Signal stacks are per thread: this covers the calling thread, guest threads keep theirs.
    static char altstack[256 * 1024] __attribute__((aligned(16)));  // (SIGSTKSZ is not a compile-time constant here)
    stack_t ss = {.ss_sp = altstack, .ss_size = sizeof altstack}, old;
    if (sigaltstack(NULL, &old) == 0 && !(old.ss_flags & SS_ONSTACK)) sigaltstack(&ss, NULL);
    struct sigaction sa = {0};
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
}
