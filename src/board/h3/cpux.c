#include "board/h3/cpux.h"

#include "arch/arm/layout.h"
#include "board/h3/h3.h"
#include "board/h3/pio.h"
#include "core/console.h"
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

/* N and K for a frequency, as PLL_CPUX = 24 MHz * N * K (M and P left at 1).
 *
 * **N alone is not enough**, which is what `minikern.cpufreq=probe` was
 * written to find out and found immediately: N tops out at 32, so 24 MHz * N
 * reaches 768 MHz, and three of this chip's five published points —  816,
 * 912 and 1008 MHz — are above that. They had all been failing with EINVAL,
 * invisibly, because the governor's tests drive a fake adapter and nothing
 * had ever run this arithmetic against the register it writes.
 *
 * K is tried in ascending order, so the smallest multiplier that brings N
 * into range is the one used and the common points keep the factors they
 * had. M and P stay at 1: a divider that is never anything but 1 cannot
 * produce an out-of-range intermediate frequency while the PLL settles. */
static int pll_factors(uint32_t khz, uint32_t *n, uint32_t *k) {
    uint32_t product;
    if (khz < OSC_KHZ || khz % OSC_KHZ != 0) {
        return KERR_INVAL;
    }
    product = khz / OSC_KHZ; /* N * K */
    for (uint32_t try_k = 1; try_k <= 4u; try_k++) {
        if (product % try_k == 0 && product / try_k >= 1u && product / try_k <= 32u) {
            *n = product / try_k;
            *k = try_k;
            return KERR_OK;
        }
    }
    return KERR_INVAL;
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
    uint32_t n = 0, k = 1, ctrl;
    int rc = pll_factors(khz, &n, &k);
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
    ctrl |= ((n - 1u) << PLL_N_SHIFT) | ((k - 1u) << PLL_K_SHIFT) | PLL_ENABLE;
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

/* What the PLL register says it is set to, decoded back into kHz.
 *
 * PLL_CPUX = 24 MHz * N * K / (M * P), with every field stored one less
 * than its value except P, which is a shift. Reading it back is the only
 * way to check that a write landed where it was aimed. */
static uint32_t pll_khz_from(uint32_t ctrl) {
    uint32_t n = ((ctrl >> PLL_N_SHIFT) & 31u) + 1u;
    uint32_t k = ((ctrl >> PLL_K_SHIFT) & 3u) + 1u;
    uint32_t m = ((ctrl >> PLL_M_SHIFT) & 3u) + 1u;
    uint32_t pdiv = 1u << ((ctrl >> PLL_P_SHIFT) & 3u);
    return OSC_KHZ * n * k / (m * pdiv);
}

void h3_cpuclock_probe(const uint32_t *khz, const uint32_t *mv, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        uint32_t ctrl;
        int rc = cpux_set(NULL, khz[i], mv[i]);
        ctrl = *ccu(PLL_CPUX_CTRL);
        kprintf("cpuclock: asked %u kHz at %u mV -> %d, pll ctrl %08x = %u kHz, src %u, enabled %u, locked %u\n",
                khz[i], mv[i], rc, ctrl, pll_khz_from(ctrl),
                (*ccu(CPU_AXI_CFG) >> CPU_SRC_SHIFT) & 3u, (ctrl & PLL_ENABLE) != 0 ? 1u : 0u,
                (ctrl & PLL_LOCK) != 0 ? 1u : 0u);
    }
    /* The refusals, which are as much a part of the adapter as the writes:
     * below the crystal, not a multiple of it, and past what N and K
     * together can reach (32 * 4 * 24 MHz = 3072 MHz). */
    kprintf("cpuclock: 12000 kHz (below the crystal) -> %d\n", cpux_set(NULL, 12000, 1040));
    kprintf("cpuclock: 500000 kHz (not a multiple of 24 MHz) -> %d\n", cpux_set(NULL, 500000, 1040));
    kprintf("cpuclock: 3096000 kHz (no N and K reach it) -> %d\n", cpux_set(NULL, 3096000, 1300));
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
