// Keep ARM names outside legacy "Linux-" so old x86 updaters cannot select them.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#ifndef MHGU_TARGET_MACHINE
#if defined(__aarch64__)
#define MHGU_TARGET_MACHINE 183
#elif defined(__x86_64__)
#define MHGU_TARGET_MACHINE 62
#else
#error "Unsupported updater architecture"
#endif
#endif

static inline bool update_asset_matches(const char *name) {
#ifdef _WIN32
    const char *prefix = "Windows-MHGURecomp.";
    return !strncmp(name, prefix, strlen(prefix));
#elif MHGU_TARGET_MACHINE == 183
    const char *prefix = "LinuxARM64-MHGURecomp.";
    return !strncmp(name, prefix, strlen(prefix));
#else
    const char *a = "Linux-MHGURecomp.", *b = "ArchLinux-MHGURecomp.";
    return !strncmp(name, a, strlen(a)) || !strncmp(name, b, strlen(b));
#endif
}

static inline bool update_elf_matches(const unsigned char *h, size_t n) {
    return n >= 64 && !memcmp(h, "\177ELF\2\1\1", 7) &&
           (h[16] == 2 || h[16] == 3) && h[17] == 0 &&
           h[18] == (MHGU_TARGET_MACHINE & 255) &&
           h[19] == (MHGU_TARGET_MACHINE >> 8) &&
           h[20] == 1 && h[21] == 0 && h[22] == 0 && h[23] == 0 &&
           h[52] == 64 && h[53] == 0;
}
