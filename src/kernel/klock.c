#include "kernel/klock.h"

#include "core/console.h"
#include "kernel/sched.h"
#include "kernel/vfp.h"

#include "arch/arm/smp.h"
#include "arch/arm/spinlock.h"

static struct spinlock lock = SPINLOCK_INIT;
static volatile uint64_t acquires;
static volatile uint64_t per_cpu[SMP_MAX_CPUS];  /* entries by core: proof it is used */
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
    per_cpu[smp_cpu_id() % SMP_MAX_CPUS]++;
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

void klock_report(void) {
    kprintf("smp: %u cores scheduling, %llu kernel entries (%llu contended), per core",
            smp_scheduling(), (unsigned long long)acquires, (unsigned long long)contended);
    for (uint32_t cpu = 0; cpu < SMP_MAX_CPUS; cpu++) {
        kprintf(" %llu", (unsigned long long)per_cpu[cpu]);
    }
    kprintf("\n");
    kprintf("vfp: state moved %llu times in %llu switches\n", (unsigned long long)vfp_moves(),
            (unsigned long long)sched_switches());
    kprintf("cpuoff: %u cores powered off while idle, %u still off\n", smp_parks(),
            smp_any_parked() ? 1u : 0u);
}
