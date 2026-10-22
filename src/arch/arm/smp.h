/* smp — the H3's other three cores.
 *
 * **Stage two is not finished.** The cores come up, the kernel lock is in
 * place and each core initialises what is its own; but letting them
 * schedule still hangs shortly after a second thread appears, so
 * `smp_release` is called only for `minikern.smp=all`. What is known to
 * remain is in TASKS.md P10.3.
 *
 * Bringing them up and letting them work are two separate steps on
 * purpose. `smp_start_secondaries` starts them and they wait in a holding
 * pen: the MMU on, their own stack, touching nothing. `smp_release` — the
 * last thing the boot path does — lets them into the scheduler, because
 * until then the primary is running kernel code without the kernel lock
 * (klock.h rule 4) and a second core in there would race it.
 *
 * What makes this safe once they are in is the lock: **one core is inside
 * the kernel at a time** (klock.h), so every structure written for a single
 * core stays correct. The parallelism is in user space, which is where a Go
 * program spends its time.
 *
 * Not yet: an interrupt to tell an idle core that work has appeared. It
 * notices on its next tick instead, a millisecond later — a latency, not a
 * correctness problem (TASKS.md P10.4).
 */
#ifndef MINIKERN_ARCH_ARM_SMP_H
#define MINIKERN_ARCH_ARM_SMP_H

#include <stdbool.h>
#include <stdint.h>

/* The H3 has four Cortex-A7s. */
#define SMP_MAX_CPUS 4u

/* Starts every core but this one, with PSCI CPU_ON, and waits briefly for
 * each to report itself. Returns how many are up, including this one. A
 * core that does not answer is left alone and logged: a kernel that runs on
 * one core is the kernel we already had. */
uint32_t smp_start_secondaries(void);

/* Lets every started core into the kernel: each one sets up its own vector
 * table, interrupt interface and tick, adopts its idle context and starts
 * scheduling. Called once, at the end of boot, by the core that booted. */
void smp_release(void);

/* How many cores have reported themselves, including this one. */
uint32_t smp_online(void);

/* Which core this is, from MPIDR. */
uint32_t smp_cpu_id(void);

/* True when that core is up. */
bool smp_is_online(uint32_t cpu);

#endif
