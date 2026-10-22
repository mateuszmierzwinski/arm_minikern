/* pgtable — one user address space's translation table, ARMv7-A short
 * descriptors with TTBCR.N = 1 (ARM ARM B3.5): an 8 KiB first-level table
 * of 2048 entries covers 0-2 GiB, each pointing at a 1 KiB second-level
 * table of 256 small (4 KiB) pages.
 *
 * Pure C over a memory port, so the encoding is host-tested; the kernel
 * supplies real pages and the TLB maintenance (mmu.h) after each change.
 * User pages are normal write-back memory, non-global (ASID-tagged), and
 * never executable unless asked. */
#ifndef MINIKERN_ARCH_ARM_PGTABLE_H
#define MINIKERN_ARCH_ARM_PGTABLE_H

#include <stdint.h>

/* Access rights; the values are Linux's PROT_* so mmap's argument passes
 * straight through. Write implies read: the hardware has no write-only. */
#define PT_READ  1u
#define PT_WRITE 2u
#define PT_EXEC  4u

#define PT_L1_ENTRIES 2048u
#define PT_L1_BYTES   8192u
#define PT_L2_ENTRIES 256u
#define PT_L2_BYTES   1024u
#define PT_PAGE_SIZE  4096u

/* Where tables come from. alloc returns zeroed, aligned table memory: its
 * kernel address and physical address. to_virt maps a table's physical
 * address (as found in a descriptor) back to the kernel's pointer. */
struct pt_mem {
    int (*alloc)(void *self, uint32_t bytes, void **virt, uint32_t *phys);
    void (*free)(void *self, void *virt, uint32_t bytes);
    void *(*to_virt)(void *self, uint32_t phys);
    void *self;
};

struct pgtable {
    uint32_t *l1;
    uint32_t l1_phys;
    struct pt_mem mem;
};

/* Creates an empty address space. KERR_OK or KERR_NOMEM. */
int pgtable_create(struct pgtable *pt, const struct pt_mem *mem);

/* Maps the page at user address va to physical pa with prot (non-zero).
 * Replaces any existing mapping. KERR_OK, KERR_NOMEM (no second-level
 * table), or KERR_INVAL (va not page aligned or not in user space). */
int pgtable_map(struct pgtable *pt, uint32_t va, uint32_t pa, uint32_t prot);

/* Finds va's mapping: KERR_OK with *pa (the page's base) and *prot, or
 * KERR_NOENT. */
int pgtable_lookup(const struct pgtable *pt, uint32_t va, uint32_t *pa, uint32_t *prot);

/* Changes an existing mapping's rights. KERR_OK or KERR_NOENT. */
int pgtable_protect(struct pgtable *pt, uint32_t va, uint32_t prot);

/* Removes every mapping in [va, end), page aligned, calling release(ctx, pa)
 * for each page that was mapped (the caller owns the frames). Absent
 * second-level tables are skipped a MiB at a time. Returns the number of
 * pages unmapped. */
uint32_t pgtable_unmap_range(struct pgtable *pt, uint32_t va, uint32_t end,
                             void (*release)(void *ctx, uint32_t pa), void *ctx);

/* Frees the tables themselves (not the pages they map: unmap them first). */
void pgtable_destroy(struct pgtable *pt);

#endif
