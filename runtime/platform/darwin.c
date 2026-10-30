// macOS host layer (runtime/platform.h), adapted from runtime/platform/linux.c.
//
// The guest window is reserved PROT_NONE up front and pages are backed on their first touch by the SIGSEGV / SIGBUS
// handler below (64 KB at a time: a multiple of Apple Silicon's 16 KB pages), exactly as linux.c does.
//
// Differences from Linux worth knowing:
// - No MAP_FIXED_NOREPLACE: the window is mapped at a hint address and refused when the kernel puts it elsewhere.
// - A first touch of a guest page shows up in lldb as EXC_BAD_ACCESS: `process handle SIGSEGV SIGBUS -n false -p true
//   -s false` lets the handler back it.
// - No clock_nanosleep: plat_sleep_until_ns sleeps relative to the monotonic clock.
// - Saves go to ~/Library/Application Support/MHGURecomp.
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <execinfo.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/ucontext.h>
#include <time.h>
#include <unistd.h>

#include "../platform.h"

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
                   MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_NORESERVE, -1, 0);
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
    void *p = mmap((void *)(uintptr_t)base, size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return false;
    if (p != (void *)(uintptr_t)base) { munmap(p, size); return false; }  // (the hint was taken: not ours to use)
    g_lo = base;
    g_hi = base + size;
    struct sigaction sa = {0};
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);  // (Darwin reports some protection faults as SIGBUS)
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
uint64_t plat_now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

// macOS lets an ordinary sleep wake late, to coalesce timers: a 12.4 ms nanosleep (the engine's frame limiter, which
// sleeps to ~1 ms before its deadline and spins the rest) woke ~3.1 ms late at the median, so 60 fps came out as 57.
// A kqueue timer with NOTE_CRITICAL opts out of that (measured: 45 us median, 0.7 ms p99). One kqueue per thread,
// closed when the thread ends.
static pthread_key_t kq_key;
static pthread_once_t kq_once = PTHREAD_ONCE_INIT;
static void kq_close(void *p) { close((int)(intptr_t)p - 1); }
static void kq_key_init(void) { pthread_key_create(&kq_key, kq_close); }

static int thread_kqueue(void) {
    pthread_once(&kq_once, kq_key_init);
    int kq = (int)(intptr_t)pthread_getspecific(kq_key) - 1;  // (stored + 1: 0 = none yet)
    if (kq < 0 && (kq = kqueue()) >= 0) pthread_setspecific(kq_key, (void *)(intptr_t)(kq + 1));
    return kq;
}

void plat_sleep_until_ns(uint64_t t) {
    for (;;) {
        uint64_t now = plat_now_ns();
        if (now >= t) return;
        uint64_t left = t - now;
        if (left < 200000) { sched_yield(); continue; }  // (the last bit: spinning beats a syscall)
        left -= 100000;  // (wake a little early, the loop spins the rest)
        int kq = thread_kqueue();
        if (kq >= 0) {
            struct kevent ev;
            EV_SET(&ev, 1, EVFILT_TIMER, EV_ADD | EV_ONESHOT, NOTE_NSECONDS | NOTE_CRITICAL, (int64_t)left, NULL);
            kevent(kq, &ev, 1, &ev, 1, NULL);  // (EINTR: the loop re-reads the clock)
        } else {
            struct timespec d = {(time_t)(left / 1000000000ull), (long)(left % 1000000000ull)};
            nanosleep(&d, NULL);
        }
    }
}

// ---- process
void plat_init(void) {
    // send() on a dropped Local Play connection gives EPIPE here instead of killing the process: Windows reports
    // WSAEPIPE to the caller, which is what nn::socket expects.
    signal(SIGPIPE, SIG_IGN);
    // An app bundle carries its own MoltenVK (tools/make_app.py): the Vulkan loader takes that one only, not also one
    // installed on the system. Set before SDL makes the window: the loader reads its drivers on the first query.
    // An explicit setting in the environment wins.
    char exe[PATH_MAX], icd[PATH_MAX + 64];
    if (plat_in_app_bundle() && plat_exe_path(exe, sizeof exe) && strrchr(exe, '/')) {
        *strrchr(exe, '/') = 0;
        snprintf(icd, sizeof icd, "%s/../Resources/vulkan/icd.d/MoltenVK_icd.json", exe);
        if (access(icd, R_OK) == 0) setenv("VK_DRIVER_FILES", icd, 0);  // (a bundle without one: the system's)
    } else {
        // build/: the loader of make macos-deps searches /usr/local/etc and /usr/local/share, not Homebrew's folder
        // on Apple Silicon: add its MoltenVK to the drivers it finds
        static const char brew_icd[] = "/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json";
        if (access(brew_icd, R_OK) == 0) setenv("VK_ADD_DRIVER_FILES", brew_icd, 0);
    }
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
    char raw[PATH_MAX];
    uint32_t n = sizeof raw;
    if (_NSGetExecutablePath(raw, &n) != 0) return false;
    char real[PATH_MAX];
    if (!realpath(raw, real)) return false;
    if (strlen(real) >= size) return false;
    snprintf(buf, size, "%s", real);
    return true;
}

