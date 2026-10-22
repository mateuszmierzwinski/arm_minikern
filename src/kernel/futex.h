/* futex — Linux's fast user-space mutex wait queue, the one primitive Go's
 * runtime builds its locks, notes and thread parking on. A waiter sleeps on
 * the channel (mm, uaddr): the address space plus the user address, so two
 * processes' futexes at the same address never meet. Because the kernel is
 * not preemptible, "check the word, then sleep" is atomic by construction. */
#ifndef MINIKERN_KERNEL_FUTEX_H
#define MINIKERN_KERNEL_FUTEX_H

#include <stdint.h>

#include "kernel/mm.h"

/* FUTEX_WAIT: sleeps if *uaddr == val until woken, the deadline
 * (monotonic ns, 0 = none) or a signal. Returns 0, KERR_AGAIN (the value
 * differed), KERR_TIMEDOUT, KERR_INTR or KERR_FAULT. */
int futex_wait(struct mm *mm, uint32_t uaddr, uint32_t val, uint64_t deadline_ns);

/* FUTEX_WAKE: wakes up to n waiters; returns how many. */
uint32_t futex_wake(struct mm *mm, uint32_t uaddr, uint32_t n);

#endif
