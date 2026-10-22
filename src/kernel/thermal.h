/* thermal — reads the temperature, asks the policy, and moves the CPU.
 *
 * This is the wiring, not the decision: `core/cpufreq.h` decides and is
 * tested on the host, and the sensor and the clock are ports, so the whole
 * chain can be driven by a fake on the emulator (`minikern.cpufreq=test`).
 * What cannot be tested anywhere but on the board is the two adapters
 * underneath — QEMU models neither the H3's thermal sensor nor its PLLs —
 * which is why the real thing is off unless the command line asks for it.
 *
 * Load is measured the only way a kernel can measure it honestly: the share
 * of ticks on which a core had something to run. It is sampled over the
 * interval between decisions, not instantaneously, because one tick's
 * answer is either 0 or 100 and tells you nothing.
 */
#ifndef MINIKERN_KERNEL_THERMAL_H
#define MINIKERN_KERNEL_THERMAL_H

#include <stdbool.h>
#include <stdint.h>

#include "core/cpufreq.h"
#include "core/ports.h"

/* Starts the governor: it decides every interval_ms from then on. The
 * policy is copied, and is refused if cpufreq_policy_valid says no — a
 * board's table is checked before it is acted on, never after. */
int thermal_start(const struct thermal_port *sensor, const struct cpuclock_port *clock,
                  const struct cpufreq_policy *policy, uint32_t interval_ms);

/* One decision, which the time hook calls. Exposed so a test can step it
 * without waiting for a wall clock. */
void thermal_step(void);

/* The last temperature read and the point the CPU is at, for the log and
 * for a test to assert against. */
void thermal_state(int32_t *temp_mC, uint32_t *khz, uint32_t *opp);

/* Prints where the governor is, like the other report functions. */
void thermal_report(void);

/* How many sensor readings have been rejected as impossible. A board that
 * reports any of these has a sensor that is not wired, not clocked or not
 * calibrated, and has been running on the worst-case assumption since the
 * first one — which is safe, and worth saying out loud. */
uint32_t thermal_unbelievable_readings(void);

#endif
