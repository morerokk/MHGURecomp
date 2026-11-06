// Forge plugins (Forge: an MHGU mod loader for code mods, github.com/Fexty12573/forge). A plugin is an AArch32 .nro in
// the RomFS folder nativeNX/plugins/ (so it comes with a mod's romfs, runtime/mods.c); on the console Forge's own
// module (exefs subsdk) loads it and gives it an API: function hooks, code patches, the game's singletons, save data,
// logging, input, an ImGui overlay. The port does not need Forge's module (its hooks rewrite the console's code): it
// loads the plugins itself, runs their code with the interpreter (runtime/interp.c, the plugins call into the
// recompiled game and back) and implements Forge's API on top of the recompiled game, at the points Forge uses:
//   sApp::run          singletons resolved, sockets up, plugins initialized (DT_INIT, DT_INIT_ARRAY), then for each
//                      forge_onLoad, the version check, forge_onInit (a plugin with no forge_onLoad skips these)
//   the frame update   forge_onUpdate(dt) after it, then the ImGui overlay's frame (runtime/overlay.cpp:
//                      forge_onImGuiInit once, forge_onImGuiRender in Forge's window, forge_onImGuiFreeRender)
//   cSystem ctor/dtor  the singletons (forge_singleton_*)
// Hooks run through FN_ENTER (cpu.h, dispatch.c); hooks in a plugin's own code are guest code the interpreter reads.
// Patches over data apply as written; a patch over code can only redirect a function entry (Forge's own hook form,
// `ldr pc, [pc, #-4]` + address, or a `b`): others are logged.
// The plugins' ImGui calls (cimgui's ig* / Im*): runtime/forge_imgui.c.
// forge_socket_*: the game's own nn::socket (a virtual network, runtime/net/vsock.c: local play only, no internet).
// A plugin that requires a Forge version newer than this one (FORGE_MAJOR.FORGE_MINOR) gets no calls. One that can't be
// loaded (a relocation this port doesn't know) is dropped with a log line (runtime/loader.c); the others go on.
#include <ctype.h>
#include <math.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <SDL3/SDL.h>

#include "config.h"
#include "forge.h"
#include "hle.h"
#include "hle/input.h"
#include "interp.h"
#include "mods.h"
#include "overlay.h"

#define FORGE_MAJOR 1
#define FORGE_MINOR 5
#define FORGE_PATCH 1
#define FORGE_VERSION_STRING "1.5.1"

// Main module offsets (from its text) Forge uses, MHGU 1.4.0
#define OFF_APP_RUN     0xB8692Cu  // sApp::run
#define OFF_UPDATE      0x3DA8FCu  // the frame update (this: dt at +0x68)
#define OFF_SYSTEM_CTOR 0x887810u  // cSystem::cSystem / ~cSystem: every singleton
#define OFF_SYSTEM_DTOR 0x88784Cu
#define OFF_MOUNT_SAVE  0x878DA0u  // sSavedata's mount
#define OFF_JOB_SAFE    0x212041Cu

#define MAX_PLUGINS (MAX_MODULES - 2)  // g_modules also holds main and sdk
typedef struct {
    char name[256];
    Module *m;
    uint32_t on_load, on_init, on_update;
    uint32_t on_imgui[3];  // forge_onImGuiInit, forge_onImGuiRender, forge_onImGuiFreeRender (FORGE_IMGUI_*)
    uint32_t param;  // PluginInitParam: u16 major, minor, patch; char name[64]
    bool active;  // not rejected (a plugin without forge_onLoad is active too)
} Plugin;
static Plugin plugins[MAX_PLUGINS];
static int nplugins;
static uint32_t main_text;
static int log_level = 1;  // Forge LogLevel: 0 debug, 1 info, 2 warn, 3 error (MHGU_FORGE_LOG)

static uint32_t text_addr(uint32_t off) { return main_text + off; }

// A symbol of the game's modules, or 0 (logged once): the SDK of a build this port doesn't know may lack it, and a
// plugin's call then fails instead of the process (guest_sym is fatal)
static uint32_t gsym(const char *name) {
    static const char *missing[16];
    uint32_t a = loader_find_symbol(name);
    if (a) return a;
    for (int i = 0; i < 16; i++) {
        if (missing[i] == name) return 0;
        if (!missing[i]) { missing[i] = name; break; }
    }
    LOG("[forge] the game has no %s: the Forge call that needs it fails\n", name);
    return 0;
}

// a guest call from host code with up to four word arguments (r0-r3 are scratch here: callers are hooks or HLE
// functions, at a call boundary of the guest)
static uint32_t gcall(Cpu *c, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    if (!fn) return 0;
    c->r[0] = a0, c->r[1] = a1, c->r[2] = a2, c->r[3] = a3;
    guest_call(c, fn);
    return c->r[0];
}

// A string of the guest copied into buf (at most n - 1 bytes), whatever pointer it was given: it stops where mapped
// memory ends (a plugin's bad pointer is not the port's crash). NULL is "".
static const char *gstr_copy(uint32_t p, char *buf, size_t n) {
    size_t i = 0;
    for (; p && i + 1 < n; i++) {
        if ((i == 0 || !((p + i) & 0xFFF)) && !kernel_mapped(p + (uint32_t)i, 1)) break;
        char ch = (char)LD8(p + (uint32_t)i);
        if (!ch) break;
        buf[i] = ch;
    }
    buf[i] = 0;
    return buf;
}

// Forge's own messages, at its levels (0 debug .. 3 error; its forge_log: a message below the level is dropped)
static void flog(int level, const char *fmt, ...) {
    if (level < log_level) return;
    static const char *names[] = {"DEBUG", "INFO", "WARN", "ERROR"};
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    LOG("[forge] [%s] %s\n", names[level & 3], buf);
}

// ------------------------------------------------------------------ bad guest accesses by plugins
// What the interpreter (interp.c) and the fault handler (platform.h plat_touch_hook) report of a plugin's code: a
// load / store at the null page, at an unmapped address that looks like a float, or on the first touch of an unmapped
// page (the port backs it with zeros, where the console faults). One WARN line per (plugin, pc), at most FAULT_CAP per
// plugin, then one line saying the rest are dropped. The lock is only taken on a report, never on a mapped access.
#define FAULT_CAP 20
#define FAULT_SITES 128
static struct { uint32_t pc[FAULT_SITES]; int nsites, lines; bool capped; } pfault[MAX_MODULES];
static pthread_mutex_t fault_lock = PTHREAD_MUTEX_INITIALIZER;

static const char *plugin_name_of(const Module *m) {
    for (int i = 0; i < nplugins; i++)
        if (plugins[i].m == m) return plugins[i].name;
    return m->name;
}

void forge_guest_fault(uint32_t pc, uint32_t addr, int size, int write, const char *why) {
    if (log_level > 2) return;
    int mi = -1;
    for (int i = 2; i < g_nmodules; i++)
        if (pc >= g_modules[i].base && pc < g_modules[i].base + g_modules[i].size) { mi = i; break; }
    if (mi < 0) return;
    const Module *m = &g_modules[mi];
    pthread_mutex_lock(&fault_lock);
    __typeof__(pfault[0]) *f = &pfault[mi];
    bool known = false;
    for (int i = 0; i < f->nsites; i++) known |= f->pc[i] == pc;
    if (known || f->capped) { pthread_mutex_unlock(&fault_lock); return; }
    if (f->nsites < FAULT_SITES) f->pc[f->nsites++] = pc;
    bool last = ++f->lines >= FAULT_CAP || f->nsites == FAULT_SITES;
    if (last) f->capped = true;
    pthread_mutex_unlock(&fault_lock);
    const char *name = plugin_name_of(m);
    char acc[24];
    if (size) snprintf(acc, sizeof acc, "%d-byte", size);
    else snprintf(acc, sizeof acc, "%s", "");
    const Cpu *c = g_ip_cpu;
    char regs[96] = "";
    if (c) snprintf(regs, sizeof regs, " r0=%X r1=%X r2=%X r3=%X lr=%X", c->r[0], c->r[1], c->r[2], c->r[3], c->r[14]);
    flog(2, "%s: %s%s%s at 0x%08X (%s) by code at +0x%X (pc %08X)%s", name, acc, acc[0] ? " " : "",
         write < 0 ? "access" : write ? "write" : "read", addr, why, pc - m->base, pc, regs);
    if (last) flog(2, "%s: further memory warnings from this plugin are suppressed", name);
}

static void touch_hook(uint64_t ga, int write) {
    if (!g_ip_pc || kernel_mapped(ga, 1)) return;
    forge_guest_fault(g_ip_pc, (uint32_t)ga, 0, write, "unmapped page");
}

static uint32_t gstring(const char *s) {  // a constant string in guest memory
    size_t n = strlen(s) + 1;
    uint32_t a = hle_alloc((uint32_t)n);
    memcpy(G2H(a), s, n);
    return a;
}

// ------------------------------------------------------------------ singletons
// Every cSystem the game makes is noted when its constructor starts (before it has its vtable), and typed once it
// is complete: at sApp::run all of those are (the game's start-up is over), later ones a frame after they were
// noted, and only if what they hold looks like a finished object (a vtable in the game's data whose getDti is code).
#define MAX_SINGLETONS 1024
static struct { uint32_t obj, frame, tries; } pending[MAX_SINGLETONS];
static uint32_t npending;
static struct { uint32_t id, obj; } singletons[MAX_SINGLETONS];
static uint32_t nsingletons;
static pthread_mutex_t singleton_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t frame_no;  // frames updated so far (frame_update)

static uint32_t mt_id(const char *name) {  // MtDti::makeId: CRC-32 without the final inversion, 31 bits
    uint32_t crc = 0xFFFFFFFFu;
    for (; *name; name++) crc = rt_crc32_tab[(crc ^ (uint8_t)*name) & 0xFF] ^ (crc >> 8);
    return crc & 0x7FFFFFFFu;
}

static bool in_code(uint32_t a);
static const Module *plugin_code(uint32_t a);
static const char *plugin_name_of(const Module *m);

