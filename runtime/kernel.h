// Kernel object types shared by kernel.c and the IPC / service layer.
#pragma once
#include <setjmp.h>

#include "rt.h"

typedef enum { KOBJ_THREAD = 1, KOBJ_EVENT, KOBJ_SESSION, KOBJ_SHMEM, KOBJ_TMEM } KType;
typedef enum { WAIT_NONE = 0, WAIT_MUTEX, WAIT_CONDVAR, WAIT_ARBITER } WaitKind;

typedef struct KObject {
    KType type;
    int refs;
    bool signaled;
} KObject;

typedef KObject KEvent;

struct KThread {
    KObject obj;  // signaled when the thread exits
    Cpu cpu;
    pthread_t host;
    uint32_t entry, tls;
    uint64_t id;
    uint32_t handle;
    int prio, core;
    bool cancel, in_sync_wait;
    uint32_t sync_handles, sync_count;  // (debug dump) what WaitSynchronization waits on
    // wait state (protected by g_klock)
    WaitKind wait_kind;
    uint32_t wait_addr, wait_mutex;
    uint32_t wait_tag, wait_result;
    KThread *wnext;
    KThread *snext;        // in WaitSynchronization (g_syncwaiters)
    pthread_cond_t cond;   // each thread sleeps on its own condition; wakers signal it directly
    jmp_buf unwind;        // svcExitThread: back to thread_main
    bool started, finished;
    KThread *anext;        // every thread (g_klock)
};

typedef struct KSharedMem {
    KObject obj;
    uint32_t size, guest_addr;  // guest_addr: where the guest mapped it (0 if not mapped)
    void (*on_map)(struct KSharedMem *s);  // the HLE owner fills initial contents
    void *owner;
} KSharedMem;

KSharedMem *kernel_create_shared_memory(uint32_t size, void (*on_map)(KSharedMem *), uint32_t *handle);

typedef struct {
    KObject obj;
    uint32_t addr, size;
} KTransferMem;

extern pthread_mutex_t g_klock;
extern KThread *g_waitlist;

KObject *handle_get(uint32_t h);
uint32_t kernel_new_handle(KObject *o);
void kernel_signal(KObject *o);  // caller holds g_klock
KEvent *kernel_create_event_pair(uint32_t *writable, uint32_t *readable);
KThread *kernel_current_thread(void);
int kernel_stack_callers(uint32_t sp, char *line, size_t size);  // " addr addr ...": the game's calls, from the stack
int kernel_game_callers(uint32_t sp, char *line, size_t size);   // the same, the game's code (main) only, deeper
void kernel_log_guest_stack(const char *tag, const Cpu *c);       // logs tag, lr and those callers
void kernel_dump_threads(const char *why, bool all_callers);      // every guest thread's state to the log (see kernel.c)
uint32_t kernel_heap_size(void);
