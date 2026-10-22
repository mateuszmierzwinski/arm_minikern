/* spinlock — the one synchronisation primitive, for ARMv7.
 *
 * A ticket lock, so a core that has been waiting goes next: a plain
 * test-and-set lets one core keep winning, and on four cores that is how a
 * thread starves. Waiters sleep in `wfe` and are woken by the `sev` the
 * unlock sends, which costs nothing while the lock is held.
 *
 * The barriers matter as much as the atomics: `dmb` after taking the lock
 * so nothing inside the critical section is read before it is held, and
 * `dmb` before releasing so everything written inside is visible to the
 * core that takes it next.
 */
#ifndef MINIKERN_ARCH_ARM_SPINLOCK_H
#define MINIKERN_ARCH_ARM_SPINLOCK_H

#include <stdbool.h>
#include <stdint.h>

/* next is the ticket to hand out, serving the one being served. */
struct spinlock {
    volatile uint32_t next;
    volatile uint32_t serving;
};

#define SPINLOCK_INIT {0, 0}

static inline void spin_lock(struct spinlock *l) {
    uint32_t ticket, tmp;
    __asm__ volatile(
        "1: ldrex %0, [%2]\n"
        "   add   %1, %0, #1\n"
        "   strex %3, %1, [%2]\n"
        "   teq   %3, #0\n"
        "   bne   1b\n"
        : "=&r"(ticket), "=&r"(tmp), "+r"(l), "=&r"(tmp)
        :
        : "cc", "memory");
    while (l->serving != ticket) {
        __asm__ volatile("wfe" ::: "memory");
    }
    __asm__ volatile("dmb" ::: "memory");
}

static inline void spin_unlock(struct spinlock *l) {
    __asm__ volatile("dmb" ::: "memory");
    l->serving++;
    __asm__ volatile("dsb\n\tsev" ::: "memory");
}

/* Takes the lock only if it is free. For an assertion, not for waiting. */
static inline bool spin_is_locked(const struct spinlock *l) {
    return l->next != l->serving;
}

#endif