static void system_ctor(Cpu *c) {
    pthread_mutex_lock(&singleton_lock);
    if (npending < MAX_SINGLETONS) {
        pending[npending].obj = c->r[0];
        pending[npending].tries = 0;
        pending[npending++].frame = __atomic_load_n(&frame_no, __ATOMIC_RELAXED);
    } else {
        LOG("[forge] too many singletons are being made: one is not noted\n");
    }
    pthread_mutex_unlock(&singleton_lock);
    fnhook_call_original(c, text_addr(OFF_SYSTEM_CTOR));
}

static void system_dtor(Cpu *c) {
    uint32_t obj = c->r[0];
    pthread_mutex_lock(&singleton_lock);
    for (uint32_t i = 0; i < npending; i++)
        if (pending[i].obj == obj) pending[i--] = pending[--npending];
    for (uint32_t i = 0; i < nsingletons; i++)
        if (singletons[i].obj == obj) singletons[i--] = singletons[--nsingletons];
    pthread_mutex_unlock(&singleton_lock);
    fnhook_call_original(c, text_addr(OFF_SYSTEM_DTOR));
}

// MtObject::getDti of a finished object, or 0: the object is not one (yet)
static uint32_t object_dti(Cpu *c, uint32_t obj) {
    if (!kernel_mapped(obj, 4)) return 0;
    uint32_t vt = LD32(obj);
    if (!vt || !kernel_mapped(vt + 0x14, 4)) return 0;
    uint32_t fn = LD32(vt + 0x14);
    if (!in_code(fn)) return 0;
    uint32_t dti = gcall(c, fn, obj, 0, 0, 0);
    return dti && kernel_mapped(dti, 0x20) ? dti : 0;
}

// An object that is not finished is asked again a frame later, at most this many times (it may never be one)
#define MAX_TYPE_TRIES 120

// Type the noted objects that are complete (all = at sApp::run, else the ones a frame old). The others stay noted.
static void singletons_resolve(Cpu *c, bool all) {
    uint32_t list[MAX_SINGLETONS], n = 0, now = __atomic_load_n(&frame_no, __ATOMIC_RELAXED);
    pthread_mutex_lock(&singleton_lock);
    for (uint32_t i = 0; i < npending; i++)
        if (all || pending[i].frame < now) list[n++] = pending[i].obj;
    pthread_mutex_unlock(&singleton_lock);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t dti = object_dti(c, list[i]);
        if (!dti) {  // (not finished: not asked again before the next frame, nor for ever)
            pthread_mutex_lock(&singleton_lock);
            for (uint32_t j = 0; j < npending; j++)
                if (pending[j].obj == list[i]) {
                    pending[j].frame = now;
                    if (!all && ++pending[j].tries >= MAX_TYPE_TRIES) pending[j] = pending[--npending];
                    break;
                }
            pthread_mutex_unlock(&singleton_lock);
            continue;
        }
        uint32_t id = LD32(dti + 0x1C);
        pthread_mutex_lock(&singleton_lock);
        uint32_t k = 0, j = 0;
        while (j < npending && pending[j].obj != list[i]) j++;
        if (j == npending) { pthread_mutex_unlock(&singleton_lock); continue; }  // (destroyed meanwhile)
        pending[j] = pending[--npending];
        while (k < nsingletons && singletons[k].id != id) k++;
        if (k < MAX_SINGLETONS) {
            singletons[k].id = id;
            singletons[k].obj = list[i];
            if (k == nsingletons) nsingletons++;
        }
        pthread_mutex_unlock(&singleton_lock);
        char name[64];
        if (log_level == 0) {
            if (LD32(dti + 4)) flog(0, "Resolved Singleton: %s @ 0x%X", gstr_copy(LD32(dti + 4), name, sizeof name), list[i]);
            else flog(0, "Resolved Singleton: 0x%08X @ 0x%X", id, list[i]);
        }
    }
}

static uint32_t singleton_by_id(uint32_t id) {
    pthread_mutex_lock(&singleton_lock);
    uint32_t r = 0;
    for (uint32_t i = 0; i < nsingletons; i++)
        if (singletons[i].id == id) r = singletons[i].obj;
    pthread_mutex_unlock(&singleton_lock);
    return r;
}

static uint32_t singleton(Cpu *c, uint32_t id) {  // (singletons made since sApp::run: typed here once they are finished)
    uint32_t r = singleton_by_id(id);
    if (!r && __atomic_load_n(&npending, __ATOMIC_RELAXED)) {
        singletons_resolve(c, false);
        r = singleton_by_id(id);
    }
    return r;
}
HLE_FUNC("forge_singleton_getInstanceByName", f_singleton_by_name) {
    char name[256];
    RET(ARG(0) && kernel_mapped(ARG(0), 1) ? singleton(c, mt_id(gstr_copy(ARG(0), name, sizeof name))) : 0);
}
HLE_FUNC("forge_singleton_getInstanceById", f_singleton_by_id) { RET(singleton(c, ARG(0))); }
HLE_FUNC("forge_singleton_getAllInstances", f_singleton_all) {
    uint32_t out = ARG(0), max = ARG(1), n = 0;
    if (__atomic_load_n(&npending, __ATOMIC_RELAXED)) singletons_resolve(c, false);  // (made since sApp::run)
    pthread_mutex_lock(&singleton_lock);
    if (!out) n = nsingletons;
    else if (max < 0x10000000u && kernel_mapped(out, 4 * max))
        for (; n < nsingletons && n < max; n++) ST32(out + 4 * n, singletons[n].obj);
    pthread_mutex_unlock(&singleton_lock);
    RET(n);
}

// ------------------------------------------------------------------ hooks and patches
// A hook makes the target's first two words Forge's `ldr pc, [pc, #-4]; .word detour` (plugins check that) and
// installs the detour as a function hook; the original is a trampoline holding the two original words and a jump
// back, as Forge builds it (plugins check that too), which dispatch.c runs as the unhooked function.
static uint32_t tramp_next = FORGE_TRAMP, stub_next = FORGE_STUBS;
static pthread_mutex_t forge_lock = PTHREAD_MUTEX_INITIALIZER;
static __thread uint32_t t_hook_context;

static bool in_code(uint32_t a) {
    for (int i = 0; i < 2 && i < g_nmodules; i++) {
        Module *m = &g_modules[i];
        if (a >= m->base + m->text_off && a < m->base + m->text_off + m->text_size) return true;
    }
    return false;
}

// The word at x (aligned) as it was before a patch of n bytes at a, whose old bytes are at orig: the patch's bytes of the
// word come from orig, the others are memory's (a patch need not start or end on a word).
static uint32_t word_before(uint32_t x, uint32_t a, uint32_t n, uint32_t orig) {
    uint32_t w = LD32(x);
    for (uint32_t k = 0; k < 4; k++)
        if (x + k >= a && x + k - a < n) w = (w & ~(0xFFu << (8 * k))) | ((uint32_t)LD8(orig + (x + k - a)) << (8 * k));
    return w;
}

// Code patched in guest memory: re-derive the hooks at the function entries in [a, a + n). orig: the bytes before
// (enabling a patch), to report a change that is not a hook.
static void code_changed(uint32_t a, uint32_t n, uint32_t orig) {
    bool hook = false;
    for (uint32_t x = a & ~3u; x < a + n; x += 4) {
        if (!in_code(x) || !dispatch_lookup(x)) continue;
        uint32_t w = LD32(x);
        if (orig && (w == 0xE51FF004u || (w & 0xFF000000u) == 0xEA000000u))  // (the words it replaced)
            fnhook_keep_entry(x, word_before(x, a, n, orig), word_before(x + 4, a, n, orig));
        if (w == 0xE51FF004u) hook = fnhook_set(x, LD32(x + 4));
        else if ((w & 0xFF000000u) == 0xEA000000u) hook = fnhook_set(x, x + 8 + (uint32_t)((int32_t)(w << 8) >> 6));
        else if (fnhook_get(x)) fnhook_set(x, 0);
    }
    if (!hook && orig && memcmp(G2H(a), G2H(orig), n)) {
        static uint32_t seen[64];  // (once per site: a menu toggle repeats the patch)
        static int nseen;
        bool dup = false;
        for (int i = 0; i < nseen; i++) dup |= seen[i] == a;
        if (!dup) {
            if (nseen < 64) seen[nseen++] = a;
            const Module *pm = plugin_code(g_ip_last);
            flog(2, "%s: code patch at main+0x%X (%u bytes) cannot apply to recompiled code: ignored",
                 pm ? plugin_name_of(pm) : "a plugin", a - main_text, n);
        }
    }
}

// A slot of n bytes in the trampoline or stub region (*next .. end); 0 and a log line when the region is full
static uint32_t slot_alloc(uint32_t *next, uint32_t end, uint32_t n, const char *what) {
    uint32_t a = 0;
    pthread_mutex_lock(&forge_lock);
    if (*next + n <= end) {
        a = *next;
        *next += n;
    }
    pthread_mutex_unlock(&forge_lock);
    if (!a) flog(3, "No room left for %s: the hook is not created", what);
    return a;
}

// Give back a slot that nothing uses (the last one taken; an earlier one stays, there is no free list for 16 bytes)
static void slot_release(uint32_t *next, uint32_t a, uint32_t n) {
    pthread_mutex_lock(&forge_lock);
    if (*next == a + n) *next = a;
    pthread_mutex_unlock(&forge_lock);
}

// the original of a hook at target: its two words, then a jump back after them (words that read pc would not survive
// the move; Forge's trampoline has the same limit)
static uint32_t make_trampoline(uint32_t slot, uint32_t target) {
    ST32(slot, LD32(target));
    ST32(slot + 4, LD32(target + 4));
    ST32(slot + 8, 0xE51FF004u);  // ldr pc, [pc, #-4]
    ST32(slot + 12, target + 8);
    return slot;
}

// The plugin module whose code holds a, if any (the game's own are modules 0 and 1)
static const Module *plugin_code(uint32_t a) {
    for (int i = 2; i < g_nmodules; i++) {
        const Module *m = &g_modules[i];
        if (a >= m->base + m->text_off && a < m->base + m->text_off + m->text_size) return m;
    }
    return NULL;
}

