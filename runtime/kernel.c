// A small Horizon kernel for an AArch32 process: handles, threads, synchronization, memory, and the SVC
// dispatcher. Register usage follows the 32-bit SVC ABI (the SDK's svc stubs: 64-bit values in register pairs,
// fifth and later arguments in r4+).
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "hle.h"
#include "kernel.h"

// ------------------------------------------------------------------ results
#define R_OK_ 0
#define R_TIMEOUT 0xEA01
#define R_CANCELLED 0xEC01
#define R_INVALID_HANDLE 0xE401
#define R_INVALID_STATE 0xFA01
#define R_INVALID_ADDRESS 0xCC01
#define R_NOT_FOUND 0xF201
#define R_INVALID_ENUM 0xF001
#define R_OUT_OF_RESOURCE 0xD001

#define PSEUDO_THREAD 0xFFFF8000u
#define PSEUDO_PROCESS 0xFFFF8001u
#define WAITERS_BIT 0x40000000u

#define TITLE_ID 0x0100770008DD8000ull

pthread_mutex_t g_klock = PTHREAD_MUTEX_INITIALIZER;
static __thread KThread *t_cur;

// ------------------------------------------------------------------ handle table
#define MAX_HANDLES 4096
static KObject *handles[MAX_HANDLES];

static uint32_t handle_alloc(KObject *o) {
    for (int i = 1; i < MAX_HANDLES; i++) {
        if (!handles[i]) {
            handles[i] = o;
            o->refs++;
            return 0x100 + i;
        }
    }
    FATAL("out of handles\n");
}

KObject *handle_get(uint32_t h) {
    if (h == PSEUDO_THREAD) return t_cur ? &t_cur->obj : NULL;
    if (h < 0x101 || h >= 0x100 + MAX_HANDLES) return NULL;
    return handles[h - 0x100];
}

uint32_t kernel_new_handle(KObject *o) {
    pthread_mutex_lock(&g_klock);
    uint32_t h = handle_alloc(o);
    pthread_mutex_unlock(&g_klock);
    return h;
}

static void handle_close(uint32_t h) {
    KObject *o = handle_get(h);
    if (!o || h == PSEUDO_THREAD) return;
    handles[h - 0x100] = NULL;
    o->refs--;
}

// Threads blocked in WaitSynchronization; object signals wake only these (caller holds g_klock).
static KThread *g_syncwaiters;

static void wake(KThread *t) { pthread_cond_signal(&t->cond); }

void kernel_signal(KObject *o) {
    o->signaled = true;
    for (KThread *t = g_syncwaiters; t; t = t->snext) wake(t);
}

// ------------------------------------------------------------------ time
uint64_t rt_ticks(void) {  // 19.2 MHz system tick
    uint64_t ns = plat_now_ns();  // (ns * 12 / 625 without a 128-bit division: called all the time)
    return ns / 625 * 12 + ns % 625 * 12 / 625;
}

static void deadline(struct timespec *ts, int64_t timeout_ns) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += timeout_ns / 1000000000;
    ts->tv_nsec += timeout_ns % 1000000000;
    if (ts->tv_nsec >= 1000000000) { ts->tv_sec++; ts->tv_nsec -= 1000000000; }
}

static pthread_cond_t g_nothread_cond = PTHREAD_COND_INITIALIZER;
__thread uint64_t g_blocked_ns;  // time this thread spent blocked in the kernel (frame pacing)

static bool kwait(int64_t timeout_ns, const struct timespec *ts) {
    pthread_cond_t *cv = t_cur ? &t_cur->cond : &g_nothread_cond;
    uint64_t t0 = plat_now_ns();
    bool r = true;
    if (timeout_ns < 0) pthread_cond_wait(cv, &g_klock);
    else r = pthread_cond_timedwait(cv, &g_klock, ts) == 0;
    g_blocked_ns += plat_now_ns() - t0;
    return r;
}

// ------------------------------------------------------------------ memory map
typedef struct { uint32_t addr, size, type, perm; } Region;
static Region regions[1024];
static int nregions;
static uint32_t heap_size;
static uint32_t tls_next = TLS_BASE;
uint32_t kernel_heap_size(void) { return heap_size; }

static void region_add(uint32_t addr, uint32_t size, uint32_t type, uint32_t perm) {
    if (nregions == 1024) FATAL("too many regions\n");
    plat_commit(GUEST_BASE + addr, size);  // back it before the OS writes there (file reads into the heap)
    int i = nregions++;
    while (i > 0 && regions[i - 1].addr > addr) { regions[i] = regions[i - 1]; i--; }
    regions[i] = (Region){addr, size, type, perm};
}

void kernel_map_region(uint32_t addr, uint32_t size, uint32_t type, uint32_t perm) {
    pthread_mutex_lock(&g_klock);
    region_add(addr, size, type, perm);
    pthread_mutex_unlock(&g_klock);
}

