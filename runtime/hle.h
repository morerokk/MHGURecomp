// Registration of host implementations for guest symbols, and argument helpers for them (AAPCS, hard float).
#pragma once
#include <stdlib.h>
// Environment lookups that run per call: looked up once per call site. ENV("MHGU_X") -> const char * or NULL.
#define ENV(name) ({ static const char *env_v_; static int env_i_; if (!env_i_) { env_v_ = getenv(name); env_i_ = 1; } env_v_; })

#include "rt.h"

typedef struct { const char *name; HostFn fn; } HleImplEntry;

// HLE_FUNC("mangled_symbol", c_function) binds a guest import (or an SDK function listed in hle_symbols.txt) to
// a host function.
#define HLE_CAT2(a, b) a##b
#define HLE_CAT(a, b) HLE_CAT2(a, b)
#if defined(__linux__)
#define HLE_SECTION "hle_impls"
#elif defined(__APPLE__)
#define HLE_SECTION "__DATA,hle_impls"  // Mach-O: bounds from ld64's section$start / section$end (runtime/dispatch.c)
#else
#define HLE_SECTION ".hle$m"  // between the .hle$a / .hle$z markers in runtime/dispatch.c
#endif
#define HLE_FUNC(sym, fn)                                                                   \
    static void fn(Cpu *c);                                                                 \
    __attribute__((used, section(HLE_SECTION))) static const HleImplEntry                   \
        HLE_CAT(hle_entry_, __LINE__) = {sym, fn};                                          \
    static void fn(Cpu *c)

// HLE_SDK("symbol", cname) replaces an SDK function everywhere: the game's imports of it (as HLE_FUNC) and the
// SDK's own calls (list the symbol in runtime/hle/hle_symbols.txt: the generator turns the recompiled function into a
// call of hle_<symbol with non-identifier characters as '_'>, which is cname prefixed with hle_).
#define HLE_SDK(sym, cname)                                                                 \
    void hle_##cname(Cpu *c);                                                               \
    __attribute__((used, section(HLE_SECTION))) static const HleImplEntry                   \
        HLE_CAT(hle_entry_, __LINE__) = {sym, hle_##cname};                                 \
    void hle_##cname(Cpu *c)

// ---- arguments (AAPCS): r0-r3, then the stack; 64-bit values in an even/odd register pair (or 8-byte aligned
// stack slot); floats in s0-s15 / d0-d7 (VFP hard-float).
#define ARG(i) (c->r[(i)])
#define ARGP(i) G2H(c->r[(i)])
#define STK(i) LD32(c->r[13] + 4 * (i))           // i-th word of the caller's outgoing stack arguments
#define ARG64(lo) (((uint64_t)c->r[(lo) + 1] << 32) | c->r[(lo)])
#define ARGF(i) (c->v.f[(i)])
#define RET(v) (c->r[0] = (uint32_t)(v))
#define RET64(v) do { uint64_t r_ = (uint64_t)(v); c->r[0] = (uint32_t)r_; c->r[1] = (uint32_t)(r_ >> 32); } while (0)
#define RETF(fv_) (c->v.f[0] = (fv_))

// Per-boot host state reset (LAS Recomp's in-place reboot): no reboot here, kept so shared code compiles.
#define RT_ON_RESET(fn) static __attribute__((unused)) void fn(void)

#define RES_OK 0u
#define MAKE_RESULT(module, desc) ((uint32_t)(module) | ((uint32_t)(desc) << 9))
