/* ths — the H3's thermal sensor.
 *
 * **Unverified against silicon**, for the same reason as cpux.h: QEMU does
 * not model the sensor, so a reading has never been compared with a real
 * temperature. The register block and the conversion from a raw sample to
 * milli-degrees are datasheet facts that CLAUDE.md §3 lists as needing
 * verification on the board.
 *
 * The fake ramps, which is what makes the governor testable: a temperature
 * that climbs past the throttle point and then falls proves the policy is
 * wired to something, which is the half that can be proved here.
 */
#ifndef MINIKERN_BOARD_H3_THS_H
#define MINIKERN_BOARD_H3_THS_H

#include "core/ports.h"

/* Powers up the sensor and returns a port over it. */
struct thermal_port h3_thermal_port(void);

/* A sensor that climbs from cold to above critical and falls back, a step
 * per reading, so one boot walks the whole policy. */
struct thermal_port h3_thermal_fake_port(void);

/* A sensor that answers with a value no silicon could produce, for proving
 * that the governor does not act on one. `minikern.cpufreq=test-badsensor`
 * uses it with -264000 mC, which is what this chip's conversion makes of an
 * all-ones sample — the reading from a sensor block that is absent or has
 * never been clocked. */
struct thermal_port h3_thermal_nonsense_port(int32_t mC);

#endif
