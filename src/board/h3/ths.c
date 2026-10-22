#include "board/h3/ths.h"

#include "arch/arm/layout.h"
#include "board/h3/h3.h"
#include "core/errno.h"

#include <stdbool.h>

/* The thermal sensor's block. Datasheet addresses, **never run on silicon**
 * (ths.h): QEMU does not model it, so a reading here has never been
 * compared with a real temperature. */
#define H3_THS_BASE   0x01C25000u
#define THS_CTRL0     0x0000u
#define THS_CTRL2     0x0040u
#define THS_INT_CTRL  0x0044u
#define THS_INT_STA   0x0048u
#define THS_FILTER    0x0070u
#define THS_DATA0     0x0080u

#define THS_ACQ0      0x0190u      /* acquisition time */
#define THS_SENSE_EN  (1u << 0)    /* enable sensor 0 */
#define THS_ACQ1_SHIFT 16u
#define THS_FILTER_EN (1u << 2)
#define THS_FILTER_2  0x1u         /* average two samples */
#define THS_DATA_MASK 0xfffu
#define THS_PERIOD    0x0100u

/* The sensor needs its bus clock ungated, its reset released and its module
 * clock running before any of its registers mean anything. **This was
 * missing**, and on a board it would have left the block unclocked and held
 * in reset: every write ignored, every read a stale or floating value, and
 * a governor running for ever on the worst-case assumption while looking
 * like it worked. QEMU models no sensor, so nothing here could have shown
 * it; the device tree of an implementation that runs on these boards did —
 * it declares two clocks and a reset for this node, and this driver touched
 * neither.
 *
 * Offsets corroborated against that implementation's clock table
 * (CLAUDE.md §3), which also agrees with this kernel's existing EMAC
 * numbers — the same table gives the EMAC reset as 0x2c0 bit 17 and the
 * internal PHY's as 0x2c8 bit 2, which is what emac.c already used. */
#define CCU_BUS_GATE2  0x0068u
#define CCU_BUS_RST3   0x02d0u
#define CCU_THS_CLK    0x0074u
#define CCU_THS_BIT    (1u << 8)   /* in both the gate and the reset register */
#define THS_CLK_ENABLE (1u << 31)
#define THS_CLK_DIV1   0u          /* 24 MHz: the rate the sensor's timing assumes */

/* Raw sample to milli-degrees, as the H3's datasheet gives it:
 *
 *     T(°C) = (OFFSET_mC - raw * SLOPE_uC_per_lsb / 1000) / 1000
 *
 * **The constants are disputed, and only a board can settle them.** There
 * are at least three conversions in circulation for this part:
 *
 *   these:            223000 - raw * 119100 / 1000
 *   hardware-tested:  217000 - raw * 1211 / 10   (and a second form of the
 *                     same thing, 217000 with a divisor of 8253, which works
 *                     out at 121.17 µC per LSB)
 *   a third, quoted from a datasheet, that its user had to correct by an
 *                     empirical 51 °C to get sensible readings at all.
 *
 * That third one is the useful signal: a datasheet-derived formula for this
 * sensor has been reported needing a 51 °C fudge, so "the datasheet says so"
 * is weak evidence here. **These constants' own provenance is a comment,
 * not a citation** — this file used to claim they were the datasheet's, and
 * that claim cannot be supported from anything available, so it has been
 * withdrawn rather than repeated.
 *
 * They are kept in force anyway, for one reason that does not depend on
 * which is right: they read *hotter* than the hardware-tested pair, by about
 * 9 °C at a room-temperature sample and by a similar margin at the trip
 * points, so an error here throttles early rather than late.
 * `minikern.hwreport=1` prints the raw sample **and both conversions**, so
 * one reading from a board in a room of a known temperature decides it —
 * and the raw value means a third formula can be applied later without
 * another trip to the hardware.
 *
 * A sample of zero means the sensor has not produced one yet, which is not
 * 223 °C and is reported as a failure instead — the same choice that
 * implementation makes. */
#define THS_OFFSET_mC 223000
#define THS_SLOPE_uC  119100

static volatile uint32_t *ths(uint32_t off) {
    return (volatile uint32_t *)io_virt(H3_THS_BASE + off);
}

