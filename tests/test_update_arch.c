#include <assert.h>
#include <stdio.h>
#include "updater/arch.h"

int main(void) {
    const char *arm = "LinuxARM64-MHGURecomp.1.2.3.tar.gz";
    const char *x86 = "Linux-MHGURecomp.1.2.3.tar.gz";
#if defined(_WIN32)
    assert(!update_asset_matches(arm));
    assert(!update_asset_matches(x86));
    assert(update_asset_matches("Windows-MHGURecomp.1.2.3.zip"));
#elif MHGU_TARGET_MACHINE == 183
    assert(update_asset_matches(arm));
    assert(!update_asset_matches(x86));
    assert(!update_asset_matches("ArchLinux-MHGURecomp.1.2.3.zip"));
#else
    assert(!update_asset_matches(arm));
    assert(update_asset_matches(x86));
    assert(update_asset_matches("ArchLinux-MHGURecomp.1.2.3.zip"));
#endif
    assert(!update_asset_matches("Linux-aarch64-MHGURecomp.1.2.3.tar.gz"));
#ifndef _WIN32
    assert(!update_asset_matches("Windows-MHGURecomp.1.2.3.zip"));
#endif
    assert(!update_asset_matches("Linux-MHGURecompExtra.1.2.3.tar.gz"));
    unsigned char h[64] = {0x7f, 'E', 'L', 'F', 2, 1, 1};
    h[16] = 3;
    h[18] = MHGU_TARGET_MACHINE & 255;
    h[19] = MHGU_TARGET_MACHINE >> 8;
    h[20] = 1;
    h[52] = 64;
    assert(update_elf_matches(h, sizeof h));
    assert(!update_elf_matches(h, 20));
    for (unsigned i = 0; i < 7; ++i) {
        unsigned char old = h[i];
        h[i] = 0;
        assert(!update_elf_matches(h, sizeof h));
        h[i] = old;
    }
    h[18] = MHGU_TARGET_MACHINE == 183 ? 62 : 183;
    assert(!update_elf_matches(h, sizeof h));
    puts("updater architecture checks: PASS");
}
