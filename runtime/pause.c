// Pause/Suspend: runtime/pause.h.
#include <stdatomic.h>
#include <stdio.h>

#include "config.h"
#include "net/net.h"
#include "overlay.h"
#include "pause.h"
#include "platform.h"
#include "rt.h"
#include "settings_locale.h"

bool fs_writing(void);  // runtime/services/fs.c: a file open for writing, or a save's changes not committed yet

static atomic_int g_want;  // the window thread asked for a pause (cleared to resume)
static atomic_int g_held;  // the render thread holds

// The guest's clock: a seqlock over the time the game stood still. Only the render thread writes (pause_frame).
static atomic_uint g_seq;
static _Atomic uint64_t g_frozen;  // the guest's time when it stopped (0: running)
static _Atomic uint64_t g_total;   // host time spent paused before now

uint64_t pause_clock_ns(void) {
    for (;;) {
        unsigned s = atomic_load_explicit(&g_seq, memory_order_acquire);
        if (!s) return plat_now_ns();  // (never paused)
        if (s & 1) continue;
        uint64_t now = plat_now_ns();  // (read inside: a time read before a resume would come out earlier than the stop)
        uint64_t f = atomic_load_explicit(&g_frozen, memory_order_relaxed);
        uint64_t t = atomic_load_explicit(&g_total, memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&g_seq, memory_order_relaxed) == s) return f ? f : now - t;
    }
}
static void clock_write(uint64_t frozen, uint64_t total) {
    atomic_fetch_add_explicit(&g_seq, 1, memory_order_acq_rel);
    atomic_store_explicit(&g_frozen, frozen, memory_order_relaxed);
    atomic_store_explicit(&g_total, total, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_seq, 1, memory_order_release);
}

// Why a pause can't start now (English, settings_locale.h translates it); NULL: it can
static const char *refused(void) {
    if (!ldn_offline()) return "Pausing works only offline: not in a hub or with Hunter Search.";
    if (fs_writing()) return "The game is saving. Try again in a moment.";
    return NULL;
}

void pause_toggle(void) {
    if (atomic_load(&g_want)) { pause_resume(); return; }
    const char *why = refused();
    if (why) {
        LOG("[pause] refused: %s\n", why);
        int lang = config_language();
        overlay_notice(settings_text(lang, "Can't pause"), settings_text(lang, why));
        return;
    }
    LOG("[pause] asked\n");
    atomic_store(&g_want, 1);
}
bool pause_resume(void) {
    if (!atomic_exchange(&g_want, 0)) return false;
    LOG("[pause] resume asked\n");
    return true;
}
bool pause_wanted(void) { return atomic_load(&g_want) || atomic_load(&g_held); }
bool pause_held(void) { return atomic_load(&g_held); }

void pause_frame(void *cpu, void (*present)(void *), void *arg) {
    if (!atomic_load(&g_want)) return;
    if (refused()) {  // (went online or started a save in the frame since the key)
        LOG("[pause] dropped: %s\n", refused());
        atomic_store(&g_want, 0);
        return;
    }
    uint64_t t0 = plat_now_ns(), total = atomic_load(&g_total);
    clock_write(t0 - total, total);
    atomic_store(&g_held, 1);
    LOG("[pause] paused\n");
    const uint64_t period = 1000000000ull / 60;  // (the picture again at 60 Hz: the overlay, the settings menu)
    uint64_t next = t0;
    while (atomic_load(&g_want)) {
        overlay_frame(cpu);
        present(arg);
        uint64_t now = plat_now_ns();
        next += period;
        if (next < now) next = now;
        plat_sleep_until_ns(next);
    }
    uint64_t t1 = plat_now_ns();
    clock_write(0, total + (t1 - t0));
    atomic_store(&g_held, 0);
    LOG("[pause] resumed after %.1f s\n", (double)(t1 - t0) / 1e9);
}
