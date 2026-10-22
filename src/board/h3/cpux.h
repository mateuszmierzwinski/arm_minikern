/* cpux — the H3's CPU clock and the voltage that feeds it.
 *
 * **Unverified against silicon.** QEMU models neither the PLL nor the
 * regulator, so nothing here has ever run on hardware; the register
 * addresses come from the datasheet and are marked in CLAUDE.md §3 as
 * needing verification when first used. The policy that drives it is tested
 * (core/cpufreq.h), and the whole chain can be exercised with a fake
 * (minikern.cpufreq=test) — but a real frequency change is one of the two
 * things in this project that only the board can confirm, alongside the
 * EMAC's PHY setup.
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

/* What the fake was last asked for. */
void h3_cpuclock_fake_last(uint32_t *khz, uint32_t *mv, uint32_t *changes);

#endif
