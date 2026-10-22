#include "kernel/selftest.h"

#include <stdint.h>

#include "core/console.h"

/* 0x10: low user space, which has no mapping until a process exists.
 * Volatile, so the compiler cannot see (and reject) the constant address. */
static volatile uintptr_t unmapped_addr = 0x10u;

static int name_is(const char *name, unsigned len, const char *want) {
    unsigned i = 0;
    while (i < len && want[i] != '\0' && name[i] == want[i]) {
        i++;
    }
    return i == len && want[i] == '\0';
}

void selftest_run(const char *name, unsigned len) {
    kprintf("selftest: %.*s\n", (int)len, name);
    if (name_is(name, len, "undef")) {
        __asm__ volatile("udf #0");
    } else if (name_is(name, len, "dabt")) {
        (void)*(volatile uint32_t *)unmapped_addr;
    } else if (name_is(name, len, "pabt")) {
        ((void (*)(void))unmapped_addr)();
    } else {
        kprintf("selftest: unknown, ignored\n");
    }
}
