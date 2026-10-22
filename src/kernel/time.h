/* time — kernel time: the monotonic clock from the generic timer, the
 * wall clock (an offset over it), and the periodic tick that drives
 * preemption and timed sleeps. */
#ifndef MINIKERN_KERNEL_TIME_H
#define MINIKERN_KERNEL_TIME_H

#include <stdint.h>

#define TIME_HZ 1000u  /* tick rate: sleep and preemption granularity is 1 ms */
#define NS_PER_SEC 1000000000ull

/* Starts the tick (registers the timer interrupt). epoch_sec is the wall
 * clock at boot, in seconds since 1970. */
void time_init(uint32_t epoch_sec);

/* Nanoseconds since boot. */
uint64_t time_monotonic_ns(void);

/* Nanoseconds since 1970. */
uint64_t time_realtime_ns(void);

/* Something run on every tick, in interrupt context (IRQs masked). */
struct tick_hook {
    void (*tick)(void *self, uint64_t now_ns);
    void *self;
};

#define TIME_MAX_HOOKS 4u

/* Adds a tick hook. KERR_OK, or KERR_NOMEM when all slots are taken. */
int time_add_hook(const struct tick_hook *hook);

/* Ticks taken so far (for the boot log and tests). */
uint64_t time_ticks(void);

#endif