// A hook inside a function (not at its entry; Forge writes its jump wherever it is asked to): the jump goes into guest
// memory as on the console, and the function holding it runs in the interpreter from then on (dispatch_interpret, as
// for code a cheat changes), which reads that code. The original is a trampoline in interpreted memory.
static bool hook_inside(uint32_t target, uint32_t detour, bool want_orig, uint32_t *tramp) {
    const Module *m = &g_modules[target >= SDK_BASE];
    uint32_t next, entry = dispatch_function_at(target, &next);
    uint32_t end = next ? next : m->base + m->text_off + m->text_size;
    if (!entry || target + 8 > end) {
        flog(3, "Failed to hook %08x: it is not inside a function of the game", target);
        return false;
    }
    uint32_t slot = 0;
    if (want_orig && !(slot = slot_alloc(&stub_next, FORGE_END, 16, "trampolines"))) return false;
    pthread_mutex_lock(&forge_lock);
    uint32_t w0 = LD32(target), w1 = LD32(target + 4);
    if (slot) make_trampoline(slot, target);
    ST32(target, 0xE51FF004u);
    ST32(target + 4, detour);
    pthread_mutex_unlock(&forge_lock);
    int r = fnhook_room(entry, false) ? dispatch_interpret(entry, end) : -1;
    if (r < 0) {  // (nothing can run that code: the words are put back, as if no hook had been asked for)
        pthread_mutex_lock(&forge_lock);
        ST32(target, w0);
        ST32(target + 4, w1);
        pthread_mutex_unlock(&forge_lock);
        if (slot) slot_release(&stub_next, slot, 16);
        flog(3, "Failed to hook %08x: its function (%s+%x) can't run in the interpreter", target, m->name, entry - m->base);
        return false;
    }
    LOG("[forge] hook at %s+%x, inside the function at %s+%x (which runs in the interpreter now) -> %08x\n",
        m->name, target - m->base, m->name, entry - m->base, detour);
    *tramp = slot;
    return true;
}

// A hook in a plugin's own code (a plugin hooking another's function, or its own): the code is interpreted from guest
// memory, so the jump written there is all it takes.
static bool hook_in_plugin(const Module *m, uint32_t target, uint32_t detour, bool want_orig, uint32_t *tramp) {
    if (target + 8 > m->base + m->text_off + m->text_size) {
        flog(3, "Failed to hook %08x: less than two words from the end of %s's code", target, m->name);
        return false;
    }
    uint32_t slot = 0;
    if (want_orig && !(slot = slot_alloc(&stub_next, FORGE_END, 16, "trampolines"))) return false;
    pthread_mutex_lock(&forge_lock);
    if (slot) make_trampoline(slot, target);
    ST32(target, 0xE51FF004u);
    ST32(target + 4, detour);
    pthread_mutex_unlock(&forge_lock);
    LOG("[forge] hook at %s+%x -> %08x\n", m->name, target - m->base, detour);
    *tramp = slot;
    return true;
}

// false: nothing was hooked (logged), whatever the caller's original pointer says; *tramp: the original (0: none wanted)
static bool hook_install(uint32_t target, uint32_t detour, bool want_orig, uint32_t *tramp) {
    *tramp = 0;
    if (!target || !detour) {
        flog(3, "Invalid parameters: target=%p, detour=%p", (void *)(uintptr_t)target, (void *)(uintptr_t)detour);
        return false;
    }
    if (target & 3) {  // (Forge hooks Thumb code too: the interpreter runs ARM only)
        flog(3, "Failed to hook %08x: it is not a word-aligned ARM address (Thumb is not supported)", target);
        return false;
    }
    const Module *pm = plugin_code(target);
    if (pm) return hook_in_plugin(pm, target, detour, want_orig, tramp);
    if (in_code(target) && !dispatch_lookup(target)) return hook_inside(target, detour, want_orig, tramp);
    if (!in_code(target)) {
        flog(3, "Failed to hook %08x: it is not in the code of the game or a plugin", target);
        return false;
    }
    uint32_t next;
    dispatch_function_at(target, &next);
    if (next && next < target + 8) {  // (the jump would overwrite the next function)
        flog(3, "Failed to hook %08x: the function after it starts at %08x, under two words on", target, next);
        return false;
    }
    if (!fnhook_room(target, want_orig)) {
        flog(3, "Failed to hook %08x: too many hooks (the game's function hooks, trampolines and entries are 1024 each)", target);
        return false;
    }
    uint32_t slot = 0;
    if (want_orig && !(slot = slot_alloc(&tramp_next, FORGE_STUBS, 16, "trampolines"))) return false;
    pthread_mutex_lock(&forge_lock);
    uint32_t prev = fnhook_get(target);  // (a hook already there: the original runs it)
    if (slot) {
        make_trampoline(slot, target);
        if (!fnhook_add_trampoline(slot, prev ? prev : target, !prev)) {
            pthread_mutex_unlock(&forge_lock);
            flog(3, "Failed to hook %08x: too many trampolines", target);
            return false;
        }
    }
    fnhook_keep_entry(target, LD32(target), LD32(target + 4));  // (interpreted, the function runs its own words)
    ST32(target, 0xE51FF004u);
    ST32(target + 4, detour);
    pthread_mutex_unlock(&forge_lock);
    if (!fnhook_set(target, detour)) {  // (the room was checked: only a race can get here)
        flog(3, "Failed to hook %08x", target);
        return false;
    }
    if (log_level == 0) LOG("[forge] hook main+%x -> %08x\n", target - main_text, detour);
    *tramp = slot;
    return true;
}

// (debug) Hooks made the way a plugin makes them: MHGU_FORGE_TESTHOOK=<runtime hex address>[,<address>...] hooks there
// at start-up, before the cheats are applied; the control file's "testhook <address>" while the game runs, after them
// (as plugins hook, from forge_onLoad). Each detour is guest code (interpreted), as a plugin's: it logs its first calls
// (an HLE call between saved scratch registers), then at a function's entry calls the original through its trampoline
// and returns (it pushes two words: not for functions with arguments on the stack), inside a function (hook_inside)
// jumps on to it (the jump in is not a call). An entry and an inside hook on one function is what Damage Numbers does.
static void forge_map_regions(void);
#define MAX_TEST_HOOKS 4
static struct { uint32_t target; int calls; } tests[MAX_TEST_HOOKS];
static int ntests;

static void test_count(Cpu *c) {
    int i = (int)c->r[0];
    if (i < 0 || i >= ntests) return;
    int n = ++tests[i].calls;
    if (n <= 5 || !(n % 1000)) LOG("[forge] test hook %08x: call %d\n", tests[i].target, n);
}

void forge_test_hook(uint32_t target) {
    target &= ~1u;
    if (!target || ntests == MAX_TEST_HOOKS) return;
    for (int i = 0; i < ntests; i++)
        if (tests[i].target == target) return;
    int i = ntests++;
    tests[i].target = target;
    forge_map_regions();  // (no plugins: not mapped yet)
    bool entry = dispatch_lookup(target) != NULL;
    uint32_t n = entry ? 11 : 8;
    uint32_t stub = slot_alloc(&stub_next, FORGE_END, 4 * n, "the test hook");
    if (!stub) { ntests--; return; }
    // push {r0-r3, r12, lr}; mov r0, #i; ldr r12, =log; blx r12; pop {r0-r3, r12, lr}; then
    //   entry:  push {r4, lr}; ldr r12, =trampoline; blx r12; pop {r4, pc}; trampoline; log
    //   inside: ldr pc, =trampoline; trampoline; log
    uint32_t code[8] = {0xE92D500F, 0xE3A00000u | (uint32_t)i, entry ? 0xE59FC018 : 0xE59FC00C, 0xE12FFF3C, 0xE8BD500F};
    if (entry) code[5] = 0xE92D4010, code[6] = 0xE59FC004, code[7] = 0xE12FFF3C;
    else code[5] = 0xE51FF004;
    for (uint32_t k = 0; k < (entry ? 8 : 6); k++) ST32(stub + 4 * k, code[k]);
    if (entry) ST32(stub + 32, 0xE8BD8010);
    ST32(stub + 4 * (n - 1), hle_register("forge:test hook", test_count));
    uint32_t tramp;
    if (!hook_install(target, stub, true, &tramp)) { ntests--; return; }
    ST32(stub + 4 * (n - 2), tramp);
    LOG("[forge] test hook %08x (trampoline %08x)\n", target, tramp);
}

static void setcontext(Cpu *c) { t_hook_context = ARG(0); }

// Hook (24 bytes): target, detour, original, context, context cell, has_context (and padding). A failed hook comes
// back with the target and detour and no original (as Forge's does: 0), nothing hooked.
#define HOOK_SIZE 24
static bool hook_args(const char *fn, uint32_t out, uint32_t orig) {
    if (!kernel_mapped(out, HOOK_SIZE) || (orig && !kernel_mapped(orig, 4))) {
        LOG("[forge] %s: the result or the original pointer is not valid memory: not hooked\n", fn);
        return false;
    }
    memset(G2H(out), 0, HOOK_SIZE);
    if (orig) ST32(orig, 0);
    return true;
}

// Hook forge_hook_create(void *target, void *detour, void **original): returned in memory (r0)
HLE_FUNC("forge_hook_create", f_hook_create) {
    uint32_t out = ARG(0), target = ARG(1), detour = ARG(2), orig = ARG(3), tramp;
    if (!hook_args("forge_hook_create", out, orig)) return;
    ST32(out, target), ST32(out + 4, detour);
    if (!hook_install(target, detour, orig != 0, &tramp)) return;
    ST32(out + 8, tramp);
    if (orig) ST32(orig, tramp);
}

// The context stub of forge_hook_createWithContext: Forge's own ARM code (interpreted), which stores the context
// for forge_hook_getContext and goes on to the detour.
HLE_FUNC("forge_hook_createWithContext", f_hook_create_ctx) {
    uint32_t out = ARG(0), target = ARG(1), detour = ARG(2), orig = ARG(3), context = STK(0), tramp;
    if (!hook_args("forge_hook_createWithContext", out, orig)) return;
    ST32(out, target), ST32(out + 4, detour), ST32(out + 12, context), ST8(out + 20, 1);
    uint32_t stub = slot_alloc(&stub_next, FORGE_END, 48, "the context stubs");
    if (!stub) return;
    static const uint32_t code[7] = {0xE92D500F, 0xE59F0010, 0xE5900000, 0xE59FC00C, 0xE12FFF3C, 0xE8BD500F, 0xE59FF004};
    for (int i = 0; i < 7; i++) ST32(stub + 4 * i, code[i]);
    ST32(stub + 28, stub + 40);
    ST32(stub + 32, hle_register("forge_hook_setContextInternal", setcontext));
    ST32(stub + 36, detour);
    ST32(stub + 40, context);
    ST32(out + 16, stub + 40);
    if (!hook_install(target, stub, orig != 0, &tramp)) return;
    ST32(out + 8, tramp);
    if (orig) ST32(orig, tramp);
}

