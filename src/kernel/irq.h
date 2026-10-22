/* irq — interrupt dispatch: one handler per interrupt ID, run with IRQs
 * masked, on whatever kernel stack the interrupt landed. */
#ifndef MINIKERN_KERNEL_IRQ_H
#define MINIKERN_KERNEL_IRQ_H

#include <stdint.h>

#include "core/ports.h"

#define IRQ_MAX_LINES 160u  /* the H3's GIC: 16 SGI + 16 PPI + 128 SPI */

/* What runs for one interrupt ID. */
struct irq_handler {
    void (*handle)(void *self);
    void *self;
};

void irq_init(const struct irq_chip_port *chip);

/* Installs h for id and unmasks id at the controller. KERR_INVAL if id is
 * out of range or already claimed. */
int irq_register(uint32_t id, const struct irq_handler *h);

/* Claims and runs every pending interrupt (the trap layer's irq hook). */
void irq_dispatch(void);

/* Enables IRQs, waits for one, masks them again: the idle step. */
void irq_wait(void);

#endif
