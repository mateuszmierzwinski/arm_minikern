/* timer — the ARMv7 generic timer's virtual channel: the free-running
 * counter for time, and a compare value for the periodic tick (PPI 27 on a
 * GIC). Its frequency is read from CNTFRQ, never assumed (CLAUDE.md §3). */
#ifndef MINIKERN_ARCH_ARM_TIMER_H
#define MINIKERN_ARCH_ARM_TIMER_H

#include <stdint.h>

#define TIMER_VIRT_PPI 27u  /* the virtual timer's interrupt ID on a GIC */

/* The counter: CNTVCT, monotonic since reset. */
uint64_t timer_count(void);

/* Counter ticks to nanoseconds without 64-bit overflow for ~585 years. */
uint64_t timer_count_to_ns(uint64_t count, uint32_t hz);

/* Starts a periodic interrupt every period counts. timer_ack re-arms it
 * from the previous deadline, so ticks never drift. */
void timer_start_periodic(uint32_t period);

/* Starts this core's tick with the period already chosen, for a core that
 * came up after the primary. */
void timer_start_cpu(void);
void timer_ack(void);

#endif
