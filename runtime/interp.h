// AArch32 interpreter for guest code that was not recompiled ahead of time: Forge plugins (runtime/forge.c), loaded
// at run time from the mods, and game functions whose code cheats changed (runtime/cheats.c, dispatch_interpret).
// It works on the same Cpu state as the recompiled code and follows the lifter
// (tools/lifter.py, tools/neon.py) instruction by instruction, so both run guest code the same way.
#pragma once
#include "cpu.h"

// Execute the instruction w at pc and return the address of the next one. *link is set for a branch with link
// (BL / BLX: r14 already holds the return address).
uint32_t interp_exec(Cpu *c, uint32_t pc, uint32_t w, int *link);
// The same through the pre-decoded op a run uses for w (tools/test_lift.py --interp-ops).
uint32_t interp_exec_op(Cpu *c, uint32_t pc, uint32_t w, int *link);
// Bumped when what a run decodes may have changed without the code word in memory changing (a hook set, an entry's
// kept words): every thread drops its decoded blocks (interp.c) at its next block.
extern volatile uint32_t g_code_gen;
static inline void interp_code_changed(void) { __atomic_add_fetch(&g_code_gen, 1, __ATOMIC_RELEASE); }

// Run the function at c->pc until it returns to its caller (c->r[14] on entry). Code inside [lo, hi) of a
// registered range is interpreted; calls out of it (to the game, the SDK, host functions) go through rt_dispatch.
void interp_run(Cpu *c);
void interp_add_range(uint32_t lo, uint32_t hi);  // (one touching or overlapping a registered range extends it)
int interp_owns(uint32_t pc);

// Bad guest accesses by interpreted code (Forge plugins). The interpreter notes the instruction it runs (0 outside
// it); a load / store whose address is in the null page, or looks like a float bit pattern and is unmapped, is
// reported to forge_guest_fault, and so is the first touch of an unmapped page (platform.h plat_touch_hook).
extern __thread uint32_t g_ip_pc;       // the instruction this thread is interpreting now (0: none)
extern __thread uint32_t g_ip_last;     // the last one it interpreted (stays set across a call out of it)
extern __thread Cpu *g_ip_cpu;          // its registers
// (runtime/forge.c) size 0: not known; write: 1 / 0 / -1 unknown. Only a plugin's pc is reported.
void forge_guest_fault(uint32_t pc, uint32_t addr, int size, int write, const char *why);
