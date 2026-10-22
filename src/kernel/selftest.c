#include "kernel/selftest.h"

#include <stdint.h>

#include "arch/arm/layout.h"
#include "core/console.h"
#include "kernel/dma.h"

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

static void take_undef(void) {
    __asm__ volatile("udf #0");
}

static void take_dabt(void) {
    (void)*(volatile uint32_t *)unmapped_addr;
}

static void take_pabt(void) {
    ((void (*)(void))unmapped_addr)();
}

/* The DMA window is unmapped from the cacheable linear map when it is
 * created, so that one physical page is never reachable under two
 * cacheabilities (kernel/dma.c). Reading it by its linear address must
 * therefore abort — which is the only half of that fix QEMU can judge,
 * since it models no caches at all. */
static void take_dma_alias(void) {
    uint32_t phys = dma_window_phys();
    if (phys == 0) {
        kprintf("selftest: no dma window (the network is off)\n");
        return;
    }
    kprintf("selftest: reading the retired dma alias at %p\n", phys_to_virt(phys));
    (void)*(volatile uint32_t *)(uintptr_t)phys_to_virt(phys);
}

/* Wedges the kernel the way a deadlock does: interrupts masked and no way
 * out. Nothing can report this — no print, no panic, no question answered —
 * which is exactly why the watchdog exists, and this is how it is tested:
 * with `minikern.watchdog=`, the machine must reboot. */
static void take_wedge(void) {
    kprintf("selftest: wedging the kernel with interrupts masked\n");
    __asm__ volatile("cpsid if" ::: "memory");
    for (;;) {
        __asm__ volatile("" ::: "memory");
    }
}

/* Prints the real-time clock's registers raw, so that what they hold can be
 * compared with what the host says the time is. A measurement, not a
 * driver: the layout is a datasheet claim until it is read back. */
static void take_rtc(void) {
    static const uint32_t RTC_BASE = 0x01F00000u;
    for (uint32_t off = 0; off <= 0x14u; off += 4u) {
        kprintf("selftest: rtc+%02x = %08x\n", off, *(volatile uint32_t *)io_virt(RTC_BASE + off));
    }
}

/* One deliberate fault per name: the C form of master Rule 6.3, so adding
 * a case is a row rather than another branch. */
static const struct {
    const char *name;
    void (*take)(void);
} CASES[] = {
    {"undef", take_undef},
    {"dabt", take_dabt},
    {"pabt", take_pabt},
    {"dma-alias", take_dma_alias},
    {"wedge", take_wedge},
    {"rtc", take_rtc},
};

void selftest_run(const char *name, unsigned len) {
    kprintf("selftest: %.*s\n", (int)len, name);
    for (unsigned i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        if (name_is(name, len, CASES[i].name)) {
            CASES[i].take();
            return;
        }
    }
    kprintf("selftest: unknown, ignored\n");
}
