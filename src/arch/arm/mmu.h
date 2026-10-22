/* mmu — translation control for the running CPU: the split between the
 * kernel's table (TTBR1, built by start.S) and the current process's
 * (TTBR0), TLB and cache maintenance. Table *contents* are pgtable.h's. */
#ifndef MINIKERN_ARCH_ARM_MMU_H
#define MINIKERN_ARCH_ARM_MMU_H

#include <stdint.h>

/* Hands the low 2 GiB to TTBR0 (TTBCR.N = 1) with an empty user table and
 * drops the boot identity map from the kernel table. */
void mmu_init(void);

/* Makes the table at physical address l1_phys (8 KiB, 8 KiB aligned) the
 * current user address space and flushes the TLB. */
void mmu_switch_user(uint32_t l1_phys);

/* Leaves no user address space current (before one is destroyed). */
void mmu_clear_user(void);

/* Maps one 1 MiB section of the kernel half (virt, phys both 1 MiB
 * aligned) with the given section attributes. */
void mmu_map_kernel_section(uint32_t virt, uint32_t phys, uint32_t attrs);

/* Flushes every TLB entry; called after any change to a live table. */
void mmu_flush_tlb(void);

/* Makes freshly written instructions at [virt, virt+len) visible to
 * instruction fetch: clean the D-cache to the point of unification, then
 * invalidate the I-cache. */
void mmu_sync_icache(const void *virt, uint32_t len);

#endif
