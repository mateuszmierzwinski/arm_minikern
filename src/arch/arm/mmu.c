#include "arch/arm/mmu.h"

#include "arch/arm/layout.h"

#define TTBCR_N_2GIB   1u
#define CACHE_LINE     64u   /* Cortex-A7 L1 line size */
#define USER_L1_BYTES  8192u

extern uint32_t boot_l1[4096];

/* An all-faults user table for when no process is current. */
static uint32_t empty_user_l1[USER_L1_BYTES / 4] __attribute__((aligned(USER_L1_BYTES)));

void mmu_flush_tlb(void) {
    __asm__ volatile("dsb\n\tmcr p15, 0, %0, c8, c7, 0\n\tdsb\n\tisb" : : "r"(0) : "memory");
}

void mmu_switch_user(uint32_t l1_phys) {
    __asm__ volatile("mcr p15, 0, %0, c2, c0, 0\n\tisb" : : "r"(l1_phys | TTBR_WALK_ATTRS) : "memory");
    mmu_flush_tlb();
}

void mmu_map_kernel_section(uint32_t virt, uint32_t phys, uint32_t attrs) {
    boot_l1[virt >> SECTION_SHIFT] = (phys & 0xfff00000u) | attrs;
    mmu_flush_tlb();
}

void mmu_clear_user(void) {
    mmu_switch_user(virt_to_phys(empty_user_l1));
}

void mmu_init(void) {
    __asm__ volatile("mcr p15, 0, %0, c2, c0, 2\n\tisb" : : "r"(TTBCR_N_2GIB) : "memory");
    mmu_switch_user(virt_to_phys(empty_user_l1));
    for (uint32_t i = 0; i < (uint32_t)USER_TOP >> SECTION_SHIFT; i++) {
        boot_l1[i] = 0;
    }
    mmu_flush_tlb();
}

void mmu_sync_icache(const void *virt, uint32_t len) {
    uintptr_t at = (uintptr_t)virt & ~(uintptr_t)(CACHE_LINE - 1u);
    uintptr_t end = (uintptr_t)virt + len;
    for (; at < end; at += CACHE_LINE) {
        __asm__ volatile("mcr p15, 0, %0, c7, c11, 1" : : "r"(at) : "memory");  /* DCCMVAU */
    }
    __asm__ volatile("dsb\n\tmcr p15, 0, %0, c7, c5, 0\n\tmcr p15, 0, %0, c7, c5, 6\n\tdsb\n\tisb"
                     : : "r"(0) : "memory");  /* ICIALLU, BPIALL */
}
