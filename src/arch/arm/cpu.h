/* cpu.h — ARMv7-A system-register reads used during bring-up. */
#ifndef MINIKERN_ARCH_ARM_CPU_H
#define MINIKERN_ARCH_ARM_CPU_H

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

void arm_vfp_enable(void);
void arm_vfp_save(struct vfp_state *s);
void arm_vfp_restore(const struct vfp_state *s);

/* Names a CPSR mode field for the boot log. */
const char *arm_mode_name(uint32_t cpsr);

#endif
