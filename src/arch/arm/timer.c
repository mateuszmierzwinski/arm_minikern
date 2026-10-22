#include "arch/arm/timer.h"

#define CNTV_CTL_ENABLE 1u
#define NS_PER_SEC 1000000000ull

static uint64_t deadline;
static uint32_t tick_period;

uint64_t timer_count(void) {
    uint32_t lo, hi;
    __asm__ volatile("isb\n\tmrrc p15, 1, %0, %1, c14" : "=r"(lo), "=r"(hi));
    return ((uint64_t)hi << 32) | lo;
}

uint64_t timer_count_to_ns(uint64_t count, uint32_t hz) {
    return (count / hz) * NS_PER_SEC + (count % hz) * NS_PER_SEC / hz;
}

static void set_compare(uint64_t cval) {
    __asm__ volatile("mcrr p15, 3, %0, %1, c14\n\tisb" : : "r"((uint32_t)cval), "r"((uint32_t)(cval >> 32)));
}

static uint32_t tick_period;

/* Starts this core's own tick, with the period the primary chose. A core
 * without a tick never preempts and never notices work appearing. */
void timer_start_cpu(void) {
    if (tick_period != 0) {
        timer_start_periodic(tick_period);
    }
}

void timer_start_periodic(uint32_t period) {
    tick_period = period;
    tick_period = period;
    deadline = timer_count() + period;
    set_compare(deadline);
    __asm__ volatile("mcr p15, 0, %0, c14, c3, 1\n\tisb" : : "r"(CNTV_CTL_ENABLE));
}

/* Skips deadlines already past (after a long masked stretch) rather than
 * firing a burst of stale ticks. */
void timer_ack(void) {
    uint64_t now = timer_count();
    do {
        deadline += tick_period;
    } while (deadline <= now);
    set_compare(deadline);
}