HLE_FUNC("forge_hook_updateContext", f_hook_update_ctx) {
    uint32_t h = ARG(0);
    // (libnx: Module_Libnx 345, LibnxError_BadInput 11)
    if (!kernel_mapped(h, HOOK_SIZE) || !LD8(h + 20) || !LD32(h + 16) || !kernel_mapped(LD32(h + 16), 4)) {
        RET(MAKE_RESULT(345, 11));
        return;
    }
    __atomic_store_n((uint32_t *)G2H(LD32(h + 16)), ARG(1), __ATOMIC_RELEASE);
    ST32(h + 12, ARG(1));
    RET(0);
}

HLE_FUNC("forge_hook_getContext", f_hook_get_ctx) { RET(t_hook_context); }

// Patch: u32 address, size, void *patch_bytes, *original_bytes; bool enabled. The two buffers come from a pool of
// guest memory (every buffer made is listed, in use or free) that takes them back when the patch is destroyed; a
// Patch put together by hand, with pointers that are not the pool's, gives the pool nothing.
#define PATCH_SIZE 20
#define MAX_PATCH 0x100000u
typedef struct { uint32_t addr, size; bool used; } PBuf;
static PBuf *pbufs;
static uint32_t npbufs, cap_pbufs;

static uint32_t pbuf_alloc(uint32_t len) {
    uint32_t size = (len + 63) & ~63u, a = 0;
    pthread_mutex_lock(&forge_lock);
    for (uint32_t i = 0; i < npbufs && !a; i++)
        if (!pbufs[i].used && pbufs[i].size == size) pbufs[i].used = true, a = pbufs[i].addr;
    pthread_mutex_unlock(&forge_lock);
    if (a) {
        memset(G2H(a), 0, size);
        return a;
    }
    a = hle_alloc(size);
    pthread_mutex_lock(&forge_lock);
    if (npbufs == cap_pbufs) {
        uint32_t cap = cap_pbufs ? cap_pbufs * 2 : 64;
        PBuf *grown = realloc(pbufs, cap * sizeof *pbufs);
        if (grown) pbufs = grown, cap_pbufs = cap;
    }
    if (npbufs < cap_pbufs) pbufs[npbufs++] = (PBuf){a, size, true};  // (else it is simply never reused)
    pthread_mutex_unlock(&forge_lock);
    return a;
}

static void pbuf_release(uint32_t addr) {
    pthread_mutex_lock(&forge_lock);
    for (uint32_t i = 0; i < npbufs; i++)
        if (pbufs[i].addr == addr) pbufs[i].used = false;
    pthread_mutex_unlock(&forge_lock);
}

static void patch_apply(uint32_t p, bool on) {
    if (!kernel_mapped(p, PATCH_SIZE)) return;
    uint32_t addr = LD32(p), size = LD32(p + 4), src = LD32(p + (on ? 8 : 12));
    if (!addr || LD8(p + 16) == on) return;
    if (size > MAX_PATCH || !kernel_mapped(addr, size) || !kernel_mapped(src, size)) {
        LOG("[forge] patch at %08x (%u bytes) is not valid memory: not %s\n", addr, size, on ? "enabled" : "disabled");
        return;
    }
    memmove(G2H(addr), G2H(src), size);
    ST8(p + 16, on);
    if (in_code(addr) || in_code(addr + size - 1)) code_changed(addr, size, on ? LD32(p + 12) : 0);
}

HLE_FUNC("forge_patch_create", f_patch_create) {  // (Patch *out, u32 address, const void *bytes, u32 length, bool enable)
    uint32_t out = ARG(0), addr = ARG(1), bytes = ARG(2), len = ARG(3), enable = STK(0) & 0xFF;
    if (!kernel_mapped(out, PATCH_SIZE)) {
        LOG("[forge] forge_patch_create: the result is not valid memory: no patch\n");
        return;
    }
    memset(G2H(out), 0, PATCH_SIZE);
    if (!len) return;
    if (len > MAX_PATCH || !kernel_mapped(addr, len) || !kernel_mapped(bytes, len)) {
        LOG("[forge] forge_patch_create: %u bytes at %08x (from %08x) are not valid memory: no patch\n", len, addr, bytes);
        return;
    }
    uint32_t pb = pbuf_alloc(len), ob = pbuf_alloc(len);
    memcpy(G2H(pb), G2H(bytes), len);
    memcpy(G2H(ob), G2H(addr), len);
    ST32(out, addr), ST32(out + 4, len), ST32(out + 8, pb), ST32(out + 12, ob);
    if (enable) patch_apply(out, true);
}
HLE_FUNC("forge_patch_enable", f_patch_enable) { patch_apply(ARG(0), true); }
HLE_FUNC("forge_patch_disable", f_patch_disable) { patch_apply(ARG(0), false); }
HLE_FUNC("forge_patch_destroy", f_patch_destroy) {
    uint32_t p = ARG(0);
    if (!kernel_mapped(p, PATCH_SIZE) || !LD32(p)) return;
    patch_apply(p, false);
    pbuf_release(LD32(p + 8));
    pbuf_release(LD32(p + 12));
    memset(G2H(p), 0, PATCH_SIZE);
}

// ------------------------------------------------------------------ memory
HLE_FUNC("forge_mem_getMainTextAddr", f_text) { RET(g_modules[0].base + g_modules[0].text_off); }
HLE_FUNC("forge_mem_getMainRodataAddr", f_rodata) { RET(g_modules[0].base + g_modules[0].ro_off); }
HLE_FUNC("forge_mem_getMainDataAddr", f_data) { RET(g_modules[0].base + g_modules[0].data_off); }
HLE_FUNC("forge_mem_getMainBssAddr", f_bss) {
    Module *m = &g_modules[0];
    RET(m->base + m->data_off + m->data_size - m->bss_size);
}
HLE_FUNC("forge_mem_getMainHeapAddr", f_heap) { RET(HEAP_BASE); }

// ------------------------------------------------------------------ log
// printf from the guest: variadic arguments in r2.. (after level and format) and on the stack, 64-bit values in an
// even register pair or an 8-byte aligned stack slot (doubles too: variadic arguments do not use VFP registers)
typedef struct { Cpu *c; int reg; uint32_t stk; } GArgs;
static uint32_t ga32(GArgs *a) {
    if (a->reg < 4) return a->c->r[a->reg++];
    uint32_t v = LD32(a->stk);
    a->stk += 4;
    return v;
}
static uint64_t ga64(GArgs *a) {
    if (a->reg & 1) a->reg++;
    if (a->reg < 4) {
        uint64_t v = a->c->r[a->reg] | ((uint64_t)a->c->r[a->reg + 1] << 32);
        a->reg += 2;
        return v;
    }
    a->reg = 4;
    a->stk = (a->stk + 7) & ~7u;
    uint64_t v = LD64(a->stk);
    a->stk += 8;
    return v;
}

// Append s to the conversion spec being built (spec[32]: room is kept for the length modifier, the conversion and the
// NUL); what does not fit is left out, so a plugin's absurd format can't run over it
#define SPEC_MAX 24
static void spec_add(char *spec, size_t *k, const char *s) {
    size_t n = strlen(s);
    if (*k + n <= SPEC_MAX) memcpy(spec + *k, s, n), *k += n;
}

static void gformat(char *out, size_t n, const char *f, GArgs *a) {  // (forge_format)
    size_t o = 0;
    while (*f && o + 1 < n) {
        if (*f != '%') { out[o++] = *f++; continue; }
        char spec[32], num[16];
        size_t k = 0;
        spec[k++] = *f++;
        while (*f && strchr("-+ #0", *f)) {
            if (k < 8) spec[k++] = *f;
            f++;
        }
        if (*f == '*') {
            snprintf(num, sizeof num, "%d", (int)ga32(a));
            spec_add(spec, &k, num);
            f++;
        } else {
            for (; isdigit((unsigned char)*f); f++)
                if (k < SPEC_MAX - 8) spec[k++] = *f;
        }
        if (*f == '.') {
            spec_add(spec, &k, ".");
            f++;
            if (*f == '*') {
                snprintf(num, sizeof num, "%d", (int)ga32(a));
                spec_add(spec, &k, num);
                f++;
            } else {
                for (; isdigit((unsigned char)*f); f++)
                    if (k < SPEC_MAX) spec[k++] = *f;
            }
        }
        int ll = 0;
        while (*f && strchr("hlzjtL", *f)) { if (*f == 'l' || *f == 'j' || *f == 'L') ll++; f++; }
        char conv = *f ? *f++ : 0;
        char tmp[512], str[256];
        if (conv == '%') { out[o++] = '%'; continue; }
        if (conv && strchr("diuxXoc", conv)) {
            uint64_t v = ll >= 2 ? ga64(a) : ga32(a);
            spec[k++] = 'l', spec[k++] = 'l', spec[k++] = conv, spec[k] = 0;
            if (conv == 'd' || conv == 'i') snprintf(tmp, sizeof tmp, spec, ll >= 2 ? (long long)v : (long long)(int32_t)v);
            else if (conv == 'c') snprintf(tmp, sizeof tmp, "%c", (int)v);
            else snprintf(tmp, sizeof tmp, spec, (unsigned long long)v);
        } else if (conv == 's') {
            spec[k++] = 's', spec[k] = 0;
            uint32_t p = ga32(a);
            snprintf(tmp, sizeof tmp, spec, p ? gstr_copy(p, str, sizeof str) : "(null)");
        } else if (conv == 'p') {
            snprintf(tmp, sizeof tmp, "0x%x", ga32(a));
        } else if (conv && strchr("fFeEgGaA", conv)) {
            uint64_t bits = ga64(a);
            double d;
            memcpy(&d, &bits, 8);
            spec[k++] = conv, spec[k] = 0;
            snprintf(tmp, sizeof tmp, spec, d);
        } else {
            break;
        }
        tmp[sizeof tmp - 1] = 0;
        for (char *t = tmp; *t && o + 1 < n; t++) out[o++] = *t;
    }
    out[o] = 0;
}

