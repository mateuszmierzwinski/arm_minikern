#include "kernel/irq.h"

#include <stdbool.h>

#include "kernel/klock.h"

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

/* Waits for an interrupt, and gives up the kernel lock while waiting.
 *
 * It has to: the interrupt this is waiting for arrives through
 * trap_dispatch, which takes that lock on the same core, so holding it
 * across the wait is a deadlock against oneself (klock.h rule 5). Callers
 * all loop on a condition, so re-reading their state after the wait is
 * what they already do. */
void irq_wait(void) {
    bool held = klock_held_here();
    if (held) {
        klock_release();
    }
    __asm__ volatile("dsb\n\twfi\n\tcpsie i\n\tisb\n\tcpsid i" ::: "memory");
    if (held) {
        klock_acquire();
    }
}
