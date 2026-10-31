#define _GNU_SOURCE
#include <assert.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#include "platform.h"
#include "platform/linux_context.h"

static volatile sig_atomic_t touches;
static void touch(uint64_t address, int write) {
    (void)address;
    (void)write;
    ++touches;
}
const char *rt_host_fn_name(uint64_t pc, uint64_t *off) {
    (void)pc; (void)off; return NULL;
}
void rt_fault_report(const char *what, uint64_t address, uint64_t pc,
                     uint64_t lr) {
    (void)what; (void)address; (void)lr;
    _exit(pc ? 42 : 43);
}

int main(void) {
    ucontext_t context = {0};
#if defined(__aarch64__)
    assert(linux_fault_write(&context) == -1);
    uint32_t record[] = {0x45535201u, 16};
    uint64_t esr = (UINT64_C(0x24) << 26) | 64;
    memcpy(context.uc_mcontext.__reserved, record, sizeof record);
    memcpy(context.uc_mcontext.__reserved + 8, &esr, 8);
    assert(linux_fault_write(&context) == 1);
    esr &= ~UINT64_C(64);
    memcpy(context.uc_mcontext.__reserved + 8, &esr, 8);
    assert(linux_fault_write(&context) == 0);
    esr = UINT64_C(0x20) << 26;
    memcpy(context.uc_mcontext.__reserved + 8, &esr, 8);
    assert(linux_fault_write(&context) == -1);
    // Unknown records can precede ESR; the terminator must not be read as ESR.
    unsigned char *records = context.uc_mcontext.__reserved;
    memmove(records + 16, records, 16);
    record[0] = 0x46508001u;
    memcpy(records, record, sizeof record);
    esr = (UINT64_C(0x25) << 26) | 64;
    memcpy(records + 24, &esr, 8);
    assert(linux_fault_write(&context) == 1);
    memset(records, 0, sizeof context.uc_mcontext.__reserved);
    assert(linux_fault_write(&context) == -1);
    record[0] = 0x45535201u;
    record[1] = 65536;
    memcpy(context.uc_mcontext.__reserved, record, sizeof record);
    assert(linux_fault_write(&context) == -1);
    record[1] = 8;
    memcpy(context.uc_mcontext.__reserved, record, sizeof record);
    assert(linux_fault_write(&context) == -1);
#else
    assert(linux_fault_write(&context) == 0);
    context.uc_mcontext.gregs[REG_ERR] = 2;
    assert(linux_fault_write(&context) == 1);
#endif
    const uint64_t base = UINT64_C(0x10000000000);
    assert(plat_reserve_guest(base, UINT64_C(0x100000000)));
    plat_crash_handler_install();
    plat_touch_hook = touch;
    volatile uint32_t *read_page = (void *)(uintptr_t)(base + 0x1000);
    volatile uint32_t *write_page = (void *)(uintptr_t)(base + 0x21000);
    assert(*read_page == 0);
    *write_page = 0x12345678;
    assert(*write_page == 0x12345678);
    assert(touches == 2);
    uint64_t end;
    assert(plat_committed((uint64_t)(uintptr_t)read_page, &end));
    assert(plat_committed((uint64_t)(uintptr_t)write_page, &end));
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        raise(SIGILL);
        _exit(44);
    }
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 42);
    puts("actual lazy guest-page faults and fatal signal context: PASS");
    return 0;
}
