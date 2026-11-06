// Exercise the real native adapters and policy. Only guest functions, memory
// allocation and hook dispatch are replaced; no adapter state is exposed.
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include "hle/armorfusion.h"
#include "rt.h"

enum {
    UPDATE = 0x860314, CLOSE = 0x86549c, CONFIRM = 0x74fd94,
    CONSUME = 0x74ff80, SMITH_UPDATE = 0x8f1ae8, SMITH_CLOSE = 0x8f0c70,
    SMITH_DTOR = 0x8f02fc, SMITH_DELETE = 0x8f085c,
    SMITH = 0x5000000, COORD = 0x5010000, SMITH_CHILDREN = 0x5020000,
    COORD_CHILDREN = 0x5030000, COMMON = 0x5040000, COMMON_CHILDREN = 0x5050000,
    WIDGETS = 0x5060000, RECORDS = 0x5080000, ITEM = 0x5090000,
    INPUT = 0x50a0000, FACILITY = 0x50b0000, BOX = 0x50c0000, PLAYER = 0x50d0000,
    ACCEPT = 0x2000, BACK = 0x4000, MEMORY_SIZE = 0x8000000
};
static struct { uint32_t address; HostFn fn; } hooks[16];
static HostFn registered[16];
static unsigned hook_count, registered_count;
static uint32_t heap = 0x6000000;
static unsigned confirms, charges, deductions, refreshes, ui_changes, closes, questions;
static unsigned poll_result, native_answer, material_quantity;
static unsigned zenny, ownership, native_pair_result, material_points;
static uint32_t consumed_item, consumed_quantity;
static bool confirm_in_update, consume_in_update;
static uint32_t confirm_caller = 0x860e7c;
static uint32_t consume_caller = 0x860eb0;
static uint32_t confirmation_result;

static HostFn hook(uint32_t address) {
    for (unsigned i = 0; i < hook_count; ++i)
        if (hooks[i].address == address) return hooks[i].fn;
    assert(!"missing hook");
    return NULL;
}
uint32_t hle_register(const char *name, HostFn fn) {
    (void)name; assert(registered_count < 16);
    registered[registered_count++] = fn;
    return registered_count;
}
bool fnhook_set(uint32_t address, uint32_t detour) {
    assert(hook_count < 16 && detour > 0 && detour <= registered_count);
    hooks[hook_count].address = address;
    hooks[hook_count++].fn = registered[detour - 1];
    return true;
}
uint32_t hle_alloc(uint32_t size) { uint32_t p = heap; heap += size; return p; }
void rt_fatal_trace(void) { abort(); }

static bool query(FusionPiece *base, FusionPiece *appearance, FusionRecipe *recipe) {
    static FusionMaterial materials[] = {{7, 2}};
    materials[0].points = material_points;
    base->rank = appearance->rank = FUSION_LR;
    *recipe = (FusionRecipe){appearance->armor_id, 1, FUSION_LR, materials, 1};
    return true;
}

void fnhook_call_original(Cpu *c, uint32_t address) {
    switch (address) {
    case UPDATE:
        if (!confirm_in_update) return;
        c->r[14] = confirm_caller;
        hook(CONFIRM)(c);
        confirmation_result = c->r[0];
        if (c->r[0] == 1 && consume_in_update) {
            // Native charge and material deduction happen synchronously after
            // confirmation returns yes, inside this same native update.
            ++charges;
            c->r[0] = 999; c->r[1] = 1; c->r[14] = consume_caller;
            hook(CONSUME)(c);
            assert(c->r[0] == 0);
        }
        return;
    case CONFIRM: ++confirms; c->r[0] = native_answer; return;
    case CLOSE: ++closes; return;
    case 0x340d60: c->r[0] = native_pair_result; return;
    case 0x341060: ST32(c->r[3], zenny); return;
    case CONSUME:
        consumed_item = c->r[0]; consumed_quantity = c->r[1];
        ++deductions; c->r[0] = 0; return;
    case SMITH_UPDATE: case SMITH_CLOSE: case SMITH_DTOR: case SMITH_DELETE: return;
    default: assert(!"unexpected original guest function");
    }
}

