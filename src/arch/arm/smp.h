/* smp — the H3's other three cores.
 *
 * All four run user code. Seven assumptions that were correct on one core
 * turned out to be bugs on four; they are listed in TASKS.md P10.3.
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
 *
 * **Parking** (`minikern.cpuoff=1`): a core with nothing to run can power
 * itself off with PSCI CPU_OFF rather than idle in `wfi`, and is started
 * again when work appears. On this chip that saves more than stepping the
 * frequency does, because an idle A7 that is *off* draws no core power at
 * all, while one in `wfi` still has its clock and its voltage.
 *
 * What makes it safe is one invariant: **the boot core never parks**. A
 * wake can always race a power-off — the firmware reports ALREADY_ON for a
 * core that has decided to go but has not gone — and the fix is not to win
 * that race but to survive losing it: a core that is still ticking retries
 * the wake, so a lost one costs a millisecond, the same millisecond an
 * idle core already costs to notice work. Without a core that cannot park,
 * the last wake could be lost with nobody left to retry it.
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

/* How many cores the scheduler may actually use: every core that has been
 * released into it, or 1 before that. This is what user space is told when
 * it asks how many CPUs it has, and Go sizes GOMAXPROCS from it. */
uint32_t smp_scheduling(void);

/* How many cores have reported themselves, including this one. A core that
 * has parked still counts: it has not gone away, and it comes back when
 * there is work for it (see the parking note above). */
uint32_t smp_online(void);

/* Which core this is, from MPIDR. */
uint32_t smp_cpu_id(void);

/* True when that core is up. */
bool smp_is_online(uint32_t cpu);

/* Lets idle cores power themselves off (`minikern.cpuoff=1`). Off by
 * default: an emulator spends no power, so until the kernel runs on a
 * board there is nothing to measure and no reason to change how every boot
 * schedules. */
void smp_allow_parking(bool allow);

/* True when this core is allowed to power itself off: parking is on, the
 * cores have been released, and this is not the boot core. */
bool smp_may_park(void);

/* Powers this core off, to be started again by smp_wake_parked. The
 * **kernel lock must be released first** (klock.h): a core holding it
 * while off is a kernel that never runs again. It returns only if the
 * firmware refuses, or if a wake arrived in the meantime. */
void smp_park_self(void);

/* True when any core is parked — a cheap check for the scheduler's hot
 * path, which must not make an SMC call per wakeup. */
bool smp_any_parked(void);

/* Starts every parked core again. Safe to call repeatedly: a core that is
 * on its way down reports ALREADY_ON and is retried on the next tick. */
void smp_wake_parked(void);

/* How many times a core has powered itself off, for the log and the test. */
uint32_t smp_parks(void);

#endif
