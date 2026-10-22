/* cpux — the H3's CPU clock and the voltage that feeds it.
 *
 * **What is verified, and where.** QEMU does model the clock block
 * (`allwinner-h3-ccu`), so the register sequence here is driven for real by
 * `minikern.cpufreq=probe`: each published operating point is set through
 * this adapter and PLL_CPUX is read back and decoded, which checks the
 * factor arithmetic, the field placement, the clock-source handover and
 * every refusal path against a model of the real block. That is the same
 * standard the EMAC driver was first held to (CLAUDE.md §3).
 *
 * It is **not** silicon, and two things remain that only the board can
 * settle: whether the PLL actually locks (QEMU's model never raises the
 * lock bit, so the adapter's timeout path is what runs here — it is
 * exercised, and it refuses to switch to an unlocked PLL, which is the
 * behaviour that matters), and the regulator pin, since QEMU models no PIO.
 * The thermal sensor is the third (board/h3/ths.h) and has no model at all,
 * which is why the governor runs against a fake unless asked otherwise.
 *
 * The order matters and is the reason the clock and the regulator are one
 * port: raise the voltage before the frequency, lower it after. The other
 * order browns out the core.
 */
#ifndef MINIKERN_BOARD_H3_CPUX_H
#define MINIKERN_BOARD_H3_CPUX_H

#include "core/gpio.h"
#include "core/ports.h"

/* Names the pin that switches VDD_CPUX, which unlocks the operating points
 * the lower supply cannot feed.
 *
 * Which pin it is belongs to the board, not the chip: it is in the
 * schematic in front of you and differs between boards that share this
 * H3, so it is given on the kernel command line
 * (`minikern.vddcpux=`) rather than compiled in as a guess. Without it,
 * only the points the fixed supply can feed are offered — which is the
 * safe direction to be wrong in. */
void h3_cpuclock_use_vdd_pin(struct gpio_pin pin);

/* The port over the real PLL and regulator. */
struct cpuclock_port h3_cpuclock_port(void);

/* A port that changes nothing and only remembers, so the governor and the
 * policy can be exercised where the hardware cannot be. */
struct cpuclock_port h3_cpuclock_fake_port(void);

/* Drives the real adapter through a list of operating points and prints
 * what each register actually took, decoded — the clock half of the
 * hardware path, which QEMU's CCU model can judge even though the board is
 * not here. `minikern.cpufreq=probe` calls it. */
void h3_cpuclock_probe(const uint32_t *khz, const uint32_t *mv, uint32_t n);

/* What the fake was last asked for. */
void h3_cpuclock_fake_last(uint32_t *khz, uint32_t *mv, uint32_t *changes);

#endif