static void guest_dispatch(Cpu *c, uint32_t address) {
    for (unsigned i = 0; i < hook_count; ++i) if (hooks[i].address == address) {
        hooks[i].fn(c); return;
    }
    switch (address) {
    case 0x343894: // Native equipment-box record lookup.
        assert(c->r[0] == BOX && c->r[2] < 2);
        c->r[0] = RECORDS + 8 * c->r[2]; return;
    case 0x74fe8c: assert(c->r[0] == 7); c->r[0] = ownership; return;
    case 0x723180: assert(c->r[0] == PLAYER); c->r[0] = c->r[1] <= 1000; return;
    case 0x20f248: c->r[0] = COORD; return;
    case 0x85fcec: return;
    case 0x5ea218: c->r[0] = 1; return;
    case 0x75d2b4: c->r[0] = COMMON; return;
    case 0x85cb3c:
        assert(c->r[0] == SMITH && c->r[1] < 30);
        c->r[0] = LD32(SMITH_CHILDREN + c->r[1] * 4); return;
    case 0x85cba0: case 0x85cbc4: case 0x85cc0c: {
        uint32_t children = LD32(c->r[0] + 0x34);
        assert(children && c->r[1] < LD32(c->r[0] + 0x30));
        ST8(LD32(children + c->r[1] * 4) + 0x18, address == 0x85cba0);
        ++ui_changes; return;
    }
    case 0x85e5bc: if (c->r[0] == 0x14) ++questions; return;
    case 0x85e794: return;
    case 0x8fcbcc:
        ST32(SMITH + 0x84, 0); ST32(SMITH + 0x88, 1); ST32(SMITH + 0x8c, 0);
        ST16(SMITH + 0x2ad4, 7); ST16(SMITH + 0x2ad6, ownership); return;
    case 0x8fa3d8: case 0x74edec: case 0x74ee34: return;
    case 0x8f8e48:
        ++refreshes;
        ST32(SMITH + 0x8c, LD32(SMITH + 0x84) ? material_points * LD16(SMITH + 0x2a56) : 0);
        return;
    case 0x8fb8e4: case 0x8fcea8: return;
    case 0x74ee98: c->r[0] = 0; return;
    case 0x394228: c->r[0] = ITEM + 0x2000; return;
    case 0x74fa74:
        if (poll_result == 1) ST16(SMITH + 0x3202, material_quantity);
        c->r[0] = poll_result; return;
    case 0x8fd448:
        ST32(SMITH + 0x84, 1); ST16(SMITH + 0x2a54, 7);
        ST16(SMITH + 0x2a56, LD16(SMITH + 0x2a56) + LD16(SMITH + 0x3202));
        return;
    case 0x8fb720: ST32(SMITH + 0x84, 0); ST16(SMITH + 0x2a56, 0); return;
    default: assert(!"unexpected guest function");
    }
}

void guest_call(Cpu *c, uint32_t address) {
    uint32_t lr = c->r[14];
    c->r[14] = 0;
    guest_dispatch(c, address);
    c->r[14] = lr;
}

static void input(unsigned pressed, unsigned held) {
    ST32(INPUT + 0x6bfc, pressed); ST32(INPUT + 0x6c34, held);
}
static unsigned frame(Cpu *c, unsigned pressed, unsigned held) {
    input(pressed, held); c->r[0] = COORD;
    hook(UPDATE)(c);
    return confirmation_result;
}
static void open_picker(Cpu *c) {
    input(ACCEPT, 0); c->r[0] = SMITH; hook(SMITH_UPDATE)(c);
    assert(frame(c, ACCEPT, 0) == 0);
    assert(refreshes == 1 && questions == 0 && charges == 0);
}
static void choose(Cpu *c, unsigned quantity) {
    // The native pool and its quantity widget each require a release update.
    assert(frame(c, 0, 0) == 0);
    assert(frame(c, ACCEPT, 0) == 0);
    assert(frame(c, 0, 0) == 0);
    material_quantity = quantity; poll_result = 1;
    assert(frame(c, ACCEPT, 0) == 0);
    poll_result = 0;
}
static void finish_picker(Cpu *c) {
    open_picker(c); choose(c, 5);
    assert(questions == 1 && confirms == 0 && charges == 0 && deductions == 0);
}
static void close_coordinate(Cpu *c) { c->r[0] = COORD; hook(CLOSE)(c); }

