#include "kernel/klock.h"

#include "arch/arm/smp.h"
#include "arch/arm/spinlock.h"

static struct spinlock lock = SPINLOCK_INIT;
static volatile uint64_t acquires;
static volatile uint64_t contended;

/* Which core holds it, as its number plus one so that zero means free.
 * Written only by the holder, so it needs no atomic of its own. */
static volatile uint32_t owner;

void klock_acquire(void) {
    if (spin_is_locked(&lock)) {
        contended++;  /* racy by nature: it is a counter, not a decision */
    }
    spin_lock(&lock);
    owner = smp_cpu_id() + 1u;
    acquires++;
}

void klock_release(void) {
    owner = 0;
    spin_unlock(&lock);
}

bool klock_held(void) {
    return spin_is_locked(&lock);
}

bool klock_held_here(void) {
    return owner == smp_cpu_id() + 1u;
}

void klock_stats(uint64_t *a, uint64_t *c) {
    *a = acquires;
    *c = contended;
}
