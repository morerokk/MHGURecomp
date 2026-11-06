// The native adapters need their CPU registers back after inspecting a guest
// function. guest_call itself restores only LR; the helper owns this contract.
#include "hle/guest_call.h"
#include <assert.h>
#include <stdio.h>

enum { OUTER_FN = 0x123400, INNER_FN = 0x567800 };
static uint32_t caller[16];
static unsigned calls;

void guest_call(Cpu *c, uint32_t address) {
    ++calls;
    if (address == INNER_FN) {
        assert(c->r[0] == 11 && c->r[1] == 22 && c->r[2] == 33 && c->r[3] == 44);
        for (unsigned i = 4; i < 16; ++i) assert(c->r[i] == 0xa0000000u + i);
        for (unsigned i = 0; i < 16; ++i) c->r[i] = 0xb0000000u + i;
        c->r[0] = 0x87654321;
        c->pc = 0x11223344;
        c->zf = 1;
        return;
    }
    assert(address == OUTER_FN);
    assert(c->r[0] == 101 && c->r[1] == 202 && c->r[2] == 303 && c->r[3] == 404);
    for (unsigned i = 4; i < 16; ++i) assert(c->r[i] == caller[i]);
    for (unsigned i = 0; i < 16; ++i) c->r[i] = 0xa0000000u + i;
    uint32_t nested_result = guest_call4(c, INNER_FN, 11, 22, 33, 44);
    assert(nested_result == 0x87654321);
    for (unsigned i = 0; i < 16; ++i) assert(c->r[i] == 0xa0000000u + i);
    assert(c->pc == 0x11223344 && c->zf == 1);
    c->r[0] = 0xdeadbeef;
}

int main(void) {
    Cpu cpu = {0};
    for (unsigned pass = 0; pass < 2; ++pass) {
        for (unsigned i = 0; i < 16; ++i) caller[i] = 0x10000000u * (pass + 1) + i;
        memcpy(cpu.r, caller, sizeof caller);
        cpu.pc = 0xabcdef00;
        cpu.zf = 0;
        uint32_t result = guest_call4(&cpu, OUTER_FN, 101, 202, 303, 404);
        assert(result == 0xdeadbeef);
        // Includes r0, SP, LR and the normally unused r15.
        assert(!memcmp(cpu.r, caller, sizeof caller));
        // This helper promises only r[] preservation, matching the adapters.
        assert(cpu.pc == 0x11223344 && cpu.zf == 1);
    }
    assert(calls == 4);
    puts("guest call register preservation checks passed");
    return 0;
}
