#include "kernel/panic.h"

#include <stdarg.h>

#include "core/console.h"

static struct power_port panic_power;
static int panic_poweroff;

void panic_attach(const struct power_port *power, int poweroff) {
    panic_power = *power;
    panic_poweroff = poweroff;
}

void panic(const char *fmt, ...) {
    va_list ap;
    __asm__ volatile("cpsid if" ::: "memory");
    va_start(ap, fmt);
    kprintf("\nPANIC: ");
    kvprintf(fmt, ap);
    va_end(ap);
    kprintf("\n");
    if (panic_poweroff && panic_power.poweroff != NULL) {
        panic_power.poweroff(panic_power.self);
    }
    for (;;) {
        __asm__ volatile("wfi");
    }
}
