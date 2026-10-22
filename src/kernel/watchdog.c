#include "kernel/watchdog.h"

#include "core/errno.h"
#include "kernel/time.h"

#include <stdbool.h>
#include <stddef.h>

static struct watchdog_port dog;
static bool running;

/* Feeds it from the tick. The tick is the signal: if this stops being
 * called, the kernel has stopped taking interrupts and deserves the
 * reboot. */
static void on_tick(void *self, uint64_t now_ns) {
    (void)self;
    (void)now_ns;
    dog.pet();
}

uint32_t watchdog_start(const struct watchdog_port *port, uint32_t seconds) {
    struct tick_hook hook = {.tick = on_tick, .self = NULL};
    uint32_t got;
    dog = *port;
    got = dog.start(seconds);
    if (got == 0 || time_add_hook(&hook) != KERR_OK) {
        dog.stop();
        return 0;
    }
    running = true;
    return got;
}

void watchdog_stop(void) {
    if (running) {
        dog.stop();
        running = false;
    }
}
