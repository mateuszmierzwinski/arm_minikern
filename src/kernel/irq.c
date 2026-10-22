#include "kernel/irq.h"

#include "core/console.h"
#include "core/errno.h"

static struct irq_chip_port chip;
static struct irq_handler handlers[IRQ_MAX_LINES];

void irq_init(const struct irq_chip_port *c) {
    chip = *c;
}

int irq_register(uint32_t id, const struct irq_handler *h) {
    if (id >= IRQ_MAX_LINES || handlers[id].handle != NULL || h->handle == NULL) {
        return KERR_INVAL;
    }
    handlers[id] = *h;
    chip.enable(chip.self, id);
    return KERR_OK;
}

void irq_dispatch(void) {
    for (;;) {
        uint32_t id = chip.ack(chip.self);
        if (id >= IRQ_MAX_LINES) {
            return;
        }
        if (handlers[id].handle != NULL) {
            handlers[id].handle(handlers[id].self);
        } else {
            kprintf("irq: unexpected interrupt %u\n", id);
        }
        chip.eoi(chip.self, id);
    }
}

void irq_wait(void) {
    __asm__ volatile("dsb\n\twfi\n\tcpsie i\n\tisb\n\tcpsid i" ::: "memory");
}
