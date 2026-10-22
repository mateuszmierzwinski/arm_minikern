#include "board/h3/wdt.h"

#include "arch/arm/layout.h"
#include "board/h3/h3.h"

/* The sun6i watchdog, in the timer block. */
#define H3_WDT_BASE 0x01C20CA0u
#define WDT_IRQ_EN  0x00u
#define WDT_IRQ_STA 0x04u
#define WDT_CTRL    0x10u
#define WDT_CFG     0x14u
#define WDT_MODE    0x18u

#define WDT_CTRL_RESTART (1u << 0)
#define WDT_CTRL_KEY     (0x0a57u << 1) /* the restart bit is ignored without it */
#define WDT_CFG_SYSTEM   1u             /* reset the whole system, not just the CPU */
#define WDT_MODE_EN      (1u << 0)
#define WDT_MODE_SHIFT   4u

/* What the hardware offers, in half-seconds, indexed by the interval code.
 * There is no arithmetic here: the set is a table in the datasheet, and
 * computing it would be inventing a register layout. */
static const uint16_t INTERVALS_HALF_S[] = {1, 2, 4, 6, 8, 10, 12, 16, 20, 24, 28, 32};

static volatile uint32_t *wdt(uint32_t off) {
    return (volatile uint32_t *)io_virt(H3_WDT_BASE + off);
}

/* The shortest code whose interval covers seconds, or -1. */
static int code_for(uint32_t seconds) {
    uint32_t want = seconds * 2u;
    for (uint32_t i = 0; i < sizeof INTERVALS_HALF_S / sizeof INTERVALS_HALF_S[0]; i++) {
        if (INTERVALS_HALF_S[i] >= want) {
            return (int)i;
        }
    }
    return -1;
}

void h3_wdt_pet(void) {
    *wdt(WDT_CTRL) = WDT_CTRL_KEY | WDT_CTRL_RESTART;
}

uint32_t h3_wdt_start(uint32_t seconds) {
    int code = seconds == 0 ? -1 : code_for(seconds);
    if (code < 0) {
        return 0;
    }
    *wdt(WDT_IRQ_EN) = 0;                /* reboot, do not interrupt */
    *wdt(WDT_CFG) = WDT_CFG_SYSTEM;
    *wdt(WDT_MODE) = ((uint32_t)code << WDT_MODE_SHIFT) | WDT_MODE_EN;
    h3_wdt_pet();
    return INTERVALS_HALF_S[code] / 2u;
}

void h3_wdt_stop(void) {
    *wdt(WDT_MODE) = 0;
}