void forge_format(char *out, size_t n, const char *fmt, Cpu *c, int reg, uint32_t stk) {
    GArgs a = {c, reg, stk};
    gformat(out, n, fmt, &a);
}

// forge_log(level, format, ...): below the log level nothing is written. Forge's line is the level's tag ("[INFO] "; none
// for a level that is not one of the four, which is never below the level) and the text cut to its 520-byte buffer.
HLE_FUNC("forge_log", f_log) {
    int level = (int)ARG(0);
    if (level < log_level) return;
    static const char *names[] = {"[DEBUG] ", "[INFO] ", "[WARN] ", "[ERROR] "};
    const char *tag = level >= 0 && level < 4 ? names[level] : "";
    char fmt[512], buf[1024];
    GArgs a = {c, 2, c->r[13]};
    gformat(buf, sizeof buf, gstr_copy(ARG(1), fmt, sizeof fmt), &a);
    size_t room = 520 - strlen(tag) - 1;
    if (strlen(buf) > room) buf[room] = 0;
    size_t n = strlen(buf);
    while (n && buf[n - 1] == '\n') buf[--n] = 0;
    LOG("[forge] %s%s\n", tag, buf);
}
HLE_FUNC("forge_log_getLevel", f_log_level) { RET(log_level); }
// Extras beyond Forge's export list (kept from before the parity pass; harmless to plugins that never import them)
HLE_FUNC("forge_log_trace", f_log_trace) {  // (format, ...): printed whatever the level, labelled debug
    char fmt[512], buf[1024];
    GArgs a = {c, 1, c->r[13]};
    gformat(buf, sizeof buf, gstr_copy(ARG(0), fmt, sizeof fmt), &a);
    size_t n = strlen(buf);
    while (n && buf[n - 1] == '\n') buf[--n] = 0;
    LOG("[forge] [DEBUG] %s\n", buf);
}
HLE_FUNC("forge_graphics_isInitialized", f_gfx_init) { RET(overlay_enabled()); }
HLE_FUNC("forge_graphics_isMenuOpen", f_gfx_menu) { RET(overlay_menu_open()); }

// ------------------------------------------------------------------ save data
static uint32_t str_savedata;

// The sSavedata singleton, found by its name the same way for both calls
static uint32_t save_singleton(Cpu *c) { return singleton(c, mt_id("sSavedata")); }

// The SDK functions mounting and unmounting need (Forge's own calls): all of them, before anything is touched
static bool save_symbols(uint32_t *commit, uint32_t *unmount, uint32_t *lock, uint32_t *unlock) {
    *commit = gsym("_ZN2nn2fs14CommitSaveDataEPKc");
    *unmount = gsym("_ZN2nn2fs7UnmountEPKc");
    *lock = gsym("_ZN2nn2os9LockMutexEPNS0_9MutexTypeE");
    *unlock = gsym("_ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE");
    return *commit && *unmount && *lock && *unlock;
}

HLE_FUNC("forge_fs_unmountSaveData", f_unmount_save) {
    uint32_t commit, unmount, lock, unlock;
    bool job_safe = LD8(text_addr(OFF_JOB_SAFE));
    uint32_t save = save_singleton(c);
    if (!save || !LD32(save + 0xF1C)) return;  // not mounted
    if (!save_symbols(&commit, &unmount, &lock, &unlock)) return;  // (the mount count stays as it is)
    gcall(c, commit, str_savedata, 0, 0, 0);
    bool thread_safe = job_safe || LD8(save + 0x18);
    if (thread_safe) gcall(c, lock, save + 4, 0, 0, 0);
    uint32_t n = LD32(save + 0xF1C) - 1;
    ST32(save + 0xF1C, n);
    if (!n) gcall(c, unmount, str_savedata, 0, 0, 0);
    if (thread_safe) gcall(c, unlock, save + 4, 0, 0, 0);
}

HLE_FUNC("forge_fs_mountSaveData", f_mount_save) {
    uint32_t commit, unmount, lock, unlock;
    uint32_t save = save_singleton(c);
    if (!save || !save_symbols(&commit, &unmount, &lock, &unlock)) { RET(0); return; }  // (before the count changes)
    if (gcall(c, text_addr(OFF_MOUNT_SAVE), save, 0, 0, 0) != 0) {
        f_unmount_save(c);
        RET(0);
        return;
    }
    RET(1);
}

HLE_FUNC("forge_fs_getSaveDataMountPoint", f_save_mount_point) { RET(str_savedata); }
HLE_FUNC("forge_fs_getRomFsMountPoint", f_romfs_mount_point) {
    static uint32_t s;
    if (!s) s = gstring("app");
    RET(s);
}
HLE_FUNC("forge_fs_fileExists", f_file_exists) {
    uint32_t path = ARG(0), sp = c->r[13];
    if (!gsym("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci") || !gsym("_ZN2nn2fs9CloseFileENS0_10FileHandleE")) {
        RET(0);
        return;
    }
    c->r[13] = (sp - 16) & ~7u;
    uint32_t handle = c->r[13];
    uint32_t r = gcall(c, gsym("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci"), handle, path, 1, 0);
    if (!r) gcall(c, gsym("_ZN2nn2fs9CloseFileENS0_10FileHandleE"), LD32(handle), 0, 0, 0);
    c->r[13] = sp;
    RET(r == 0);
}

// ------------------------------------------------------------------ nn::os wrappers (same arguments as the SDK's)
#define FORWARD(name, sym, fn)                         \
    HLE_FUNC(name, fn) {                               \
        uint32_t a = gsym(sym);                        \
        if (!a) { RET(0); return; }                    \
        c->pc = a;                                     \
        rt_dispatch(c);                                \
    }
FORWARD("forge_nnosInitializeLightEvent", "_ZN2nn2os20InitializeLightEventEPNS0_14LightEventTypeEbNS0_14EventClearModeE", f_le_init)
FORWARD("forge_nnosFinalizeLightEvent", "_ZN2nn2os18FinalizeLightEventEPNS0_14LightEventTypeE", f_le_fini)
FORWARD("forge_nnosSignalLightEvent", "_ZN2nn2os16SignalLightEventEPNS0_14LightEventTypeE", f_le_signal)
FORWARD("forge_nnosWaitLightEvent", "_ZN2nn2os14WaitLightEventEPNS0_14LightEventTypeE", f_le_wait)
FORWARD("forge_nnosTryWaitLightEvent", "_ZN2nn2os17TryWaitLightEventEPNS0_14LightEventTypeE", f_le_trywait)
FORWARD("forge_nnosTimedWaitLightEvent", "_ZN2nn2os19TimedWaitLightEventEPNS0_14LightEventTypeENS_8TimeSpanE", f_le_timedwait)
FORWARD("forge_nnosClearLightEvent", "_ZN2nn2os15ClearLightEventEPNS0_14LightEventTypeE", f_le_clear)
FORWARD("forge_nnosLockMutex", "_ZN2nn2os9LockMutexEPNS0_9MutexTypeE", f_mutex_lock)
FORWARD("forge_nnosUnlockMutex", "_ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE", f_mutex_unlock)
HLE_FUNC("forge_nnosInitializeMutex", f_mutex_init) {  // (mutex, recursive): lock level 0
    c->r[2] = 0;
    uint32_t a = gsym("nnosInitializeMutex");
    if (!a) { RET(0); return; }
    c->pc = a;
    rt_dispatch(c);
}

// ------------------------------------------------------------------ input (the controller the game sees, the keyboard)
// Forge reads these once a frame, when it draws (after the plugins' forge_onUpdate), from nn::hid: the first connected
// of the handheld and player 1 pads (here the game's one pad, whatever the player holds: gamepad or keyboard), the
// first finger of the touch screen, the keyboard (256 HID usages).
static uint64_t in_now, in_prev;
static uint8_t keys_now[256], keys_prev[256];
static int32_t in_sticks[4];
static bool in_connected;  // (false until the first frame: Forge's state is empty until then)
static bool touch_down;
static float touch_x, touch_y;
static bool mouse_on;  // (the mouse for mods, the port's own calls below: the free pointer is on the picture)
static float mouse_x, mouse_y, mouse_dx, mouse_dy;
static uint32_t mouse_buttons;

// A point of the window (mx, my: pixels) on the game's picture, which is fitted into the window by its shape
// (g_render_aspect) and centred. The picture is the console's 1280x720 screen: its HUD canvas (g_ui_anchor_aspect wide
// at most, centred) is what 0..1279 spans; a point on the picture beyond that edge is at the edge. False off the picture.
static bool window_to_screen(SDL_Window *w, float mx, float my, float *x, float *y) {
    int ww = 0, wh = 0;
    if (!SDL_GetWindowSize(w, &ww, &wh) || ww <= 0 || wh <= 0) return false;
    float asp = g_render_aspect > 0 ? g_render_aspect : 16.0f / 9.0f;
    float pw = (float)ww, ph = (float)wh;  // (the picture's size in the window: the largest that fits)
    if (pw / ph > asp) pw = ph * asp;
    else ph = pw / asp;
    float u = (mx - ((float)ww - pw) / 2) / pw, v = (my - ((float)wh - ph) / 2) / ph;
    if (u < 0 || u >= 1 || v < 0 || v >= 1) return false;  // (outside the picture: no finger on the screen)
    float anchor = g_ui_anchor_aspect > 0 && g_ui_anchor_aspect < asp ? g_ui_anchor_aspect : asp;
    float canvas = anchor / asp;  // (the canvas's share of the picture's width)
    u = fminf(fmaxf((u - (1 - canvas) / 2) / canvas, 0.0f), 1.0f);
    *x = fminf(floorf(u * 1280.0f), 1279.0f);  // (Forge's are whole screen pixels)
    *y = fminf(floorf(v * 720.0f), 719.0f);
    return true;
}

// The mouse as the touch screen: the left button is a finger while the mouse is free (not captured for the camera,
// Forge's menu closed), and the pointer must be on the game's picture (window_to_screen).
static bool mouse_touch(float *x, float *y) {
    SDL_Window *w = ui_window();
    float mx, my;
    if (!w || overlay_menu_open() || SDL_GetWindowRelativeMouseMode(w) || !(SDL_GetMouseState(&mx, &my) & SDL_BUTTON_LMASK))
        return false;
    return window_to_screen(w, mx, my, x, y);
}

