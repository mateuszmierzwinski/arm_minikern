/* cpu.h — ARMv7-A system-register reads used during bring-up. */
#ifndef MINIKERN_ARCH_ARM_CPU_H
#define MINIKERN_ARCH_ARM_CPU_H

#include <stdbool.h>
#include <stdint.h>

#define ARM_MODE_MASK 0x1fu

/* Main ID register: implementer, variant, part, revision. */
static inline uint32_t arm_midr(void) {
    uint32_t v;
    __asm__ volatile("mrc p15, 0, %0, c0, c0, 0" : "=r"(v));
    return v;
}

/* Generic-timer frequency as firmware programmed it. Never assume it:
 * 24 MHz on the H3, 62.5 MHz under QEMU (measured). */
/* MPIDR: which core this is. On the H3 the core number is affinity 0. */
static inline uint32_t arm_mpidr(void) {
    uint32_t v;
    __asm__ volatile("mrc p15, 0, %0, c0, c0, 5" : "=r"(v));
    return v;
}

static inline uint32_t arm_cntfrq(void) {
    uint32_t v;
    __asm__ volatile("mrc p15, 0, %0, c14, c0, 0" : "=r"(v));
    return v;
}

static inline void arm_wfi(void) {
    __asm__ volatile("wfi" ::: "memory");
}

/* VFP/NEON state of one thread (vectors.S): d0-d31, then FPSCR. */
struct vfp_state {
    uint64_t d[32];
    uint32_t fpscr;
};

/* The cycle counter the virtual timer counts on, read as 32 bits: its low
 * bits are what a timing sample is made of (kernel/entropy.h). */
static inline uint32_t arm_cycle_counter(void) {
    uint32_t lo, hi;
    __asm__ volatile("mrrc p15, 1, %0, %1, c14" : "=r"(lo), "=r"(hi));
    (void)hi;
    return lo;
}

void arm_vfp_enable(void);

/* Switches the unit off for the running thread, so that its next
 * floating-point instruction traps; and asks whether it is on. Together
 * they are how the VFP state is moved only when it is used. */
void arm_vfp_off(void);
bool arm_vfp_is_on(void);
void arm_vfp_save(struct vfp_state *s);
void arm_vfp_restore(const struct vfp_state *s);

/* Names a CPSR mode field for the boot log. */
const char *arm_mode_name(uint32_t cpsr);

#endif
