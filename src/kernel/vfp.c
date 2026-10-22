#include "kernel/vfp.h"

#include "arch/arm/cpu.h"
#include "arch/arm/smp.h"
#include "kernel/sched.h"

#include <stddef.h>

/* Which thread's registers are in the unit, per core. */
static struct task *owner[SMP_MAX_CPUS];

/* How many times the state has actually been moved, against how many
 * switches could have moved it. The point of doing this lazily is that the
 * first number stays well below the second, and a kernel that reports them
 * can be checked rather than believed. */
static uint64_t moves;

static struct task **owner_slot(void) {
    uint32_t cpu = smp_cpu_id();
    return &owner[cpu < SMP_MAX_CPUS ? cpu : 0];
}

void vfp_switch_away(void) {
    arm_vfp_off();
}

bool vfp_claim(struct task *t) {
    struct task **have = owner_slot();
    if (arm_vfp_is_on()) {
        return false; /* the unit was available: a real undefined instruction */
    }
    /* The unit goes on **first**. Saving and restoring are themselves
     * floating-point instructions, so doing either with the unit off traps
     * again — in kernel mode, where an undefined instruction is a panic
     * rather than a signal. */
    arm_vfp_enable();
    if (*have != t) {
        if (*have != NULL) {
            arm_vfp_save(&(*have)->vfp);
        }
        arm_vfp_restore(&t->vfp);
        *have = t;
        moves++;
    }
    return true;
}

void vfp_capture(struct task *t) {
    if (*owner_slot() == t && arm_vfp_is_on()) {
        arm_vfp_save(&t->vfp);
    }
}

void vfp_read(struct task *t, struct vfp_state *out) {
    vfp_capture(t);
    *out = t->vfp;
}

void vfp_write(struct task *t, const struct vfp_state *in) {
    struct task **have = owner_slot();
    t->vfp = *in;
    if (*have == t) {
        /* Let go of the unit rather than reloading it here: the thread's
         * next floating-point instruction will take it back and pick the
         * new state up, and that path is already written. */
        *have = NULL;
        arm_vfp_off();
    }
}

void vfp_forget(const struct task *t) {
    for (uint32_t cpu = 0; cpu < SMP_MAX_CPUS; cpu++) {
        owner[cpu] = owner[cpu] == t ? NULL : owner[cpu];
    }
}

uint64_t vfp_moves(void) {
    return moves;
}
