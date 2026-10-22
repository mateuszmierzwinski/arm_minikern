#include "kernel/time.h"

#include "arch/arm/cpu.h"
#include "arch/arm/timer.h"
#include "kernel/irq.h"
#include "kernel/panic.h"
#include "kernel/sched.h"

static uint32_t timer_hz;
static uint64_t boot_count;
static uint64_t epoch_ns;
static uint64_t ticks;
static struct tick_hook hooks[TIME_MAX_HOOKS];

int time_add_hook(const struct tick_hook *hook) {
    for (uint32_t i = 0; i < TIME_MAX_HOOKS; i++) {
        if (hooks[i].tick == NULL) {
            hooks[i] = *hook;
            return 0;
        }
    }
    return -12;  /* ENOMEM */
}

static void tick(void *self) {
    uint64_t now = time_monotonic_ns();
    (void)self;
    timer_ack();
    ticks++;
    for (uint32_t i = 0; i < TIME_MAX_HOOKS; i++) {
        if (hooks[i].tick != NULL) {
            hooks[i].tick(hooks[i].self, now);
        }
    }
    sched_tick(now);
}

void time_init(uint32_t epoch_sec) {
    static const struct irq_handler tick_handler = {.handle = tick};
    timer_hz = arm_cntfrq();
    boot_count = timer_count();
    epoch_ns = (uint64_t)epoch_sec * NS_PER_SEC;
    if (irq_register(TIMER_VIRT_PPI, &tick_handler) != 0) {
        panic("time: timer interrupt already claimed");
    }
    timer_start_periodic(timer_hz / TIME_HZ);
}

uint64_t time_monotonic_ns(void) {
    return timer_count_to_ns(timer_count() - boot_count, timer_hz);
}

uint64_t time_realtime_ns(void) {
    return epoch_ns + time_monotonic_ns();
}

uint64_t time_ticks(void) {
    return ticks;
}