// The mouse for mods (runtime/host_input.c keeps it from the events): the pointer while the mouse is free (not captured,
// Forge's menu closed) and over the game's picture; the movement, unless Forge's menu has the mouse.
static void mouse_frame(void) {
    SDL_Window *w = ui_window();
    HostMouse m;
    host_input_mouse_take(&m);
    bool menu = overlay_menu_open();
    mouse_on = w && m.inside && !menu && !SDL_GetWindowRelativeMouseMode(w) && window_to_screen(w, m.x, m.y, &mouse_x, &mouse_y);
    mouse_buttons = m.buttons;
    mouse_dx = menu ? 0.0f : m.dx, mouse_dy = menu ? 0.0f : m.dy;
}

static void input_frame(void) {
    in_prev = in_now;
    in_now = input_npad_state(in_sticks);
    in_connected = true;
    memcpy(keys_prev, keys_now, sizeof keys_now);
    int n = 0;
    const bool *k = SDL_GetKeyboardState(&n);
    for (int i = 0; i < 256; i++) keys_now[i] = k && i < n ? k[i] : 0;
    touch_down = mouse_touch(&touch_x, &touch_y);
    mouse_frame();
}

static bool key(const uint8_t *s, uint32_t k) { return k < 256 && s[k]; }
HLE_FUNC("forge_input_isDown", f_in_down) { RET(ARG(0) < 64 && (in_now >> ARG(0)) & 1); }
HLE_FUNC("forge_input_isPressed", f_in_pressed) { RET(ARG(0) < 64 && ((in_now & ~in_prev) >> ARG(0)) & 1); }
HLE_FUNC("forge_input_isReleased", f_in_released) { RET(ARG(0) < 64 && ((~in_now & in_prev) >> ARG(0)) & 1); }
// the pad is there once Forge has read it (the game's player 1 pad always is)
HLE_FUNC("forge_input_isConnected", f_in_connected) { RET(in_connected); }
HLE_FUNC("forge_input_getTouch", f_in_touch) {  // (float *x, float *y): the first finger, in screen pixels (1280x720)
    if (!touch_down) { RET(0); return; }
    if (ARG(0)) STF(ARG(0), touch_x);
    if (ARG(1)) STF(ARG(1), touch_y);
    RET(1);
}
// The mouse for mods: the port's own calls, not Forge's (a mod made for the console doesn't know them).
// forge_input_getMouse(float *x, float *y, uint32_t *buttons): the pointer in screen pixels (1280x720, as getTouch's)
// and the buttons held or pressed since the last frame (1 left, 2 middle, 4 right, 8 / 16 the side buttons). False, and
// nothing written, while the mouse is captured, Forge's menu is open or the pointer is off the game's picture.
HLE_FUNC("forge_input_getMouse", f_in_mouse) {
    if (!mouse_on) { RET(0); return; }
    if (ARG(0)) STF(ARG(0), mouse_x);
    if (ARG(1)) STF(ARG(1), mouse_y);
    if (ARG(2)) ST32(ARG(2), mouse_buttons);
    RET(1);
}
// forge_input_getMouseMove(float *dx, float *dy): how far the mouse moved since the last frame, wherever the movement
// went (also the hunt camera's): counts while captured, window pixels while free; y down. 0 while Forge's menu is open.
HLE_FUNC("forge_input_getMouseMove", f_in_mouse_move) {
    if (ARG(0)) STF(ARG(0), mouse_dx);
    if (ARG(1)) STF(ARG(1), mouse_dy);
}
// forge_input_setMouseFree(bool free): while true the mouse isn't captured (let go if it was; captured again after, if
// the player had clicked in) and its clicks are only the mod's (getMouse): not a capture, not the game's buttons. For a
// mod's own pointer, while its menu is open.
HLE_FUNC("forge_input_setMouseFree", f_in_mouse_free) { host_input_set_mouse_free((ARG(0) & 0xFF) != 0); }
static float stick_value(int32_t v) { return fminf(fmaxf(v / 32767.0f, -1.0f), 1.0f); }
static void stick(Cpu *c, int i) {
    if (ARG(0)) STF(ARG(0), stick_value(in_sticks[i]));
    if (ARG(1)) STF(ARG(1), stick_value(in_sticks[i + 1]));
}
HLE_FUNC("forge_input_getStickL", f_in_stick_l) { stick(c, 0); }
HLE_FUNC("forge_input_getStickR", f_in_stick_r) { stick(c, 2); }
HLE_FUNC("forge_input_isKeyDown", f_key_down) { RET(key(keys_now, ARG(0))); }
HLE_FUNC("forge_input_isKeyPressed", f_key_pressed) { RET(key(keys_now, ARG(0)) && !key(keys_prev, ARG(0))); }
HLE_FUNC("forge_input_isKeyReleased", f_key_released) { RET(!key(keys_now, ARG(0)) && key(keys_prev, ARG(0))); }
HLE_FUNC("forge_input_isShiftDown", f_shift) { RET(key(keys_now, SDL_SCANCODE_LSHIFT) || key(keys_now, SDL_SCANCODE_RSHIFT)); }
HLE_FUNC("forge_input_isCtrlDown", f_ctrl) { RET(key(keys_now, SDL_SCANCODE_LCTRL) || key(keys_now, SDL_SCANCODE_RCTRL)); }
HLE_FUNC("forge_input_isAltDown", f_alt) { RET(key(keys_now, SDL_SCANCODE_LALT) || key(keys_now, SDL_SCANCODE_RALT)); }
HLE_FUNC("forge_graphics_isMenuVisible", f_gfx_menu_visible) { RET(overlay_menu_open()); }

// ------------------------------------------------------------------ sockets
// forge_socket_*: Forge's thin wrappers of nn::socket (a Socket is {int fd}, returned in r0), on the port's own
// nn::socket: the virtual network of runtime/net/vsock.c, which carries the game's local play and nothing else. Calls
// that network can't serve fail as Forge's do: -1 (the errno is nn::socket's, from vsock.c).
static void sock_forward(Cpu *c, const char *sym) {  // (the Socket * in r0 becomes its fd)
    HostFn impl = hle_lookup_impl(sym);
    uint32_t p = c->r[0];
    if (!impl || !p || !kernel_mapped(p, 4)) { RET(-1); return; }
    c->r[0] = LD32(p);
    impl(c);
}
#define SOCKET_FWD(name, sym, fn) HLE_FUNC(name, fn) { sock_forward(c, sym); }
SOCKET_FWD("forge_socket_bind", "_ZN2nn6socket4BindEiPK8sockaddrj", f_sock_bind)
SOCKET_FWD("forge_socket_connect", "_ZN2nn6socket7ConnectEiPK8sockaddrj", f_sock_connect)
SOCKET_FWD("forge_socket_listen", "_ZN2nn6socket6ListenEii", f_sock_listen)
SOCKET_FWD("forge_socket_accept", "_ZN2nn6socket6AcceptEiP8sockaddrPj", f_sock_accept)
SOCKET_FWD("forge_socket_shutdown", "_ZN2nn6socket8ShutdownEii", f_sock_shutdown)
SOCKET_FWD("forge_socket_send", "_ZN2nn6socket4SendEiPKvji", f_sock_send)
SOCKET_FWD("forge_socket_recv", "_ZN2nn6socket4RecvEiPvji", f_sock_recv)
SOCKET_FWD("forge_socket_getSockOpt", "_ZN2nn6socket10GetSockOptEiiiPvPj", f_sock_getopt)
SOCKET_FWD("forge_socket_setSockOpt", "_ZN2nn6socket10SetSockOptEiiiPKvj", f_sock_setopt)

HLE_FUNC("forge_socket_create", f_sock_create) {  // (int domain, int type, int protocol) -> Socket
    HostFn impl = hle_lookup_impl("_ZN2nn6socket6SocketEiii");
    if (impl) impl(c);
    else RET(-1);
}
HLE_FUNC("forge_socket_destroy", f_sock_destroy) {  // (Socket *)
    uint32_t p = ARG(0);
    HostFn impl = hle_lookup_impl("_ZN2nn6socket5CloseEi");
    if (!p || !impl || !kernel_mapped(p, 4) || (int32_t)LD32(p) < 0) return;
    c->r[0] = LD32(p);
    impl(c);
    ST32(p, (uint32_t)-1);
}
// The game's sockets are initialized by the game: what Forge's initDefault gets out of the game's own library
static uint32_t f_sock_init_default_value(Cpu *c);
HLE_FUNC("forge_socket_initDefault", f_sock_init_default) { RET(f_sock_init_default_value(c)); }
static uint32_t f_sock_init_default_value(Cpu *c) {
    HostFn impl = hle_lookup_impl("_ZN2nn6socket10InitializeEPvjji");
    if (!impl) { LOG("[forge] forge_socket_initDefault: the port has no nn::socket: failed\n"); return (uint32_t)-1; }
    c->r[0] = c->r[1] = c->r[2] = c->r[3] = 0;
    impl(c);
    return c->r[0];
}
HLE_FUNC("forge_socket_init", f_sock_init) {  // (pool, pool size, allocator pool size, concurrency): nn::socket::Initialize's
    HostFn impl = hle_lookup_impl("_ZN2nn6socket10InitializeEPvjji");
    if (impl) impl(c);
    else RET(-1);
}
HLE_FUNC("forge_socket_deinit", f_sock_deinit) {  // (nn::socket::Finalize)
    HostFn impl = hle_lookup_impl("_ZN2nn6socket8FinalizeEv");
    if (impl) impl(c);
}

// -mtp=soft code reads its thread pointer through this: the thread's own block (the TCB, then each plugin's __thread
// data, loader.c: R_ARM_TLS_TPOFF32). It is made when the thread first asks and goes back to a free list when the thread
// ends, so threads that come and go reuse the same few blocks (guest memory is never freed).
static pthread_key_t tp_key;
static pthread_once_t tp_once = PTHREAD_ONCE_INIT;
static uint32_t tp_free;  // blocks not in use, linked through their first word