static volatile uint32_t *ccu(uint32_t off) {
    return (volatile uint32_t *)io_virt(H3_CCU_BASE + off);
}

/* Ungates the sensor, releases its reset and starts its module clock.
 *
 * Only ever **sets** bits, never clears them: these offsets are hardware
 * facts this kernel has not yet checked on silicon, and setting a gate or
 * releasing a reset for the wrong block costs nothing, where clearing one
 * could stop a peripheral something else depends on. */
static void ths_clock_on(void) {
    *ccu(CCU_BUS_GATE2) |= CCU_THS_BIT;
    *ccu(CCU_BUS_RST3) |= CCU_THS_BIT;
    *ccu(CCU_THS_CLK) |= THS_CLK_ENABLE | THS_CLK_DIV1;
}

static int32_t raw_to_mC(uint32_t raw) {
    return THS_OFFSET_mC - (int32_t)((raw * THS_SLOPE_uC) / 1000u);
}

static int ths_read(void *self, int32_t *mC) {
    uint32_t raw = *ths(THS_DATA0) & THS_DATA_MASK;
    (void)self;
    /* Both ends of the field mean "no sensor", not a temperature. Zero is a
     * block that has not sampled yet; all ones is one that is **not there**
     * — unclocked, held in reset, or absent from the model you are running
     * on — and through this chip's formula that reads as -264 °C, which the
     * governor would act on by boosting to the top point with no thermal
     * protection at all. A sensor whose reading cannot be believed must
     * report a failure, which the governor already treats as the worst
     * case (kernel/thermal.c). */
    if (raw == 0 || raw == THS_DATA_MASK) {
        return KERR_AGAIN;
    }
    *mC = raw_to_mC(raw);
    return KERR_OK;
}

struct thermal_port h3_thermal_port(void) {
    struct thermal_port p = {.read_mC = ths_read, .self = NULL};
    ths_clock_on();
    *ths(THS_CTRL0) = THS_ACQ0;
    *ths(THS_CTRL2) = THS_SENSE_EN | (THS_ACQ0 << THS_ACQ1_SHIFT);
    *ths(THS_FILTER) = THS_FILTER_EN | THS_FILTER_2;
    *ths(THS_INT_CTRL) = THS_PERIOD;
    *ths(THS_INT_STA) = 0xffffffffu; /* clear whatever the boot loader left */
    return p;
}

/* The fake sensor: a ramp from cold to past critical and back, one step per
 * reading, so a single boot walks the policy through every decision it can
 * make. The step is coarse on purpose — a slow ramp would need a longer
 * test than a decision interval. */
#define FAKE_FLOOR_mC   30000
#define FAKE_CEILING_mC 100000
#define FAKE_STEP_mC    6000

static struct {
    int32_t mC;
    bool falling;
} ramp = {.mC = FAKE_FLOOR_mC};

/* A fake that answers with a reading no silicon could produce, which is what
 * a sensor block that is absent or unclocked actually gives: this chip's
 * formula turns an all-ones sample into -264 °C. It exists so that a test
 * can prove the governor refuses to act on it rather than boosting to the
 * top of the table with nothing watching the temperature. */
static int32_t nonsense_mC = 0;

static int nonsense_read(void *self, int32_t *mC) {
    (void)self;
    *mC = nonsense_mC;
    return KERR_OK;
}

struct thermal_port h3_thermal_nonsense_port(int32_t mC) {
    struct thermal_port p = {.read_mC = nonsense_read, .self = NULL};
    nonsense_mC = mC;
    return p;
}

static int fake_read(void *self, int32_t *mC) {
    (void)self;
    *mC = ramp.mC;
    ramp.falling = ramp.mC >= FAKE_CEILING_mC ? true : (ramp.mC <= FAKE_FLOOR_mC ? false : ramp.falling);
    ramp.mC += ramp.falling ? -FAKE_STEP_mC : FAKE_STEP_mC;
    return KERR_OK;
}

struct thermal_port h3_thermal_fake_port(void) {
    struct thermal_port p = {.read_mC = fake_read, .self = NULL};
    return p;
}
