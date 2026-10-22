#include "board/h3/hwreport.h"

#include "arch/arm/cpu.h"
#include "arch/arm/layout.h"
#include "board/h3/h3.h"
#include "board/h3/sid.h"
#include "core/console.h"

/* The blocks this report reads, by the offsets the rest of the kernel uses.
 * Nothing here writes: a report that changes the machine it is describing
 * is not a report. */
#define SYSCON_EPHY_CLK 0x0030u /* system-control + 0x30: internal PHY select */
#define CCU_PLL_CPUX    0x0000u
#define CCU_CPU_AXI_CFG 0x0050u
#define CCU_BUS_GATE0   0x0060u
#define CCU_BUS_GATE4   0x0070u
#define CCU_BUS_RST0    0x02c0u
#define CCU_BUS_RST2    0x02c8u
#define CCU_BUS_GATE2   0x0068u
#define CCU_BUS_RST3    0x02d0u
#define CCU_THS_CLK     0x0074u
#define THS_CTRL2       0x0040u
#define THS_FILTER      0x0070u
#define THS_DATA0       0x0080u
#define H3_THS_BASE     0x01C25000u
#define H3_SYSCON_BASE  0x01C00000u

static uint32_t mmio(uint32_t base, uint32_t off) {
    return *(volatile uint32_t *)io_virt(base + off);
}

/* The timer, which is the one fact that differs between the two boot paths
 * and so the one most likely to be wrong in a way nobody notices. */
static void report_cpu(void) {
    kprintf("hw: midr %08x (datasheet: 410fc075, Cortex-A7 r0p5)\n", arm_midr());
    kprintf("hw: cntfrq %u Hz (datasheet: 24000000 on silicon; QEMU's -kernel path says 62500000)\n",
            arm_cntfrq());
}

static void report_sid(void) {
    uint32_t id[4] = {0, 0, 0, 0};
    int rc = h3_sid_read(id);
    kprintf("hw: sid %d: %08x %08x %08x %08x (QEMU's is the constant 02c00081 11112222 "
            "33330000 44556677; a board's is unique)\n",
            rc, id[0], id[1], id[2], id[3]);
}

/* The EMAC's three gates. These are the bits the driver sets, and the ones
 * that decide between "link up" and nothing at all on real hardware. */
static void report_emac_clocks(void) {
    kprintf("hw: ccu bus_gate0 %08x (EMAC gate is bit 17, expected set)\n", mmio(H3_CCU_BASE, CCU_BUS_GATE0));
    kprintf("hw: ccu bus_rst0  %08x (EMAC reset is bit 17, expected set = released)\n",
            mmio(H3_CCU_BASE, CCU_BUS_RST0));
    kprintf("hw: ccu bus_gate4 %08x (internal PHY gate is bit 0)\n", mmio(H3_CCU_BASE, CCU_BUS_GATE4));
    kprintf("hw: ccu bus_rst2  %08x (internal PHY reset is bit 2)\n", mmio(H3_CCU_BASE, CCU_BUS_RST2));
    kprintf("hw: syscon ephy_clk %08x at 01c00030 — **the register QEMU does not model**: "
            "on silicon it selects the internal PHY, powers it and sets its MDIO address\n",
            mmio(H3_SYSCON_BASE, SYSCON_EPHY_CLK));
}

static void report_clocks(void) {
    kprintf("hw: ccu pll_cpux %08x, cpu_axi_cfg %08x (source field bits 16-17: 1 = 24 MHz crystal, 2 = PLL)\n",
            mmio(H3_CCU_BASE, CCU_PLL_CPUX), mmio(H3_CCU_BASE, CCU_CPU_AXI_CFG));
}

/* Eight samples, because the first one after enabling the sensor is not
 * trustworthy and a single reading cannot show whether the block is
 * producing anything at all. */
/* The sensor's own clocking, which has to be right before a sample means
 * anything, and which this kernel did not touch until the device tree of an
 * implementation that runs on these boards said it must. */
static void report_thermal_clocks(void) {
    kprintf("hw: ccu bus_gate2 %08x (THS gate is bit 8, expected set)\n", mmio(H3_CCU_BASE, CCU_BUS_GATE2));
    kprintf("hw: ccu bus_rst3  %08x (THS reset is bit 8, expected set = released)\n",
            mmio(H3_CCU_BASE, CCU_BUS_RST3));
    kprintf("hw: ccu ths_clk   %08x (enable is bit 31, divider bits 1-0 = 0 for 24 MHz)\n",
            mmio(H3_CCU_BASE, CCU_THS_CLK));
}

static void report_thermal(void) {
    report_thermal_clocks();
    kprintf("hw: ths ctrl2 %08x filter %08x — **QEMU models no thermal sensor**; "
            "a stub reads 0, an unclocked block reads 0xfff, and both are refused\n",
            mmio(H3_THS_BASE, THS_CTRL2), mmio(H3_THS_BASE, THS_FILTER));
    for (uint32_t i = 0; i < 8u; i++) {
        uint32_t raw = mmio(H3_THS_BASE, THS_DATA0) & 0xfffu;
        /* Both candidate conversions, because they disagree by about 9 °C at
         * room temperature and one reading from a room of a known
         * temperature settles which is right (board/h3/ths.c). */
        int32_t mC = 223000 - (int32_t)((raw * 119100u) / 1000u);
        int32_t alt = 217000 - (int32_t)((raw * 1211u) / 10u);
        kprintf("hw: ths sample %u: raw %03x -> %d mC (datasheet) / %d mC (the other formula)%s\n",
                i, raw, mC, alt,
                raw == 0 ? " — raw 0 means no sample, refused by the driver"
                : raw == 0xfffu ? " — raw 0xfff means no sensor, refused by the driver"
                                : " — plausible in a room: 20000-45000");
        for (volatile uint32_t spin = 0; spin < 200000u; spin++) {
        }
    }
}

void h3_hwreport(void) {
    kprintf("hw: ---- hardware report: every value this kernel took from the datasheet ----\n");
    report_cpu();
    report_sid();
    report_clocks();
    report_emac_clocks();
    report_thermal();
    kprintf("hw: ---- end of report ----\n");
    kprintf("hw: the PHY's own answer is on the `net: eth0` line above: its ID, its MDIO address, "
            "and whether the link came up\n");
    kprintf("hw: what only a board can settle: the ephy_clk register above; which of the two "
            "ths conversions is right; and the VDD_CPUX pin (minikern.vddcpux=, QEMU models no PIO)\n");
}
