// The game's ways out: abort(), exit() and the SDK's failed checks (NN_ABORT, NN_ABORT_UNLESS, NN_RESULT_DO...) end
// the process through the SDK's exit path (svcExitProcess) without a word on the host. These log why and from where
// before the process ends, so a report of the game closing at start-up says what it gave up on. Every AbortImpl /
// nndetailDiagAbort*Impl variant of the SDK ends in VAbortImpl; HLE_SDK replaces these for the game's imports and the
// SDK's own calls alike.
#include <stdlib.h>

#include "../forge.h"
#include "../hle.h"
#include "../kernel.h"

int fs_log_open_writes(const char *tag);  // runtime/services/fs.c: files open for writing (logged), the count

// The exit code of the game giving up (exitcode.h): a mod's code that called it, or inside a Forge mod's file call,
// is the mod's; else the game's
static int gave_up(const Cpu *c) {
    int who = rt_blame_guest(c->r[14], EXIT_CRASH_GAME);
    if (who == EXIT_CRASH_GAME && forge_abort_context()) who = EXIT_CRASH_FORGE;
    return who;
}

static const char *guest_str(uint32_t addr, char *buf, size_t size) {  // a guest string ("-": none), cut at size
    if (!addr) return "-";
    size_t i = 0;
    for (; i + 1 < size; i++)
        if (!(buf[i] = (char)LD8(addr + (uint32_t)i))) break;
    buf[i] = 0;
    return buf;
}

// VAbortImpl(const char *condition, const char *function, const char *file, int line, const Result *result,
//            const os::UserExceptionInfo *info, const char *format, va_list args)
HLE_SDK("_ZN2nn4diag6detail10VAbortImplEPKcS3_S3_iPKNS_6ResultEPKNS_2os17UserExceptionInfoES3_St9__va_list",
        _ZN2nn4diag6detail10VAbortImplEPKcS3_S3_iPKNS_6ResultEPKNS_2os17UserExceptionInfoES3_St9__va_list) {
    char cond[200], fn[200], file[200], fmt[300];
    uint32_t result = STK(0) ? LD32(STK(0)) : 0;
    LOG("[abort] failed check: %s in %s (%s:%d)\n", guest_str(ARG(0), cond, sizeof cond),
        guest_str(ARG(1), fn, sizeof fn), guest_str(ARG(2), file, sizeof file), (int)ARG(3));
    if (STK(0)) LOG("[abort] result %08x (%04u-%04u)\n", result, 2000 + (result & 0x1ff), (result >> 9) & 0x1fff);
    if (STK(2)) LOG("[abort] message: %s\n", guest_str(STK(2), fmt, sizeof fmt));
    const char *forge = forge_abort_context();
    if (forge) LOG("[abort] inside a Forge mod's file call: %s\n", forge);
    kernel_log_guest_stack("[abort] the game", c);
    char plug[400];
    if (forge_stack_plugins(c->r[13], plug, sizeof plug)) LOG("[abort] Forge plugins' code on the stack:%s\n", plug);
    // A save's commit with a file still open for writing (2002-6457, nn::fs's WriteModeFileNotClosed) names no file:
    // which ones are open, and who opened them (every abort: any file I/O left half done is worth knowing)
    int nw = fs_log_open_writes("[abort] open for writing");
    int nf = forge_log_open_files("[abort] a Forge plugin's open file");
    if (STK(0) && result == 0x327202u && !nw && !nf) LOG("[abort] (no file is open for writing on the host side)\n");
    rt_exit(gave_up(c));
}

HLE_SDK("abort", abort) {
    kernel_log_guest_stack("[abort] abort()", c);
    rt_exit(gave_up(c));
}

// The game's fatal error (0xa3e16c(object, format, ...): vsnprintf into a 1 KB buffer at sp + 4, the object's error
// callback, exit(1); its caller's return address at sp + 0x414). Callers: 0x9bd71c "Memory Pool Initialization Error",
// 0xa69924 "Couldn't initialize the NVN device.", 0xb6b99c "uSerial::setup() : not found 'serial'.", 0xdd7250
// "Shader compile failed. %s" (a shader resource that did not load), 0xdd8f00 (a resource collision)
#define FATAL_EXIT_RET 0xa3e1c8u

HLE_SDK("exit", exit) {
    LOG("[abort] exit(%d)\n", (int)ARG(0));
    if (c->r[14] == FATAL_EXIT_RET) {
        char msg[400];
        LOG("[abort] the game's error: %s (called from %x)\n", guest_str(c->r[13] + 4, msg, sizeof msg),
            LD32(c->r[13] + 0x414));
    }
    kernel_log_guest_stack("[abort] the game", c);
    rt_exit(ARG(0) ? gave_up(c) : EXIT_NORMAL);
}