static void tp_return(void *blk) {
    uint32_t b = (uint32_t)(uintptr_t)blk;
    pthread_mutex_lock(&forge_lock);
    ST32(b, tp_free);
    tp_free = b;
    pthread_mutex_unlock(&forge_lock);
}
static void tp_setup(void) { pthread_key_create(&tp_key, tp_return); }

HLE_FUNC("__aeabi_read_tp", f_read_tp) {
    pthread_once(&tp_once, tp_setup);
    uint32_t blk = (uint32_t)(uintptr_t)pthread_getspecific(tp_key);
    if (!blk) {
        uint32_t size = (loader_tls_block_size() + 63) & ~63u;
        if (size < 64) size = 64;
        pthread_mutex_lock(&forge_lock);
        if ((blk = tp_free)) tp_free = LD32(blk);
        pthread_mutex_unlock(&forge_lock);
        if (blk) memset(G2H(blk), 0, size);  // (a thread's __thread data starts zeroed)
        else blk = hle_alloc(size);
        pthread_setspecific(tp_key, (void *)(uintptr_t)blk);
    }
    RET(blk);
}

// ------------------------------------------------------------------ plugins
void forge_plugins_imgui(Cpu *c, int which, uint32_t a0, uint32_t a1, uint32_t a2) {
    for (int i = 0; i < nplugins; i++)
        if (plugins[i].active && plugins[i].on_imgui[which]) gcall(c, plugins[i].on_imgui[which], a0, a1, a2, 0);
}

bool forge_plugins_have(int which) {
    for (int i = 0; i < nplugins; i++)
        if (plugins[i].active && plugins[i].on_imgui[which]) return true;
    return false;
}

int forge_plugin_titles(const char **out, int max) {  // (as Forge's list: the plugins that gave a name)
    int n = 0;
    for (int i = 0; i < nplugins && n < max; i++)
        if (plugins[i].active && plugins[i].param && LD8(plugins[i].param + 6)) out[n++] = gstr(plugins[i].param + 6);
    return n;
}

// A module's initializers, as nn::ro runs them when it loads a module: DT_INIT, then DT_INIT_ARRAY (DT_PREINIT_ARRAY is
// for executables: a shared object's is not run, as Forge's own is run by its code only). An entry that is no ARM
// function of the module (0, -1, a Thumb address) is skipped. Forge's own modules end DT_INIT in a walk of
// __init_array_start__..__init_array_end__ (source/crti.c): a module built that way would run its constructors twice
// (there, then by nn::ro), so while DT_INIT runs the array holds no-ops and the constructors run once, after it.
static uint32_t nop_fn;

static void plugin_initializers(Cpu *c, Module *m) {
    uint32_t n = m->init_array_sz / 4, fns[256], count = 0, text = m->base + m->text_off;
    if (n > 256) {
        flog(3, "%s has %u initializers: the first 256 run", m->name, n);
        n = 256;
    }
    for (uint32_t k = 0; k < n; k++) {
        uint32_t fn = LD32(m->init_array + 4 * k);
        if (fn >= text && fn < text + m->text_size && !(fn & 3)) fns[count++] = fn;
        else LOG("[forge] %s: initializer %u (%08x) is not an ARM function of the module: skipped\n", m->name, k, fn);
    }
    if (m->init) {
        if (m->init & 3) LOG("[forge] %s: DT_INIT %08x is not an ARM function: skipped\n", m->name, m->init);
        else {
            if (count && !nop_fn && (nop_fn = slot_alloc(&stub_next, FORGE_END, 4, "the initializer stub"))) ST32(nop_fn, 0xE12FFF1Eu);  // bx lr
            uint32_t saved[256];
            for (uint32_t k = 0; k < n; k++) saved[k] = LD32(m->init_array + 4 * k);
            if (nop_fn)
                for (uint32_t k = 0; k < n; k++) ST32(m->init_array + 4 * k, nop_fn);
            gcall(c, m->init, 0, 0, 0, 0);
            for (uint32_t k = 0; k < n; k++) ST32(m->init_array + 4 * k, saved[k]);
        }
    }
    for (uint32_t k = 0; k < count; k++) gcall(c, fns[k], 0, 0, 0, 0);
}

// What Forge does in sApp::run (source/main.c): singletons resolved, the library for sockets up, every plugin loaded
// (forge_init did that, with the relocations) and initialized, then each plugin's forge_onLoad, the version check and
// forge_onInit. A plugin with no forge_onLoad gets neither call but stays: it still gets its update and ImGui calls.
static void app_run(Cpu *c) {
    uint32_t args[4];
    memcpy(args, c->r, sizeof args);
    flog(0, "Resolving Singletons...");
    singletons_resolve(c, true);
    uint32_t sock = f_sock_init_default_value(c);
    if (sock) LOG("[forge] [DEBUG] Failed to initialize socket library: 0x%X\n", sock);  // (a trace: always shown)
    flog(1, "Loading plugins...");
    flog(0, "Preparing to load %d plugins", nplugins);
    for (int i = 0; i < nplugins; i++) {
        flog(1, "Loading plugin app:/nativeNX/plugins/%s", plugins[i].name);
        plugin_initializers(c, plugins[i].m);
    }
    for (int i = 0; i < nplugins; i++) {
        Plugin *p = &plugins[i];
        p->param = hle_alloc(72);
        p->active = true;
        flog(0, "Resolving symbols for plugin app:/nativeNX/plugins/%s", p->name);
        if (!p->on_load) {
            flog(2, "Plugin app:/nativeNX/plugins/%s does not have a forge_onLoad function, skipping", p->name);
            continue;
        }
        gcall(c, p->on_load, p->param, 0, 0, 0);
        ST8(p->param + 69, 0);  // (the name is a C string)
        uint32_t major = LD16(p->param), minor = LD16(p->param + 2), patch = LD16(p->param + 4);
        if (major > FORGE_MAJOR || (major == FORGE_MAJOR && minor > FORGE_MINOR)) {
            flog(3, "Plugin app:/nativeNX/plugins/%s requires Forge version %u.%u.*, but loader is version %d.%d.%d", p->name,
                 major, minor, FORGE_MAJOR, FORGE_MINOR, FORGE_PATCH);
            p->active = false;
            continue;
        }
        (void)patch;
        if (p->on_init) {
            flog(0, "Initializing plugin app:/nativeNX/plugins/%s", p->name);
            gcall(c, p->on_init, 0, 0, 0, 0);
        }
    }
    memcpy(c->r, args, sizeof args);
    fnhook_call_original(c, text_addr(OFF_APP_RUN));
}

static void frame_update(Cpu *c) {
    uint32_t self = c->r[0];
    fnhook_call_original(c, text_addr(OFF_UPDATE));
    uint32_t ret[2] = {c->r[0], c->r[1]};
    float dt = LDF(self + 0x68), s0 = c->v.f[0];
    __atomic_add_fetch(&frame_no, 1, __ATOMIC_RELAXED);
    for (int i = 0; i < nplugins; i++) {
        if (!plugins[i].active || !plugins[i].on_update) continue;
        c->v.f[0] = dt;  // (plugins are softfp, as Forge: the float in r0; s0 too, for any built hard-float)
        memcpy(&c->r[0], &dt, 4);
        guest_call(c, plugins[i].on_update);
    }
    input_frame();  // (Forge reads the input when it draws, after the updates: the plugins' view is the last frame's)
    overlay_frame(c);
    c->r[0] = ret[0], c->r[1] = ret[1], c->v.f[0] = s0;
}

// a path under dir, matched without regard to case (mods come from every kind of system); false if missing
static bool ci_path(const char *dir, const char *rel, char *out, size_t n) {
    snprintf(out, n, "%s", dir);
    char part[256];
    while (*rel) {
        size_t k = strcspn(rel, "/");
        snprintf(part, sizeof part, "%.*s", (int)k, rel);
        rel += k + (rel[k] == '/');
        DIR *d = opendir(out);
        if (!d) return false;
        struct dirent *e;
        bool found = false;
        while ((e = readdir(d)))
            if (!strcasecmp(e->d_name, part)) {
                size_t len = strlen(out);
                snprintf(out + len, n - len, "/%s", e->d_name);
                found = true;
                break;
            }
        closedir(d);
        if (!found) return false;
    }
    return true;
}

static uint8_t *read_all(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = n > 0 ? malloc((size_t)n) : NULL;
    if (p && fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); p = NULL; }
    fclose(f);
    *len = (size_t)n;
    return p;
}

// ---------------------------------------------------------------- config.ini
// Forge reads its config.ini with iniparser (4.x), at sApp::run, from the one file the RomFS gives it (the topmost
// mod's: files are not merged), and so does this: lines of at most 1023 characters (a longer one, or a line that is
// not a comment, a [section] or "key = value" / "key =", makes iniparser give up on the whole file: the defaults
// apply), a trailing backslash joins the next line, ';' and '#' start comments, a value is quoted or ends at the
// first ';' or '#', keys and sections are case-insensitive, a later line replaces an earlier one. A UTF-8 byte order
// mark at the start is skipped (not confirmed in iniparser's source; the Windows editors write it).
enum { INI_EMPTY, INI_COMMENT, INI_SECTION, INI_VALUE, INI_ERROR };

static char *ini_strip(char *s) {
    while (isspace((unsigned char)*s)) s++;
    for (size_t n = strlen(s); n && isspace((unsigned char)s[n - 1]); n--) s[n - 1] = 0;
    return s;
}

static void ini_lower(char *s) {
    for (; *s; s++) *s = (char)tolower((unsigned char)*s);
}

