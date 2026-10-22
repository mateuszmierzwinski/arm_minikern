/* gic — an ARM GIC-400 (GICv2) distributor and CPU interface: the H3's
 * interrupt controller. Works whether the kernel runs secure (QEMU's ELF
 * boot) or non-secure (U-Boot and QEMU's uImage boot): group configuration
 * is firmware's in the latter case, and every interrupt arrives as an IRQ. */
#ifndef MINIKERN_ARCH_ARM_GIC_H
#define MINIKERN_ARCH_ARM_GIC_H

#include <stdint.h>

#include "core/ports.h"

#define GIC_SPURIOUS 1023u

/* An irq_chip_port over the GIC at the two (kernel virtual) bases. */
struct irq_chip_port gic_init(uintptr_t dist_base, uintptr_t cpu_base);

/* The per-core half: this core's CPU interface and its banked private
 * interrupts. Every core calls it once, the primary through gic_init. */
void gic_init_cpu(void);

#endif
