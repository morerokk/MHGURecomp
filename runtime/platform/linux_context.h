// Linux host fault state, not the emulated game's AArch32 registers.
#pragma once
#include <stdint.h>
#include <string.h>
#include <sys/ucontext.h>

static inline int linux_fault_write(const ucontext_t *ctx) {
#if defined(__x86_64__)
    return (ctx->uc_mcontext.gregs[REG_ERR] & 2) != 0;
#elif defined(__aarch64__)
    // ESR records are optional. Validate each record before reading it.
    const unsigned char *p = ctx->uc_mcontext.__reserved;
    const size_t capacity = sizeof ctx->uc_mcontext.__reserved;
    for (size_t offset = 0; offset + 8 <= capacity;) {
        uint32_t magic, size;
        memcpy(&magic, p + offset, 4);
        memcpy(&size, p + offset + 4, 4);
        if (!magic || size < 8 || size > capacity - offset || size % 16) break;
        if (magic == 0x45535201u && size >= 16) { // ESR_MAGIC
            uint64_t esr;
            memcpy(&esr, p + offset + 8, 8);
            unsigned ec = (unsigned)(esr >> 26) & 0x3f;
            return ec == 0x24 || ec == 0x25 ? (int)((esr >> 6) & 1) : -1;
        }
        offset += size;
    }
    return -1;
#else
#error "Unsupported Linux signal ABI"
#endif
}
