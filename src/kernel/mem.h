/* mem — the kernel's memory, wired up: the one physical page allocator,
 * the kernel heap on top of it, and the table memory user address spaces
 * are built from. Everything here hands out kernel virtual addresses in
 * the linear RAM map (layout.h). */
#ifndef MINIKERN_KERNEL_MEM_H
#define MINIKERN_KERNEL_MEM_H

#include <stddef.h>
#include <stdint.h>

#include "arch/arm/pgtable.h"
#include "core/bootinfo.h"

/* Takes over RAM: everything in info's memory range becomes free except
 * the kernel image (and what lies below it: the ATAGs), the boot block at
 * bootblock_phys (bootblock_len bytes) and the initrd. */
void mem_init(const struct bootinfo *info, uint32_t bootblock_phys, uint32_t bootblock_len);

/* npages zeroed, contiguous, physically aligned to align_pages pages.
 * Returns the kernel address, or NULL. */
void *page_alloc(uint32_t npages, uint32_t align_pages);
void page_free(void *virt, uint32_t npages);

/* A single page by physical address: user memory is tracked this way. */
int page_alloc_phys(uint32_t *phys);
void page_free_phys(uint32_t phys);

void *kmalloc(size_t n);
void *kzalloc(size_t n);
void kfree(void *p);

/* Pages free right now, for the boot log and leak checks. */
uint32_t mem_free_pages(void);

/* The table memory for pgtable_create. */
const struct pt_mem *mem_table_port(void);

#endif