bool kernel_mapped(uint64_t addr, uint32_t size) {
    if (addr + size > GUEST_SPACE || addr + size < addr) return false;
    bool ok = false;
    pthread_mutex_lock(&g_klock);
    for (int i = 0; i < nregions; i++) {
        Region *r = &regions[i];
        if (addr >= r->addr && addr + size <= r->addr + (uint64_t)r->size) { ok = true; break; }
    }
    pthread_mutex_unlock(&g_klock);
    return ok;
}

static void region_remove(uint32_t addr) {
    for (int i = 0; i < nregions; i++) {
        if (regions[i].addr == addr) {
            memmove(&regions[i], &regions[i + 1], (nregions - i - 1) * sizeof(Region));
            nregions--;
            return;
        }
    }
}

// MemoryInfo: u64 addr, u64 size, u32 type, attr, perm, ipc_refs, device_refs, pad (0x28 bytes)
static void query_memory(uint32_t addr, uint32_t out) {
    uint64_t base = 0, size = 0;
    uint32_t type = 0, perm = 0;
    uint64_t prev_end = 0;
    int found = 0;
    for (int i = 0; i < nregions; i++) {
        Region *r = &regions[i];
        if (addr >= r->addr && addr < r->addr + (uint64_t)r->size) {
            base = r->addr; size = r->size; type = r->type; perm = r->perm;
            found = 1;
            break;
        }
        if (addr < r->addr) {
            base = prev_end; size = r->addr - prev_end;
            found = 1;
            break;
        }
        prev_end = (uint64_t)r->addr + r->size;
    }
    if (!found) { base = prev_end; size = GUEST_SPACE - prev_end; }
    uint8_t *o = G2H(out);
    memset(o, 0, 0x28);
    memcpy(o, &base, 8);
    memcpy(o + 8, &size, 8);
    memcpy(o + 16, &type, 4);
    memcpy(o + 24, &perm, 4);
}

// ------------------------------------------------------------------ threads
static uint64_t next_thread_id = 0x50;
volatile int g_rt_unwind;
static KThread *g_threads;

void rt_unwind_point(void) {
    if (__builtin_expect(g_rt_unwind, 0) && t_cur) longjmp(t_cur->unwind, 1);
}

static void *thread_main(void *arg) {
    KThread *t = arg;
    t_cur = t;
    crash_handler_install();
    if (!setjmp(t->unwind)) guest_call(&t->cpu, t->entry);
    pthread_mutex_lock(&g_klock);
    t->finished = true;
    kernel_signal(&t->obj);
    pthread_mutex_unlock(&g_klock);
    t_cur = NULL;
    return NULL;
}

static KThread *thread_new(uint32_t entry, uint32_t arg, uint32_t stack_top, int prio, int core) {
    KThread *t = calloc(1, sizeof *t);
    pthread_cond_init(&t->cond, NULL);
    t->obj.type = KOBJ_THREAD;
    t->entry = entry;
    t->prio = prio;
    t->core = core < 0 ? 0 : core;
    t->id = next_thread_id++;
    t->cpu.r[0] = arg;
    t->cpu.r[13] = stack_top & ~7u;
    t->cpu.excl_addr = ~0u;
    pthread_mutex_lock(&g_klock);
    t->tls = tls_next;
    tls_next += 0x200;
    t->anext = g_threads;
    g_threads = t;
    pthread_mutex_unlock(&g_klock);
    if (t->tls + 0x200 > TLS_BASE + TLS_SIZE) FATAL("out of thread local regions\n");
    memset(G2H(t->tls), 0, 0x200);
    t->cpu.tpidruro = t->tls;
    t->cpu.thread = t;
    return t;
}

static void thread_start(KThread *t) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256ull << 20);  // guest recursion runs on the host stack
    t->started = true;
    pthread_create(&t->host, &attr, thread_main, t);
    pthread_attr_destroy(&attr);
}

KThread *kernel_create_main_thread(uint32_t entry, uint32_t stack_size, int prio) {
    stack_size = (stack_size + 0xFFFF) & ~0xFFFFu;
    uint32_t stack = STACK_BASE + STACK_SIZE - stack_size;
    region_add(stack, stack_size, 0xB, 3);
    KThread *t = thread_new(entry, 0, stack + stack_size, prio, 0);
    t->handle = kernel_new_handle(&t->obj);
    return t;
}

void kernel_run_main(KThread *t, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3) {
    t->cpu.r[0] = r0; t->cpu.r[1] = r1; t->cpu.r[2] = r2; t->cpu.r[3] = r3;
    thread_start(t);
}

uint32_t kernel_current_thread_handle(void) { return t_cur ? t_cur->handle : 0; }
Cpu *kernel_current_cpu(void) { return t_cur ? &t_cur->cpu : NULL; }
KThread *kernel_current_thread(void) { return t_cur; }

