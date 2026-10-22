/* klock — one lock around the whole kernel.
 *
 * This is stage one of SMP, and it is deliberately the crudest thing that
 * is correct: **one core is inside the kernel at a time.** Every structure
 * the kernel owns — the run queue, the page allocator, the heap, each
 * process's address space and descriptors, the network stack, the buffer
 * cache — was written when "interrupts are masked" was the lock
 * (CLAUDE.md §2), and this preserves exactly that property on four cores
 * without auditing any of them.
 *
 * The payoff is not kernel parallelism, which this gives up on purpose: it
 * is that **user space runs on four cores**, which is where a Go program
 * spends its time. Finer locks come later, and only where profiling says
 * (TASKS.md P10.4).
 *
 * The rules, which are what make it work:
 *
 *   1. The lock is taken on entry to the kernel and released on the way
 *      out (`trap_dispatch`). It is **not recursive**: a core in the
 *      kernel has interrupts masked, so it cannot enter twice.
 *   2. **A context switch hands the lock over.** It belongs to the core,
 *      not to the stack: whoever is switched in inherits it and whoever
 *      eventually leaves the kernel releases it. So every switch happens
 *      with the lock held, and after a switch returns, the caller still
 *      holds it.
 *   3. The only place that gives it up voluntarily is an idle core, around
 *      `wfi` — it has nothing to protect while it is asleep.
 *   4. The boot path does not hold it, and must not: the other cores are
 *      not in the kernel until `smp_release` lets them in, which is the
 *      last thing the boot path does.
 *   5. **Kernel code that waits for an interrupt gives the lock up while it
 *      waits** (`irq_wait`), because the interrupt it is waiting for
 *      arrives through `trap_dispatch`, which takes the lock — on the same
 *      core, which would deadlock against itself. The consequence is the
 *      usual one: anything read before such a wait must be read again
 *      after it, because another core may have been in the kernel
 *      meanwhile. Every caller already loops on a condition, which is why
 *      this is safe rather than merely necessary.
 */
#ifndef MINIKERN_KERNEL_KLOCK_H
#define MINIKERN_KERNEL_KLOCK_H

#include <stdbool.h>
#include <stdint.h>

/* Takes and releases the kernel lock. */
void klock_acquire(void);
void klock_release(void);

/* True when some core holds it. For assertions in code that must only run
 * inside the kernel. */
bool klock_held(void);

/* True when **this** core holds it, which is what a wait needs to know
 * before it gives it up. */
bool klock_held_here(void);

/* How many times it has been taken, and how often a core had to wait:
 * the only contention this kernel can report, printed by minikern.netstat
 * so a slow board can be told from a contended one. */
void klock_stats(uint64_t *acquires, uint64_t *contended);

/* Prints how many times each core has been inside the kernel. A zero for a
 * core that is supposed to be scheduling means it is not being used, which
 * is the one thing an "N cores up" line cannot tell you. */
void klock_report(void);

#endif
