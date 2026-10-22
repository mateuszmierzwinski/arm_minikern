#include "arch/arm/mmu.h"

#include "arch/arm/layout.h"

#define TTBCR_N_2GIB   1u
#define CACHE_LINE     64u   /* Cortex-A7 L1 line size */
#define USER_L1_BYTES  8192u

extern uint32_t boot_l1[4096];

/* An all-faults user table for when no process is current. */
static uint32_t empty_user_l1[USER_L1_BYTES / 4] __attribute__((aligned(USER_L1_BYTES)));

/* Flushes every core's TLB, not just this one's: TLBIALLIS is the
 * inner-shareable broadcast form. A local flush would leave another core
 * translating through an entry this one has just changed — and since every
 * table change happens under the kernel lock, a broadcast here is the whole
 * of what SMP needs from TLB maintenance. */
void mmu_flush_tlb(void) {
    __asm__ volatile("dsb ish\n\tmcr p15, 0, %0, c8, c3, 0\n\tdsb ish\n\tisb"
                     : : "r"(0) : "memory");
}

void mmu_switch_user(uint32_t l1_phys) {
    __asm__ volatile("mcr p15, 0, %0, c2, c0, 0\n\tisb" : : "r"(l1_phys | TTBR_WALK_ATTRS) : "memory");
    mmu_flush_tlb();
}

/* Writes one section entry of the kernel's table and flushes the TLB. */
static void set_kernel_section(uint32_t virt, uint32_t entry) {
    boot_l1[virt >> SECTION_SHIFT] = entry;
    mmu_flush_tlb();
}

void mmu_map_kernel_section(uint32_t virt, uint32_t phys, uint32_t attrs) {
    set_kernel_section(virt, (phys & 0xfff00000u) | attrs);
}

void mmu_unmap_kernel_section(uint32_t virt) {
    set_kernel_section(virt, 0);
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

void mmu_flush_dcache_range(const void *virt, uint32_t len) {
    uintptr_t at = (uintptr_t)virt & ~(uintptr_t)(CACHE_LINE - 1u);
    uintptr_t end = (uintptr_t)virt + len;
    for (; at < end; at += CACHE_LINE) {
        __asm__ volatile("mcr p15, 0, %0, c7, c14, 1" : : "r"(at) : "memory");  /* DCCIMVAC */
    }
    __asm__ volatile("dsb" : : : "memory");
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