static void reset(Cpu *c) {
    memset(G2H(SMITH), 0, 0xe0000); memset(c, 0, sizeof *c); c->r[13] = 0x7000000;
    confirms = charges = deductions = refreshes = ui_changes = closes = questions = 0;
    poll_result = 0; native_answer = 1; material_quantity = 5;
    zenny = 500; ownership = 20; native_pair_result = 1; material_points = 2;
    consumed_item = consumed_quantity = 0;
    confirm_in_update = consume_in_update = true;
    confirm_caller = 0x860e7c; consume_caller = 0x860eb0;
    ST32(0x1a99458, FACILITY); ST32(FACILITY + 0x15c, SMITH);
    ST8(FACILITY + 0x198, 3); ST8(FACILITY + 0x199, 4);
    ST32(SMITH + 4, 1); ST32(SMITH + 0xc, 1); ST8(SMITH + 0x1f71, 4);
    ST32(SMITH + 0x34, SMITH_CHILDREN); ST32(SMITH + 0x30, 30);
    ST32(COORD + 4, 2); ST32(COORD + 0x34, COORD_CHILDREN); ST32(COORD + 0x30, 30);
    ST32(COORD + 0x84, 0); ST32(COORD + 0x88, 1);
    ST32(COMMON + 0x34, COMMON_CHILDREN); ST32(COMMON + 0x30, 6);
    for (unsigned i = 0; i < 30; ++i) {
        ST32(SMITH_CHILDREN + i * 4, WIDGETS + i * 0x100);
        ST32(COORD_CHILDREN + i * 4, WIDGETS + 0x2000 + i * 0x100);
        ST8(WIDGETS + i * 0x100 + 0x18, 1);
        ST8(WIDGETS + 0x2000 + i * 0x100 + 0x18, 1);
        if (i < 6) {
            ST32(COMMON_CHILDREN + i * 4, WIDGETS + 0x4000 + i * 0x100);
            ST8(WIDGETS + 0x4000 + i * 0x100 + 0x18, 1);
        }
    }
    ST32(WIDGETS + 0xe00 + 0x4c, SMITH + 0x2e20);
    ST32(WIDGETS + 0xf00 + 0x4c, SMITH + 0x2e20);
    ST32(WIDGETS + 0x1000 + 0x4c, SMITH + 0x1ff4);
    ST32(WIDGETS + 0x1000 + 0x50, SMITH + 0x2d50);
    ST32(WIDGETS + 0x1100 + 0x50, SMITH + 0x3200);
    ST32(0x1a8c670, INPUT); ST32(0x1a583a8, ITEM);
    ST32(ITEM + 8 + 0x414, ITEM + 0x1000); ST32(ITEM + 8 + 0x418, 1);
    ST8(ITEM + 0x2000 + 0x24, 2);
    ST32(0x1a56fe8, BOX); ST32(0x1a97f78, PLAYER); ST32(0x2326f48, 1);
    ST16(RECORDS + 4, 1); ST16(RECORDS + 6, 100);
    ST16(RECORDS + 12, 1); ST16(RECORDS + 14, 200);
    ST32(0x1a4d230, 0x1930844); ST32(0x1930844 + 8, 0x20f210);
    // Clear the forge opener's held-button latch before each new interaction.
    input(0, 0); c->r[0] = SMITH; hook(SMITH_UPDATE)(c);
    ST8(SMITH + 0x1f71, 4);
}

