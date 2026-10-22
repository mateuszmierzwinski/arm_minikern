#include "arch/arm/gic.h"

#define GICD_CTLR        0x000u
#define GICD_TYPER       0x004u
#define GICD_ISENABLER   0x100u
#define GICD_ICENABLER   0x180u
#define GICD_IPRIORITYR  0x400u
#define GICD_ITARGETSR   0x800u
#define GICC_CTLR        0x00u
#define GICC_PMR         0x04u
#define GICC_IAR         0x0cu
#define GICC_EOIR        0x10u
#define GIC_ENABLE_BOTH  3u      /* both groups (secure view); group 1 (non-secure) */
#define GIC_PRIO_DEFAULT 0xa0a0a0a0u
#define GIC_PMR_ALL      0xf0u   /* lets through every priority we assign */
#define GIC_TARGET_CPU0  0x01010101u
#define GIC_IAR_ID_MASK  0x3ffu

struct gic {
    uintptr_t dist;
    uintptr_t cpu;
};

static struct gic the_gic;

static uint32_t rd(uintptr_t a) {
    return *(volatile uint32_t *)a;
}

static void wr(uintptr_t a, uint32_t v) {
    *(volatile uint32_t *)a = v;
}

static uint32_t gic_ack(void *self) {
    struct gic *g = self;
    return rd(g->cpu + GICC_IAR) & GIC_IAR_ID_MASK;
}

static void gic_eoi(void *self, uint32_t id) {
    struct gic *g = self;
    wr(g->cpu + GICC_EOIR, id);
}

static void gic_enable(void *self, uint32_t id) {
    struct gic *g = self;
    wr(g->dist + GICD_ISENABLER + (id / 32u) * 4u, 1u << (id % 32u));
}

/* Everything disabled, mid priority, SPIs routed to CPU 0, then on. */
static void gic_reset_distributor(const struct gic *g) {
    uint32_t lines = ((rd(g->dist + GICD_TYPER) & 0x1fu) + 1u) * 32u;
    wr(g->dist + GICD_CTLR, 0);
    for (uint32_t i = 0; i < lines; i += 32u) {
        wr(g->dist + GICD_ICENABLER + i / 8u, ~0u);
    }
    for (uint32_t i = 0; i < lines; i += 4u) {
        wr(g->dist + GICD_IPRIORITYR + i, GIC_PRIO_DEFAULT);
        if (i >= 32u) {
            wr(g->dist + GICD_ITARGETSR + i, GIC_TARGET_CPU0);
        }
    }
    wr(g->dist + GICD_CTLR, GIC_ENABLE_BOTH);
}

struct irq_chip_port gic_init(uintptr_t dist_base, uintptr_t cpu_base) {
    the_gic = (struct gic){.dist = dist_base, .cpu = cpu_base};
    gic_reset_distributor(&the_gic);
    wr(cpu_base + GICC_PMR, GIC_PMR_ALL);
    wr(cpu_base + GICC_CTLR, GIC_ENABLE_BOTH);
    return (struct irq_chip_port){.ack = gic_ack, .eoi = gic_eoi, .enable = gic_enable, .self = &the_gic};
}