// iniparser_line: line (stripped by the caller) -> section (kept when the line is not one), key, value
static int ini_line(char *line, char section[1025], char key[1025], char val[1025]) {
    size_t len = strlen(line);
    if (!len) return INI_EMPTY;
    if (line[0] == '#' || line[0] == ';') return INI_COMMENT;
    if (line[0] == '[' && line[len - 1] == ']') {
        size_t n = strcspn(line + 1, "]");
        if (n) {  // ("[]" names nothing: the last section stays)
            memcpy(section, line + 1, n);
            section[n] = 0;
            memmove(section, ini_strip(section), strlen(ini_strip(section)) + 1);
            ini_lower(section);
        }
        return INI_SECTION;
    }
    char *eq = strchr(line, '=');
    if (!eq || eq == line) return INI_ERROR;
    memcpy(key, line, (size_t)(eq - line));
    key[eq - line] = 0;
    memmove(key, ini_strip(key), strlen(ini_strip(key)) + 1);
    ini_lower(key);
    const char *rest = eq + 1;
    while (isspace((unsigned char)*rest)) rest++;
    val[0] = 0;
    if (*rest == '"' || *rest == '\'') {  // "value" or 'value' (at least one character: else it is read as the next form)
        size_t n = strcspn(rest + 1, *rest == '"' ? "\"" : "'");
        if (n) {
            memcpy(val, rest + 1, n);
            val[n] = 0;
            return INI_VALUE;
        }
    }
    if (*rest && *rest != ';' && *rest != '#') {  // value; comment
        size_t n = strcspn(rest, ";#");
        memcpy(val, rest, n);
        val[n] = 0;
        memmove(val, ini_strip(val), strlen(ini_strip(val)) + 1);
        if (!strcmp(val, "\"\"") || !strcmp(val, "''")) val[0] = 0;
    }
    return INI_VALUE;  // ("key =", "key = ; comment": an empty value)
}

static int log_level_of(const char *s) {  // Forge's names; -1: none of them
    static const char *names[] = {"debug", "info", "warn", "error"};
    for (int i = 0; i < 4; i++)
        if (!strcasecmp(s, names[i])) return i;
    return -1;
}

typedef struct { char level[1025]; int key; float font_size; } ForgeConfig;

// Forge's defaults (config.c), then the file's settings if it parses; false: no file or iniparser refused it
static void forge_config_defaults(ForgeConfig *cfg) {
    snprintf(cfg->level, sizeof cfg->level, "info");
    cfg->key = 73;  // (Insert: Forge's own default is F9, 66; the port keeps the upstream author's choice)
    cfg->font_size = 12.0f;
}

static bool forge_config_load(const char *path, ForgeConfig *cfg) {
    forge_config_defaults(cfg);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    ForgeConfig got = *cfg;
    char line[1025], section[1025] = "", key[1025], val[1025];
    size_t last = 0;
    bool bad = false;
    for (int lineno = 1; fgets(line + last, (int)(sizeof line - last), f); lineno++) {
        if (lineno == 1 && !last && !memcmp(line, "\xEF\xBB\xBF", 3)) memmove(line, line + 3, strlen(line + 3) + 1);
        int len = (int)strlen(line) - 1;
        if (len <= 0) continue;
        if (line[len] != '\n' && !feof(f)) { bad = true; break; }  // (longer than iniparser's line buffer)
        while (len >= 0 && (line[len] == '\n' || isspace((unsigned char)line[len]))) line[len--] = 0;
        if (len >= 0 && line[len] == '\\') {  // (continued on the next line)
            last = (size_t)len;
            continue;
        }
        last = 0;
        int t = ini_line(ini_strip(line), section, key, val);
        if (t == INI_ERROR) { bad = true; break; }
        if (t != INI_VALUE) continue;
        if (!strcmp(section, "log") && !strcmp(key, "level")) snprintf(got.level, sizeof got.level, "%s", val);
        else if (!strcmp(section, "menu") && !strcmp(key, "key")) got.key = (int)strtol(val, NULL, 0);
        else if (!strcmp(section, "menu") && !strcmp(key, "font_size")) got.font_size = (float)atof(val);
    }
    fclose(f);
    if (bad) return false;
    *cfg = got;
    return true;
}

static bool env_level;  // MHGU_FORGE_LOG names a level

// forge_log_trace: shown whatever the level (labelled debug)
static void trace(const char *msg) { LOG("[forge] [DEBUG] %s\n", msg); }

// The ImGui overlay and the log, with Forge's settings (its config.ini: [menu] key / font_size, [log] level) and font
// where a mod has them (Forge's own files: romfs nativeNX/forge/; the topmost layer that has the file wins), else
// Forge's defaults (info, 12 px; the menu key Insert) and ImGui's font. A level that is not a name is the lowest one
// (Forge keeps its start-up level, error). MHGU_FORGE_LOG, when it names a level, picks it over the file.
static void overlay_start(const char **layers, int nl) {
    char path[1400], font[1400] = "", file[1400] = "";
    for (int l = 0; l < nl; l++) {
        if (ci_path(layers[l], "nativeNX/forge/fonts/Roboto-Medium.ttf", path, sizeof path)) snprintf(font, sizeof font, "%s", path);
        if (ci_path(layers[l], "nativeNX/forge/config.ini", path, sizeof path)) snprintf(file, sizeof file, "%s", path);
    }
    ForgeConfig cfg;
    forge_config_defaults(&cfg);  // (no file: the defaults; a file that fails to load leaves them too)
    if (!file[0] || !forge_config_load(file, &cfg)) trace("Failed to load config, using defaults");
    int level = log_level_of(cfg.level);
    if (!env_level) log_level = level >= 0 ? level : 3;
    overlay_init(font[0] ? font : NULL, cfg.font_size, cfg.key);
}

// The trampoline and stub regions of Forge's hooks (once: the test hooks come before the plugins)
static void forge_map_regions(void) {
    static bool done;
    if (done) return;
    done = true;
    kernel_map_region(FORGE_TRAMP, FORGE_STUBS - FORGE_TRAMP, 3, 5);
    kernel_map_region(FORGE_STUBS, FORGE_END - FORGE_STUBS, 3, 5);
    interp_add_range(FORGE_STUBS, FORGE_END);
}

void forge_init(void) {
    const char *lv = getenv("MHGU_FORGE_LOG");  // debug, info, warn, error
    if (lv && log_level_of(lv) >= 0) log_level = log_level_of(lv), env_level = true;
    main_text = g_modules[0].base + g_modules[0].text_off;
    plat_touch_hook = touch_hook;
    trace("Initializing Forge...");
    trace("Version " FORGE_VERSION_STRING);
    for (const char *s = getenv("MHGU_FORGE_TESTHOOK"); s && *s; s += *s == ',') {
        char *e;
        forge_test_hook((uint32_t)strtoul(s, &e, 16));
        if (e == s) break;
        s = e;
    }
    // the plugin files: the game's RomFS, then each mod's (a later one replaces one of the same name)
    static char found[MAX_PLUGINS][1400];  // (kept off the stack)
    char base[1024], dir[1100];
    const char *layers[65];
    snprintf(base, sizeof base, "%s/romfs", g_game_dir);
    layers[0] = base;
    int nl = 1 + mods_romfs_layers(layers + 1, 64), nf = 0;
    for (int l = 0; l < nl; l++) {
        if (!ci_path(layers[l], "nativeNX/plugins", dir, sizeof dir)) continue;
        DIR *d = opendir(dir);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            const char *ext = strrchr(e->d_name, '.');  // (Forge: the name ends in ".nro", exactly)
            if (!ext || strcmp(ext, ".nro")) continue;
            int i = 0;
            while (i < nf && strcasecmp(strrchr(found[i], '/') + 1, e->d_name)) i++;
            if (i == MAX_PLUGINS) {
                flog(3, "%s: skipped, at most %d plugins can be loaded (%s)", e->d_name, MAX_PLUGINS, dir);
                continue;
            }
            snprintf(found[i], sizeof found[i], "%s/%s", dir, e->d_name);
            if (i == nf) nf++;
        }
        if (d) closedir(d);
    }
    str_savedata = gstring("savedata");
    uint32_t at = PLUGIN_BASE;
    for (int i = 0; i < nf; i++) {
        size_t len;
        uint8_t *data = read_all(found[i], &len);
        const char *name = strrchr(found[i], '/') + 1;
        uint32_t bss = 0;
        if (data && len >= 0x3C) memcpy(&bss, data + 0x38, 4);
        // (the image, its bss and the 64 KiB alignment of the next one; the trampolines and stubs have their own regions)
        bool room = data && (uint64_t)at + len + bss + 0x10000 <= PLUGIN_END;
        Module *m = room ? loader_load_nro(name, data, len, at) : NULL;
        free(data);
        if (!m) {
            if (!data) flog(3, "Failed to read plugin file app:/nativeNX/plugins/%s (%s)", name, found[i]);
            else if (!room) flog(3, "%s: skipped, no room left in the plugin memory region (%s)", name, found[i]);
            else flog(3, "Failed to load plugin app:/nativeNX/plugins/%s: not a plugin this port can load (%s)", name, found[i]);
            continue;
        }
        Plugin *p = &plugins[nplugins++];
        snprintf(p->name, sizeof p->name, "%s", name);
        p->m = m;
        p->on_load = loader_module_symbol(m, "forge_onLoad");
        p->on_init = loader_module_symbol(m, "forge_onInit");
        p->on_update = loader_module_symbol(m, "forge_onUpdate");
        p->on_imgui[FORGE_IMGUI_INIT] = loader_module_symbol(m, "forge_onImGuiInit");
        p->on_imgui[FORGE_IMGUI_RENDER] = loader_module_symbol(m, "forge_onImGuiRender");
        p->on_imgui[FORGE_IMGUI_FREE_RENDER] = loader_module_symbol(m, "forge_onImGuiFreeRender");
        interp_add_range(m->base + m->text_off, m->base + m->text_off + m->text_size);
        kernel_map_region(m->base + m->text_off, m->ro_off - m->text_off, 3, 5);
        kernel_map_region(m->base + m->ro_off, m->data_off - m->ro_off, 3, 1);
        kernel_map_region(m->base + m->data_off, m->size - m->data_off, 4, 3);
        flog(0, "Found plugin file: app:/nativeNX/plugins/%s", name);
        LOG("[forge] plugin %s (%s)\n", name, found[i]);
        at = (at + m->size + 0xFFFF) & ~0xFFFFu;
    }
    // Host settings need the overlay even when no Forge mods are installed.
    fnhook_set(text_addr(OFF_UPDATE), hle_register("forge:update", frame_update));
    overlay_start(layers, nl);
    if (!nplugins) return;
    forge_map_regions();
    fnhook_set(text_addr(OFF_SYSTEM_CTOR), hle_register("forge:cSystem::cSystem", system_ctor));
    fnhook_set(text_addr(OFF_SYSTEM_DTOR), hle_register("forge:cSystem::~cSystem", system_dtor));
    fnhook_set(text_addr(OFF_APP_RUN), hle_register("forge:sApp::run", app_run));
}