int main(void) {
#ifdef _WIN32
    void *memory = VirtualAlloc(G2H(0), MEMORY_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void *memory = mmap(G2H(0), MEMORY_SIZE, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
#endif
    assert(memory == G2H(0));
    // Native function signatures recognized by the two adapters.
    const uint32_t anchors[][2] = {
        {SMITH_UPDATE,0xe92d41f0}, {UPDATE,0xe92d4830}, {0x340b7c,0xe92d4070},
        {0x340d60,0xe92d4ff0}, {0x341060,0xe92d41f0}, {CONSUME,0xe92d4070},
        {CONFIRM,0xe92d4800}, {0x85fcec,0xe92d4010}, {0x5ea218,0xe92d4010},
        {0x20f248,0xe92d4070}, {0x85fe10,0xe92d4010}, {SMITH_CLOSE,0xe92d4800},
        {SMITH_DTOR,0xe92d4830}, {SMITH_DELETE,0xe92d4800}, {0x75bac0,0xe92d4070},
        {0x75d2b4,0xe0800101}, {0x85e5bc,0xe92d41f0}, {0x85e794,0xe92d4830},
        {0x8fcbcc,0xe92d4ff0}, {0x8fa3d8,0xe92d4ff0}, {0x8f8e48,0xe92d4ff0},
        {0x8fb8e4,0xe92d4ff0}, {0x8fcea8,0xe92d4ff0}, {0x8fd448,0xe92d4bf0},
        {0x8fb720,0xe92d48f0}, {0x74edec,0xe92d4800}, {0x74ee34,0xe1c011b4},
        {0x74ee98,0xe92d4ff0}, {0x74fa74,0xe92d4010}, {0x394228,0xe590001c},
        {0x85cb3c,0xe5900034}, {0x85cba0,0xe92d4010}, {0x85cc0c,0xe92d4010}
    };
    for (size_t i = 0; i < sizeof anchors / sizeof anchors[0]; ++i) ST32(anchors[i][0], anchors[i][1]);
    armorfusion_set_catalog(query); armorfusion_init(); assert(hook_count == 13);
    Cpu cpu;

    reset(&cpu); finish_picker(&cpu);
    assert(frame(&cpu, ACCEPT, ACCEPT) == 0);
    assert(frame(&cpu, 0, BACK) == 0); // Both buttons must be released.
    assert(frame(&cpu, 0, 0) == 0);    // Release update cannot itself approve.
    assert(confirms == 0 && charges == 0);
    assert(frame(&cpu, ACCEPT, 0) == 1);
    assert(confirms == 1 && charges == 1 && deductions == 1);
    assert(consumed_item == 7 && consumed_quantity == 5);
    assert(LD32(COORD + 0x48) == 500 && cpu.r[13] == 0x7000000);
    close_coordinate(&cpu);

    reset(&cpu); finish_picker(&cpu);
    assert(frame(&cpu, 0, 0) == 0);
    native_answer = 2;
    assert(frame(&cpu, BACK, 0) == 2);
    assert(charges == 0 && deductions == 0);
    native_answer = 1;
    assert(frame(&cpu, 0, 0) == 0); // Cancelled question requires new materials.
    assert(questions == 1 && refreshes == 3);
    close_coordinate(&cpu);

    // Revalidation must reject changes made after the material choice.
    for (unsigned change = 0; change < 4; ++change) {
        reset(&cpu); finish_picker(&cpu); assert(frame(&cpu, 0, 0) == 0);
        if (change == 0) ++zenny;
        if (change == 1) ownership = 4;
        if (change == 2) ST16(RECORDS + 14, 201);
        if (change == 3) native_pair_result = 0x401;
        assert(frame(&cpu, ACCEPT, 0) == 2);
        assert(charges == 0 && deductions == 0);
        close_coordinate(&cpu);
    }

    reset(&cpu); open_picker(&cpu); assert(frame(&cpu, 0, 0) == 0);
    assert(frame(&cpu, BACK, 0) == 2);
    assert(questions == 0 && charges == 0 && deductions == 0);
    close_coordinate(&cpu);

    // Teardown must discard cached guest addresses without restoring widgets.
    const uint32_t teardown[] = {SMITH_CLOSE, SMITH_DTOR, SMITH_DELETE};
    for (unsigned i = 0; i < 3; ++i) {
        reset(&cpu); open_picker(&cpu);
        unsigned before = ui_changes;
        cpu.r[0] = SMITH; hook(teardown[i])(&cpu);
        assert(closes == 1 && ui_changes == before);
        assert(charges == 0 && deductions == 0);
    }
    reset(&cpu); open_picker(&cpu);
    ST32(FACILITY + 0x15c, 0); // Parent changed without one of our teardown hooks.
    unsigned before = ui_changes;
    assert(frame(&cpu, 0, 0) == 2);
    assert(ui_changes == before);
    close_coordinate(&cpu); assert(ui_changes == before);

    // A native point value that crosses ten still completes after refresh.
    reset(&cpu); material_points = 3; ST8(ITEM + 0x2000 + 0x24, 3);
    open_picker(&cpu); choose(&cpu, 4);
    assert(questions == 1 && refreshes == 2);
    assert(frame(&cpu, 0, 0) == 0);
    assert(frame(&cpu, ACCEPT, 0) == 1);
    assert(charges == 1 && deductions == 1 && consumed_item == 7 && consumed_quantity == 4);
    close_coordinate(&cpu);

    // Other native confirmation callers and separation keep native behavior.
    reset(&cpu); open_picker(&cpu); confirm_caller = 0x1234;
    consume_in_update = false;
    assert(frame(&cpu, ACCEPT, 0) == 1 && confirms == 1);
    close_coordinate(&cpu);
    reset(&cpu); open_picker(&cpu); ST32(COORD + 0x44, 1);
    consume_caller = 0x1234;
    assert(frame(&cpu, ACCEPT, 0) == 1 && confirms == 1 && deductions == 1);
    assert(consumed_item == 999 && consumed_quantity == 1);
    close_coordinate(&cpu);

    // A partial quantity add needs one refresh before checking current points.
    reset(&cpu); open_picker(&cpu);
    unsigned prior_refreshes = refreshes;
    choose(&cpu, 1);
    assert(questions == 0 && charges == 0 && deductions == 0);
    assert(LD32(SMITH + 0x8c) == 2);
    assert(refreshes == prior_refreshes + 1);
    close_coordinate(&cpu);

#ifdef _WIN32
    assert(VirtualFree(memory, 0, MEM_RELEASE));
#else
    assert(munmap(memory, MEMORY_SIZE) == 0);
#endif
    puts("native armor fusion transition checks passed");
    return 0;
}