// ------------------------------------------------------------------ mutex / condvar (Horizon semantics)
// Mutex word: owner handle | WAITERS_BIT. Waiting threads are handed ownership directly.
static uint32_t *u32p(uint32_t a) { return (uint32_t *)G2H(a); }

static bool mutex_acquire_or_mark(uint32_t m, uint32_t tag) {
    for (;;) {
        uint32_t v = __atomic_load_n(u32p(m), __ATOMIC_SEQ_CST);
        if (v == 0) {
            if (__atomic_compare_exchange_n(u32p(m), &v, tag, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return true;
        } else {
            if ((v & WAITERS_BIT) ||
                __atomic_compare_exchange_n(u32p(m), &v, v | WAITERS_BIT, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
                return false;
        }
    }
}

KThread *g_waitlist;

static void mutex_release(uint32_t addr) {
    KThread *next = NULL;
    int count = 0;
    for (KThread *t = g_waitlist; t; t = t->wnext) {
        if (t->wait_kind == WAIT_MUTEX && t->wait_addr == addr) {
            if (!next || t->prio < next->prio) next = t;
            count++;
        }
    }
    if (!next) {
        __atomic_store_n(u32p(addr), 0, __ATOMIC_SEQ_CST);
        return;
    }
    __atomic_store_n(u32p(addr), next->wait_tag | (count > 1 ? WAITERS_BIT : 0), __ATOMIC_SEQ_CST);
    next->wait_kind = WAIT_NONE;
    next->wait_result = R_OK_;
    wake(next);
}

static void waitlist_add(KThread *t) { t->wnext = g_waitlist; g_waitlist = t; }
static void waitlist_remove(KThread *t) {
    for (KThread **p = &g_waitlist; *p; p = &(*p)->wnext)
        if (*p == t) { *p = t->wnext; return; }
}

static uint32_t block_current(int64_t timeout_ns) {
    KThread *t = t_cur;
    struct timespec ts;
    if (timeout_ns >= 0) deadline(&ts, timeout_ns);
    waitlist_add(t);
    while (t->wait_kind != WAIT_NONE) {
        if (g_rt_unwind) { t->wait_kind = WAIT_NONE; t->wait_result = R_CANCELLED; break; }
        if (t->cancel) { t->cancel = false; t->wait_kind = WAIT_NONE; t->wait_result = R_CANCELLED; break; }
        if (!kwait(timeout_ns, &ts)) { t->wait_kind = WAIT_NONE; t->wait_result = R_TIMEOUT; break; }
    }
    waitlist_remove(t);
    return t->wait_result;
}

static uint32_t svc_arbitrate_lock(uint32_t owner, uint32_t addr, uint32_t tag) {
    pthread_mutex_lock(&g_klock);
    uint32_t v = __atomic_load_n(u32p(addr), __ATOMIC_SEQ_CST);
    if (v != (owner | WAITERS_BIT)) {
        pthread_mutex_unlock(&g_klock);
        return R_OK_;
    }
    KThread *t = t_cur;
    t->wait_kind = WAIT_MUTEX;
    t->wait_addr = addr;
    t->wait_tag = tag;
    uint32_t r = block_current(-1);
    pthread_mutex_unlock(&g_klock);
    return r == R_CANCELLED ? R_OK_ : r;
}

static uint32_t svc_arbitrate_unlock(uint32_t addr) {
    pthread_mutex_lock(&g_klock);
    mutex_release(addr);
    pthread_mutex_unlock(&g_klock);
    return R_OK_;
}

static uint32_t svc_wait_process_wide_key(uint32_t mutex, uint32_t key, uint32_t tag, int64_t timeout) {
    pthread_mutex_lock(&g_klock);
    KThread *t = t_cur;
    mutex_release(mutex);
    t->wait_kind = WAIT_CONDVAR;
    t->wait_addr = key;
    t->wait_mutex = mutex;
    t->wait_tag = tag;
    __atomic_store_n(u32p(key), 1, __ATOMIC_SEQ_CST);
    uint32_t r = block_current(timeout);
    // Timed out: return without the mutex, as Horizon does (nn::os then takes it itself: re-taking it here left the
    // thread waiting on a mutex it owned). Cancelled: hand it back, the caller assumes it holds it.
    if (r != R_OK_ && r != R_TIMEOUT) {
        if (!mutex_acquire_or_mark(mutex, tag)) {
            t->wait_kind = WAIT_MUTEX;
            t->wait_addr = mutex;
            block_current(-1);
        }
    }
    pthread_mutex_unlock(&g_klock);
    return r;
}

static void svc_signal_process_wide_key(uint32_t key, int32_t count) {
    pthread_mutex_lock(&g_klock);
    int woken = 0;
    bool any_left = false;
    for (;;) {
        if (count > 0 && woken >= count) break;
        KThread *best = NULL;
        for (KThread *t = g_waitlist; t; t = t->wnext)
            if (t->wait_kind == WAIT_CONDVAR && t->wait_addr == key && (!best || t->prio < best->prio)) best = t;
        if (!best) break;
        woken++;
        uint32_t m = best->wait_mutex;
        best->wait_result = R_OK_;
        if (mutex_acquire_or_mark(m, best->wait_tag)) {
            best->wait_kind = WAIT_NONE;
            wake(best);
        } else {
            best->wait_kind = WAIT_MUTEX;
            best->wait_addr = m;
        }
    }
    for (KThread *t = g_waitlist; t; t = t->wnext)
        if (t->wait_kind == WAIT_CONDVAR && t->wait_addr == key) any_left = true;
    if (!any_left) __atomic_store_n(u32p(key), 0, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&g_klock);
}

// ------------------------------------------------------------------ address arbiter
enum { ARB_WAIT_LESS = 0, ARB_DEC_WAIT_LESS = 1, ARB_WAIT_EQ = 2 };
enum { ARB_SIGNAL = 0, ARB_SIGNAL_INC_EQ = 1, ARB_SIGNAL_MOD_EQ = 2 };

static uint32_t svc_wait_for_address(uint32_t addr, uint32_t type, int32_t value, int64_t timeout) {
    pthread_mutex_lock(&g_klock);
    int32_t cur = (int32_t)__atomic_load_n(u32p(addr), __ATOMIC_SEQ_CST);
    bool wait;
    if (type == ARB_WAIT_LESS) wait = cur < value;
    else if (type == ARB_DEC_WAIT_LESS) {
        wait = cur < value;
        if (wait) __atomic_store_n(u32p(addr), (uint32_t)(cur - 1), __ATOMIC_SEQ_CST);
    } else if (type == ARB_WAIT_EQ) wait = cur == value;
    else { pthread_mutex_unlock(&g_klock); return R_INVALID_ENUM; }
    if (!wait) { pthread_mutex_unlock(&g_klock); return R_INVALID_STATE; }
    if (timeout == 0) { pthread_mutex_unlock(&g_klock); return R_TIMEOUT; }
    t_cur->wait_kind = WAIT_ARBITER;
    t_cur->wait_addr = addr;
    uint32_t r = block_current(timeout);
    pthread_mutex_unlock(&g_klock);
    return r;
}

static int arbiter_wake(uint32_t addr, int32_t count) {
    int woken = 0;
    for (KThread *t = g_waitlist; t; t = t->wnext) {
        if (count > 0 && woken >= count) break;
        if (t->wait_kind == WAIT_ARBITER && t->wait_addr == addr) {
            t->wait_kind = WAIT_NONE;
            t->wait_result = R_OK_;
            wake(t);
            woken++;
        }
    }
    return woken;
}

static uint32_t svc_signal_to_address(uint32_t addr, uint32_t type, int32_t value, int32_t count) {
    pthread_mutex_lock(&g_klock);
    uint32_t r = R_OK_;
    int32_t cur = (int32_t)__atomic_load_n(u32p(addr), __ATOMIC_SEQ_CST);
    if (type == ARB_SIGNAL) {
        arbiter_wake(addr, count);
    } else if (type == ARB_SIGNAL_INC_EQ) {
        if (cur != value) r = R_INVALID_STATE;
        else { __atomic_store_n(u32p(addr), (uint32_t)(cur + 1), __ATOMIC_SEQ_CST); arbiter_wake(addr, count); }
    } else if (type == ARB_SIGNAL_MOD_EQ) {
        int waiting = 0;
        for (KThread *t = g_waitlist; t; t = t->wnext)
            if (t->wait_kind == WAIT_ARBITER && t->wait_addr == addr) waiting++;
        int32_t nv;
        if (count <= 0) nv = waiting > 0 ? value - 2 : value + 1;
        else nv = waiting == 0 ? value + 1 : (waiting <= count ? value - 1 : value);
        if (cur != value) r = R_INVALID_STATE;
        else { __atomic_store_n(u32p(addr), (uint32_t)nv, __ATOMIC_SEQ_CST); arbiter_wake(addr, count); }
    } else {
        r = R_INVALID_ENUM;
    }
    pthread_mutex_unlock(&g_klock);
    return r;
}

// ------------------------------------------------------------------ wait synchronization
static uint32_t svc_wait_synchronization(uint32_t handles_ptr, int32_t count, int64_t timeout, int32_t *index) {
    pthread_mutex_lock(&g_klock);
    struct timespec ts;
    if (timeout > 0) deadline(&ts, timeout);
    KThread *t = t_cur;
    const uint32_t *hs = (const uint32_t *)G2H(handles_ptr);
    uint32_t r;
    t->sync_handles = handles_ptr;
    t->sync_count = (uint32_t)count;
    for (;;) {
        for (int i = 0; i < count; i++) {
            KObject *o = handle_get(hs[i]);
            if (!o) { r = R_INVALID_HANDLE; goto out; }
            if (o->signaled) { *index = i; r = R_OK_; goto out; }
        }
        if (t->cancel) { t->cancel = false; r = R_CANCELLED; goto out; }
        if (g_rt_unwind) { r = R_CANCELLED; goto out; }
        if (timeout == 0) { r = R_TIMEOUT; goto out; }
        t->in_sync_wait = true;
        t->snext = g_syncwaiters;
        g_syncwaiters = t;
        bool ok = kwait(timeout < 0 ? -1 : timeout, &ts);
        for (KThread **p = &g_syncwaiters; *p; p = &(*p)->snext)
            if (*p == t) { *p = t->snext; break; }
        t->in_sync_wait = false;
        if (!ok) { r = R_TIMEOUT; goto out; }
    }
out:
    pthread_mutex_unlock(&g_klock);
    return r;
}

// ------------------------------------------------------------------ SVC dispatch
static void debug_string(uint32_t p, uint32_t len) {
    fprintf(stderr, "[guest] %.*s", (int)len, gstr(p));
    if (len && gstr(p)[len - 1] != '\n') fputc('\n', stderr);
}

static uint64_t get_info(uint32_t id0, uint32_t handle, uint64_t id1, uint32_t *res) {
    (void)handle;
    *res = R_OK_;
    switch (id0) {
    case 0: return 0xF;                                  // allowed cores
    case 1: return 0xFFFFFFFFFFFFFFFFull;                // allowed priorities
    case 2: return ALIAS_BASE;
    case 3: return ALIAS_SIZE;
    case 4: return HEAP_BASE;
    case 5: return HEAP_MAX;
    // Application memory: the SDK's startup sizes the heap as total - used, so total is what makes that the heap
    // region's size (3.2 GB as on hardware cannot fit a 32-bit address space next to the code and stacks).
    case 6: case 21: return (uint64_t)HEAP_MAX + 0x4000000;
    case 7: case 22: return (uint64_t)heap_size + 0x4000000;
    case 8: return 0;                                    // debugger attached
    case 9: return 0;                                    // resource limit
    case 10: return 0;                                   // idle ticks
    case 11: return ((uint64_t)arc4random() << 32) | arc4random();
    case 12: return MAIN_BASE;                           // address space
    case 13: return GUEST_SPACE - MAIN_BASE;
    case 14: return STACK_BASE;
    case 15: return STACK_SIZE;
    case 16: case 17: return 0;
    case 18: return TITLE_ID;
    case 20: return 0;
    default:
        *res = R_INVALID_ENUM;
        LOG("[kernel] GetInfo unknown id %u/%llu\n", id0, (unsigned long long)id1);
        return 0;
    }
}

// The game's calls that led here, guessed from the stack: words from sp up that point just past a BL / BLX in the
// game's code (a heuristic backtrace; stale words can show up too)
int kernel_stack_callers(uint32_t sp, char *line, size_t size) {
    int n = 0, k = 0;
    line[0] = 0;
    for (uint32_t a = sp; a < sp + 0x600 && k < 10 && n < (int)size - 12; a += 4) {
        uint32_t v = LD32(a);
        if (v < MAIN_BASE + 4 || v >= SDK_BASE + 0x400000 || (v & 3)) continue;
        uint32_t ins = LD32(v - 4);
        if ((ins & 0x0F000000u) != 0x0B000000u && (ins & 0xFE000000u) != 0xFA000000u && (ins & 0x0FFFFFF0u) != 0x012FFF30u)
            continue;
        n += snprintf(line + n, size - n, " %x", v);
        k++;
    }
    return k;
}

// Where the game is: its last return address (with the symbol it is in, when known) and its callers
void kernel_log_guest_stack(const char *tag, const Cpu *c) {
    char line[400];
    uint32_t off = 0;
    const char *s = loader_symbol_at(c->r[14], &off);
    kernel_stack_callers(c->r[13], line, sizeof line);
    LOG("%s: lr %x (%s+%x), callers:%s\n", tag, c->r[14], s ? s : "?", off, line);
}

static int trace_svc = -1;
static void svc_impl(Cpu *c, uint32_t imm, uint32_t pc);

void rt_svc(Cpu *c, uint32_t imm, uint32_t pc) {
    rt_unwind_point();
    if (trace_svc < 0) trace_svc = getenv("MHGU_TRACE_SVC") != NULL;
    if (!trace_svc) {
        svc_impl(c, imm, pc);
    } else {
        uint32_t in[5] = {c->r[0], c->r[1], c->r[2], c->r[3], c->r[4]};
        svc_impl(c, imm, pc);
        LOG("[svc] %02x(%x, %x, %x, %x, %x) -> %x %x %x  @%x lr=%x\n", imm, in[0], in[1], in[2], in[3], in[4],
            c->r[0], c->r[1], c->r[2], pc, c->r[14]);
    }
    rt_unwind_point();
}

static void svc_impl(Cpu *c, uint32_t imm, uint32_t pc) {
    uint32_t *r = c->r;
    switch (imm) {
    case 0x01: {  // SetHeapSize(size r1) -> r1 address
        uint32_t size = r[1];
        if (size > HEAP_MAX) { r[0] = R_OUT_OF_RESOURCE; break; }
        LOG("[kernel] SetHeapSize %#x\n", size);
        if (size > heap_size) plat_commit(GUEST_BASE + HEAP_BASE + heap_size, 0);  // (backed lazily)
        heap_size = size;
        region_remove(HEAP_BASE);
        if (size) {
            int i = nregions++;
            while (i > 0 && regions[i - 1].addr > HEAP_BASE) { regions[i] = regions[i - 1]; i--; }
            regions[i] = (Region){HEAP_BASE, size, 5, 3};
        }
        r[0] = R_OK_;
        r[1] = HEAP_BASE;
        break;
    }
    case 0x02: case 0x03: r[0] = R_OK_; break;  // SetMemoryPermission / SetMemoryAttribute
    case 0x04:  // MapMemory(dst, src, size): src is inaccessible while mapped, so copying is equivalent
        plat_commit(GUEST_BASE + r[0], r[2]);
        memcpy(G2H(r[0]), G2H(r[1]), r[2]);
        region_add(r[0], r[2], 0xB, 3);
        r[0] = R_OK_;
        break;
    case 0x05:  // UnmapMemory(dst, src, size): copy back
        memcpy(G2H(r[1]), G2H(r[0]), r[2]);
        region_remove(r[0]);
        r[0] = R_OK_;
        break;
    case 0x06:  // QueryMemory(info r0, _, addr r2) -> r1 page info
        query_memory(r[2], r[0]);
        r[0] = R_OK_;
        r[1] = 0;
        break;
    case 0x07: kernel_log_guest_stack("ExitProcess (the game quit)", c); exit(0);
    case 0x08: {  // CreateThread(prio r0, entry r1, arg r2, stack_top r3, core r4) -> r1 handle
        KThread *t = thread_new(r[1], r[2], r[3], (int)r[0], (int)r[4]);
        t->handle = kernel_new_handle(&t->obj);
        LOG("[kernel] CreateThread entry=%x arg=%x stack=%x prio=%d core=%d -> %x\n", r[1], r[2], r[3], (int)r[0],
            (int)r[4], t->handle);
        r[0] = R_OK_;
        r[1] = t->handle;
        break;
    }
    case 0x09: {  // StartThread(handle)
        KObject *o = handle_get(r[0]);
        if (!o || o->type != KOBJ_THREAD) { r[0] = R_INVALID_HANDLE; break; }
        thread_start((KThread *)o);
        r[0] = R_OK_;
        break;
    }
    case 0x0A: longjmp(t_cur->unwind, 1);  // ExitThread
    case 0x0B: {  // SleepThread(ns r0:r1)
        int64_t ns = (int64_t)(((uint64_t)r[1] << 32) | r[0]);
        if (ns <= 0) { sched_yield(); break; }
        uint64_t now = plat_now_ns();
        plat_sleep_until_ns(now + (uint64_t)ns);
        g_blocked_ns += plat_now_ns() - now;
        break;
    }
    case 0x0C: {  // GetThreadPriority(_, handle r1) -> r1
        KObject *o = handle_get(r[1]);
        r[0] = o ? R_OK_ : R_INVALID_HANDLE;
        r[1] = o ? (uint32_t)((KThread *)o)->prio : 0;
        break;
    }
    case 0x0D: {  // SetThreadPriority(handle, prio)
        KObject *o = handle_get(r[0]);
        if (o) ((KThread *)o)->prio = (int)r[1];
        r[0] = o ? R_OK_ : R_INVALID_HANDLE;
        break;
    }
    case 0x0E: {  // GetThreadCoreMask(_, _, handle r2) -> r1 core, r2:r3 mask
        KObject *o = handle_get(r[2]);
        r[0] = o ? R_OK_ : R_INVALID_HANDLE;
        r[1] = o ? (uint32_t)((KThread *)o)->core : 0;
        r[2] = 0xF;
        r[3] = 0;
        break;
    }
    case 0x0F: r[0] = R_OK_; break;  // SetThreadCoreMask
    case 0x10: r[0] = t_cur ? (uint32_t)t_cur->core : 0; break;  // GetCurrentProcessorNumber
    case 0x11: case 0x12: case 0x17: {  // SignalEvent / ClearEvent / ResetSignal
        pthread_mutex_lock(&g_klock);
        KObject *o = handle_get(r[0]);
        if (!o || o->type != KOBJ_EVENT) r[0] = R_INVALID_HANDLE;
        else if (imm == 0x11) { kernel_signal(o); r[0] = R_OK_; }
        else if (imm == 0x17 && !o->signaled) r[0] = R_INVALID_STATE;
        else { o->signaled = false; r[0] = R_OK_; }
        pthread_mutex_unlock(&g_klock);
        break;
    }
    case 0x13: {  // MapSharedMemory(handle, addr, size, perm)
        KObject *o = handle_get(r[0]);
        if (!o || o->type != KOBJ_SHMEM) { r[0] = R_INVALID_HANDLE; break; }
        KSharedMem *s = (KSharedMem *)o;
        region_add(r[1], r[2], 6, 3);
        memset(G2H(r[1]), 0, r[2]);
        s->guest_addr = r[1];
        if (s->on_map) s->on_map(s);
        r[0] = R_OK_;
        break;
    }
    case 0x14: region_remove(r[1]); r[0] = R_OK_; break;  // UnmapSharedMemory
    case 0x15: {  // CreateTransferMemory(_, addr r1, size r2, perm r3) -> r1 handle
        KTransferMem *t = calloc(1, sizeof *t);
        t->obj.type = KOBJ_TMEM;
        t->addr = r[1];
        t->size = r[2];
        plat_commit(GUEST_BASE + t->addr, t->size);
        r[0] = R_OK_;
        r[1] = kernel_new_handle(&t->obj);
        break;
    }
    case 0x16:  // CloseHandle
        pthread_mutex_lock(&g_klock);
        handle_close(r[0]);
        pthread_mutex_unlock(&g_klock);
        r[0] = R_OK_;
        break;
    case 0x18: {  // WaitSynchronization(timeout lo r0, handles r1, count r2, timeout hi r3) -> r1 index
        int32_t idx = -1;
        int64_t timeout = (int64_t)(((uint64_t)r[3] << 32) | r[0]);
        r[0] = svc_wait_synchronization(r[1], (int32_t)r[2], timeout, &idx);
        r[1] = (uint32_t)idx;
        break;
    }
    case 0x19: {  // CancelSynchronization
        pthread_mutex_lock(&g_klock);
        KObject *o = handle_get(r[0]);
        if (o && o->type == KOBJ_THREAD) {
            ((KThread *)o)->cancel = true;
            wake((KThread *)o);
            r[0] = R_OK_;
        } else r[0] = R_INVALID_HANDLE;
        pthread_mutex_unlock(&g_klock);
        break;
    }
    case 0x1A: r[0] = svc_arbitrate_lock(r[0], r[1], r[2]); break;
    case 0x1B: r[0] = svc_arbitrate_unlock(r[0]); break;
    case 0x1C: {  // WaitProcessWideKeyAtomic(mutex, key, tag, timeout r3:r4)
        int64_t timeout = (int64_t)(((uint64_t)r[4] << 32) | r[3]);
        r[0] = svc_wait_process_wide_key(r[0], r[1], r[2], timeout);
        break;
    }
    case 0x1D: svc_signal_process_wide_key(r[0], (int32_t)r[1]); r[0] = R_OK_; break;
    case 0x1E: { uint64_t t = rt_ticks(); r[0] = (uint32_t)t; r[1] = (uint32_t)(t >> 32); break; }
    case 0x1F: {  // ConnectToNamedPort(_, name r1) -> r1 handle
        uint32_t h = ipc_connect_named_port(gstr(r[1]));
        r[0] = h ? R_OK_ : R_NOT_FOUND;
        r[1] = h;
        break;
    }
    case 0x21: r[0] = (uint32_t)ipc_handle_request(c, r[0]); break;  // SendSyncRequest
    case 0x22: {  // SendSyncRequestWithUserBuffer(buf, size, handle)
        uint32_t saved = c->tpidruro;
        c->tpidruro = r[0];
        r[0] = (uint32_t)ipc_handle_request(c, r[2]);
        c->tpidruro = saved;
        break;
    }
    case 0x24: r[0] = R_OK_; r[1] = 0x81; r[2] = 0; break;  // GetProcessId
    case 0x25: {  // GetThreadId(_, handle r1) -> r1:r2
        KObject *o = handle_get(r[1]);
        r[0] = o ? R_OK_ : R_INVALID_HANDLE;
        uint64_t id = o ? ((KThread *)o)->id : 0;
        r[1] = (uint32_t)id;
        r[2] = (uint32_t)(id >> 32);
        break;
    }
    case 0x26:  // Break(reason, info, size)
        if (r[0] & 0x80000000u) { LOG("[kernel] Break notification %x\n", r[0]); r[0] = R_OK_; break; }
        FATAL("svcBreak(reason=%x, info=%x, size=%x) at %x\n", r[0], r[1], r[2], pc);
    case 0x27: debug_string(r[0], r[1]); r[0] = R_OK_; break;
    case 0x29: {  // GetInfo(id1 lo r0, id0 r1, handle r2, id1 hi r3) -> r1:r2
        uint32_t res;
        uint64_t v = get_info(r[1], r[2], ((uint64_t)r[3] << 32) | r[0], &res);
        r[0] = res;
        r[1] = (uint32_t)v;
        r[2] = (uint32_t)(v >> 32);
        break;
    }
    case 0x2A: case 0x5F: r[0] = R_OK_; break;  // FlushEntireDataCache / FlushProcessDataCache
    case 0x2C:  // MapPhysicalMemory(addr, size)
        region_add(r[0], r[1], 0x7, 3);
        memset(G2H(r[0]), 0, r[1]);
        r[0] = R_OK_;
        break;
    case 0x2D: region_remove(r[0]); r[0] = R_OK_; break;
    case 0x34: {  // WaitForAddress(addr, type, value, timeout r3:r4)
        int64_t timeout = (int64_t)(((uint64_t)r[4] << 32) | r[3]);
        r[0] = svc_wait_for_address(r[0], r[1], (int32_t)r[2], timeout);
        break;
    }
    case 0x35: r[0] = svc_signal_to_address(r[0], r[1], (int32_t)r[2], (int32_t)r[3]); break;
    case 0x45: {  // CreateEvent -> r1 writable, r2 readable
        KObject *ev = calloc(1, sizeof *ev);
        ev->type = KOBJ_EVENT;
        r[0] = R_OK_;
        r[1] = kernel_new_handle(ev);
        r[2] = kernel_new_handle(ev);
        break;
    }
    default:
        FATAL("unimplemented svc 0x%x at %x (r0=%x r1=%x r2=%x)\n", imm, pc, r[0], r[1], r[2]);
    }
}

// MHGU_DUMP_THREADS=<seconds>: then (and every 10 s after) every guest thread's state: what it waits for and its
// last return address (the lr of its most recent call), to find out what a stalled game waits on.
static void *dump_threads(void *arg) {
    int secs = (int)(intptr_t)arg;
    for (;;) {
        plat_sleep_until_ns(plat_now_ns() + (uint64_t)secs * 1000000000ull);
        secs = 10;
        static const char *const kinds[] = {"running", "mutex", "condvar", "arbiter"};
        pthread_mutex_lock(&g_klock);
        LOG("[threads] ---- dump\n");
        for (KThread *t = g_threads; t; t = t->anext) {
            if (!t->started || t->finished) continue;
            uint32_t off = 0;
            const char *s = loader_symbol_at(t->cpu.r[14], &off);
            LOG("[threads] id %llu handle %x entry %x prio %d %s%s addr %x lr %x (%s+%x) sp %x\n", (unsigned long long)t->id,
                t->handle, t->entry, t->prio, kinds[t->wait_kind & 3], t->in_sync_wait ? " (WaitSynchronization)" : "",
                t->wait_addr, t->cpu.r[14], s ? s : "?", off, t->cpu.r[13]);
            if (t->wait_kind == WAIT_MUTEX && t->wait_addr) LOG("[threads]     mutex word %x\n", LD32(t->wait_addr));
            if (t->in_sync_wait)
                for (uint32_t i = 0; i < t->sync_count && i < 8; i++) LOG("[threads]     handle %x\n", LD32(t->sync_handles + 4 * i));
            if (t->wait_kind != WAIT_NONE || t->in_sync_wait) {
                char line[400];
                kernel_stack_callers(t->cpu.r[13], line, sizeof line);
                LOG("[threads]     callers:%s\n", line);
            }
        }
        pthread_mutex_unlock(&g_klock);
    }
    return NULL;
}

void kernel_init(void) {
    if (getenv("MHGU_DUMP_THREADS")) {
        pthread_t th;
        pthread_create(&th, NULL, dump_threads, (void *)(intptr_t)atoi(getenv("MHGU_DUMP_THREADS")));
    }
    region_add(TLS_BASE, TLS_SIZE, 0xC, 3);
    region_add(HLE_HEAP, HLE_HEAP_SIZE, 4, 3);
    for (int i = 0; i < g_nmodules; i++) {
        Module *m = &g_modules[i];
        region_add(m->base + m->text_off, m->ro_off - m->text_off, 3, 5);
        region_add(m->base + m->ro_off, m->data_off - m->ro_off, 3, 1);
        region_add(m->base + m->data_off, m->size - m->data_off, 4, 3);
    }
}

KEvent *kernel_create_event_pair(uint32_t *w, uint32_t *r) {
    KObject *ev = calloc(1, sizeof *ev);
    ev->type = KOBJ_EVENT;
    *w = kernel_new_handle(ev);
    *r = kernel_new_handle(ev);
    return ev;
}

KSharedMem *kernel_create_shared_memory(uint32_t size, void (*on_map)(KSharedMem *), uint32_t *handle) {
    KSharedMem *s = calloc(1, sizeof *s);
    s->obj.type = KOBJ_SHMEM;
    s->size = size;
    s->on_map = on_map;
    *handle = kernel_new_handle(&s->obj);
    return s;
}
