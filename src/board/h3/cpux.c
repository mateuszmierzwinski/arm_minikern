#include "board/h3/cpux.h"

#include "arch/arm/layout.h"
#include "board/h3/h3.h"
#include "board/h3/pio.h"
#include "core/errno.h"

/* PLL_CPUX and the CPU clock source. Datasheet addresses, **never run on
 * silicon** (cpux.h). */
#define PLL_CPUX_CTRL 0x0000u /* CCU + 0x00 */
#define CPU_AXI_CFG   0x0050u /* CCU + 0x50: which clock the cores run on */

#define PLL_ENABLE    (1u << 31)
#define PLL_LOCK      (1u << 28)
#define PLL_P_SHIFT   16u
#define PLL_N_SHIFT   8u
#define PLL_K_SHIFT   4u
#define PLL_M_SHIFT   0u

#define CPU_SRC_SHIFT 16u
#define CPU_SRC_OSC24 1u      /* the 24 MHz crystal: safe while the PLL moves */
#define CPU_SRC_PLL   2u

#define OSC_KHZ       24000u
#define PLL_LOCK_SPINS 100000u

/* The two voltages the NanoPi NEO can supply, selected by a GPIO rather
 * than by a programmable regulator: this board has a two-level supply, so a
 * point's voltage is rounded **up** to one of these. Asking for less than
 * a frequency needs is the one mistake that is not recoverable. */
#define VDD_LOW_MV   1100u
#define VDD_HIGH_MV  1300u

static volatile uint32_t *ccu(uint32_t off) {
    return (volatile uint32_t *)io_virt(H3_CCU_BASE + off);
}

/* n, k, m and p for a frequency, as PLL_CPUX = 24 MHz * N * K / (M * P).
 * Only N is varied: every published H3 point is a multiple of 24 MHz, so
 * k = m = 1 and p = 1 reach all of them, and a single varying factor cannot
 * produce an out-of-range intermediate frequency. */
static int pll_factors(uint32_t khz, uint32_t *n) {
    if (khz < OSC_KHZ || khz % OSC_KHZ != 0) {
        return KERR_INVAL;
    }
    *n = khz / OSC_KHZ;
    return *n >= 1u && *n <= 32u ? KERR_OK : KERR_INVAL;
}

/* Waits for the PLL to report lock. A PLL that never locks must not be
 * switched to, so this is checked rather than assumed. */
static int pll_wait_lock(void) {
    for (uint32_t spin = 0; spin < PLL_LOCK_SPINS; spin++) {
        if ((*ccu(PLL_CPUX_CTRL) & PLL_LOCK) != 0) {
            return KERR_OK;
        }
    }
    return KERR_TIMEDOUT;
}

/* Points the cores at a clock source. */
static void cpu_source(uint32_t src) {
    uint32_t v = *ccu(CPU_AXI_CFG) & ~(3u << CPU_SRC_SHIFT);
    *ccu(CPU_AXI_CFG) = v | (src << CPU_SRC_SHIFT);
}

/* The board's supply level for a requested voltage, rounded up. */
static uint32_t supply_for(uint32_t mv) {
    return mv <= VDD_LOW_MV ? VDD_LOW_MV : VDD_HIGH_MV;
}

/* The pin that switches the supply, once a board has told us which it is. */
static struct gpio_pin vdd_pin;
static bool vdd_pin_known;

void h3_cpuclock_use_vdd_pin(struct gpio_pin pin) {
    vdd_pin = pin;
    vdd_pin_known = true;
    h3_pio_output(pin);
    h3_pio_write(pin, false); /* start low: the voltage the board boots at */
}

/* Selects the supply level. Without a pin, the only honest thing is to do
 * nothing — and `cpux_set` refuses the points that would need the higher
 * level, so nothing asks for one. */
static void set_supply(uint32_t mv) {
    if (vdd_pin_known) {
        h3_pio_write(vdd_pin, mv > VDD_LOW_MV);
    }
}

/* True when the board can supply this voltage. */
static bool can_supply(uint32_t mv) {
    return vdd_pin_known || supply_for(mv) <= VDD_LOW_MV;
}

/* Moves the CPU to khz at mv.
 *
 * The cores are parked on the crystal while the PLL moves, because a core
 * running on a PLL that is being reprogrammed is a core running on an
 * undefined frequency. Runs to 28 lines: the sequence is one ordered
 * handover — voltage, park, reprogram, lock, unpark — and splitting it
 * would move the park and the unpark into different functions, leaving the
 * cores on the crystal if the caller forgot the second half (Rule 11,
 * Control flow). */
static int cpux_set(void *self, uint32_t khz, uint32_t mv) {
    uint32_t n = 0, ctrl;
    int rc = pll_factors(khz, &n);
    (void)self;
    if (rc != KERR_OK) {
        return rc;
    }
    if (!can_supply(mv)) {
        return KERR_NOTSUP; /* no pin to raise the supply with: refuse, do not brown out */
    }
    if (supply_for(mv) > VDD_LOW_MV) {
        set_supply(supply_for(mv)); /* up before the frequency */
    }
    cpu_source(CPU_SRC_OSC24);
    ctrl = *ccu(PLL_CPUX_CTRL);
    ctrl &= ~((31u << PLL_N_SHIFT) | (3u << PLL_K_SHIFT) | (3u << PLL_M_SHIFT) | (3u << PLL_P_SHIFT));
    ctrl |= ((n - 1u) << PLL_N_SHIFT) | PLL_ENABLE;
    *ccu(PLL_CPUX_CTRL) = ctrl;
    rc = pll_wait_lock();
    if (rc != KERR_OK) {
        cpu_source(CPU_SRC_PLL); /* back where it was: the old factors still stand */
        return rc;
    }
    cpu_source(CPU_SRC_PLL);
    if (supply_for(mv) == VDD_LOW_MV) {
        set_supply(VDD_LOW_MV); /* down after the frequency */
    }
    return KERR_OK;
}

struct cpuclock_port h3_cpuclock_port(void) {
    struct cpuclock_port p = {.set = cpux_set, .self = NULL};
    return p;
}

/* The fake: it changes nothing and counts, so the governor can be driven
 * where the hardware cannot be. */
static struct {
    uint32_t khz;
    uint32_t mv;
    uint32_t changes;
} fake;

static int fake_set(void *self, uint32_t khz, uint32_t mv) {
    (void)self;
    fake.khz = khz;
    fake.mv = mv;
    fake.changes++;
    return KERR_OK;
}

struct cpuclock_port h3_cpuclock_fake_port(void) {
    struct cpuclock_port p = {.set = fake_set, .self = NULL};
    return p;
}

void h3_cpuclock_fake_last(uint32_t *khz, uint32_t *mv, uint32_t *changes) {
    *khz = fake.khz;
    *mv = fake.mv;
    *changes = fake.changes;
}