// Started on its own: run in the executable's folder (MHGU_HOME: in that folder instead); with nobody watching
// stderr (Finder, `open`) the log goes to log.txt and plat_fatal_notice then points the player at it.
static bool g_own_log;

// Inside an app bundle (<name>.app/Contents/MacOS/<exe>, tools/make_app.py)? The game files are in the bundle
// (Contents/MacOS/data -> ../Resources/data), or for a release app in the player's folder (its data/, runtime/main.c),
// and everything the game writes goes to the player's folder: an app's bundle is sealed by its signature, and a
// rebuilt app must not take the player's mods and settings along.
bool plat_in_app_bundle(void) {
    char exe[PATH_MAX];
    return plat_exe_path(exe, sizeof exe) && strstr(exe, ".app/Contents/MacOS/") != NULL;
}

void plat_standalone(void) {
    char exe[PATH_MAX], user[PATH_MAX];
    if (!getenv("MHGU_HOME") && plat_in_app_bundle() && plat_user_dir(user, sizeof user)) setenv("MHGU_HOME", user, 1);
    const char *home = getenv("MHGU_HOME");
    if (home && *home) {
        mkdir(home, 0755);
        if (chdir(home) != 0) { /* the folder we are in will do */ }
    } else if (plat_exe_path(exe, sizeof exe)) {
        char *s = strrchr(exe, '/');
        if (s) { *s = 0; if (chdir(exe) != 0) { /* the folder we are in will do */ } }
    }
    if (!isatty(STDERR_FILENO)) {
        if (freopen("log.txt", "w", stderr)) { setvbuf(stderr, NULL, _IONBF, 0); g_own_log = true; }
    }
}

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

// ~/Library/Application Support/MHGURecomp (the folder the Windows build keeps under %APPDATA%, same layout).
bool plat_user_dir(char *buf, size_t size) {
    const char *home = getenv("HOME");
    if (!home || !*home) return false;
    char dir[PATH_MAX];
    if (snprintf(dir, sizeof dir, "%s/Library/Application Support/MHGURecomp", home) >= (int)sizeof dir) return false;
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
    // Best effort, through osascript: unlike an AppKit alert it works from any thread. Children are reaped by
    // SIG_IGN, so the game is not held up while the alert is open.
    static bool reaps;
    if (!reaps) { signal(SIGCHLD, SIG_IGN); reaps = true; }
    char text[2048];
    size_t o = 0;
    for (const char *s = msg; *s && o + 2 < sizeof text; s++) {  // (an AppleScript string: escape \ and ")
        if (*s == '\\' || *s == '"') text[o++] = '\\';
        text[o++] = *s;
    }
    text[o] = 0;
    char script[2200];
    snprintf(script, sizeof script, "display alert \"MHGU Recomp\" message \"%s\" as critical", text);
    extern char **environ;
    char *argv[] = {(char *)"osascript", (char *)"-e", script, NULL};
    pid_t pid;
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
    char real[PATH_MAX];
    const char *fname = realpath(di.dli_fname, real) ? real : di.dli_fname;
    if (!strcmp(fname, exe)) return NULL;  // (the caller prints "mhgurecomp+<off>" for the executable itself)
    const char *slash = strrchr(fname, '/');
    snprintf(name, sizeof name, "%s", slash ? slash + 1 : fname);
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
#if defined(__arm64__)
        if (plat_touch_hook) plat_touch_hook(fa - g_lo, (ctx->uc_mcontext->__es.__esr >> 6) & 1);  // (ESR WnR)
#else
        if (plat_touch_hook) plat_touch_hook(fa - g_lo, (ctx->uc_mcontext->__es.__err & 2) != 0);
#endif
        return;
    }
#if defined(__arm64__)
    uint64_t pc = (uint64_t)ctx->uc_mcontext->__ss.__pc, lr = (uint64_t)ctx->uc_mcontext->__ss.__lr;
#else
    uint64_t pc = (uint64_t)ctx->uc_mcontext->__ss.__rip, sp = (uint64_t)ctx->uc_mcontext->__ss.__rsp, lr = 0, end = 0;
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
    static char altstack[256 * 1024] __attribute__((aligned(16)));
    stack_t ss = {.ss_sp = altstack, .ss_size = sizeof altstack}, old;
    if (sigaltstack(NULL, &old) == 0 && !(old.ss_flags & SS_ONSTACK)) sigaltstack(&ss, NULL);
    struct sigaction sa = {0};
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
}
