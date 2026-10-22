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

/* Raw sample to milli-degrees, as the H3's datasheet gives it:
 *
 *     T(°C) = (OFFSET_mC - raw * SLOPE_uC_per_lsb / 1000) / 1000
 *
 * The constants are the part that must be checked on the board (CLAUDE.md
 * §3). A sample of zero means the sensor has not produced one yet, which is
 * not 223 °C and is reported as a failure instead. */
#define THS_OFFSET_mC 223000
#define THS_SLOPE_uC  119100

static volatile uint32_t *ths(uint32_t off) {
    return (volatile uint32_t *)io_virt(H3_THS_BASE + off);
}

static int32_t raw_to_mC(uint32_t raw) {
    return THS_OFFSET_mC - (int32_t)((raw * THS_SLOPE_uC) / 1000u);
}

static int ths_read(void *self, int32_t *mC) {
    uint32_t raw = *ths(THS_DATA0) & THS_DATA_MASK;
    (void)self;
    if (raw == 0) {
        return KERR_AGAIN; /* no sample yet: not a temperature */
    }
    *mC = raw_to_mC(raw);
    return KERR_OK;
}

struct thermal_port h3_thermal_port(void) {
    struct thermal_port p = {.read_mC = ths_read, .self = NULL};
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
