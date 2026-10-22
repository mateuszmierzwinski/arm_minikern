#include "arch/arm/pgtable.h"

#include <stddef.h>

#include "core/errno.h"

#define L1_COARSE     0x1u           /* first level: points at an L2 table */
#define L1_TYPE_MASK  0x3u
#define L1_L2_MASK    0xfffffc00u
#define L2_SMALL      0x2u           /* second level: a 4 KiB page ("1x": bit 0 is XN) */
#define L2_XN         0x1u
#define L2_NORMAL_WB  0x44cu         /* TEX=001 C=1 B=1 S=1 */
#define L2_AP_USER_RW 0x030u         /* APX=0 AP=11 */
#define L2_AP_USER_RO 0x220u         /* APX=1 AP=10 */
/* APX=0 AP=01: the kernel may touch it, user space may not. This is how a
 * page with no permissions stays **mapped**: dropping it instead would
 * throw away what it holds, so a program that protects a region and then
 * unprotects it would find it zeroed (mm.c). */
#define L2_AP_USER_NONE 0x010u
#define L2_AP_MASK      0x230u
#define L2_APX          0x200u
#define L2_NG         0x800u
#define PAGE_MASK     0xfffff000u
#define USER_LIMIT    0x80000000u

static int is_page(uint32_t desc) {
    return (desc & L2_SMALL) != 0;
}

static uint32_t l1_index(uint32_t va) {
    return va >> 20;
}

static uint32_t l2_index(uint32_t va) {
    return (va >> 12) & 0xffu;
}

/* The descriptor bits for prot. */
static uint32_t page_bits(uint32_t prot) {
    uint32_t bits = L2_SMALL | L2_NORMAL_WB | L2_NG;
    if (prot == 0) {
        bits |= L2_AP_USER_NONE | L2_XN;
    } else {
        bits |= (prot & PT_WRITE) ? L2_AP_USER_RW : L2_AP_USER_RO;
    }
    if ((prot & PT_EXEC) == 0) {
        bits |= L2_XN;
    }
    return bits;
}

static uint32_t page_prot(uint32_t desc) {
    uint32_t prot;
    if ((desc & L2_AP_MASK) == L2_AP_USER_NONE) {
        return 0; /* mapped, and nothing user space may do with it */
    }
    prot = PT_READ;
    prot |= (desc & L2_APX) ? 0u : PT_WRITE;
    prot |= (desc & L2_XN) ? 0u : PT_EXEC;
    return prot;
}

/* The L2 table covering va, or NULL when none exists. */
static uint32_t *l2_table(const struct pgtable *pt, uint32_t va) {
    uint32_t e = pt->l1[l1_index(va)];
    if ((e & L1_TYPE_MASK) != L1_COARSE) {
        return NULL;
    }
    return pt->mem.to_virt(pt->mem.self, e & L1_L2_MASK);
}

/* The L2 table covering va, allocated on demand. */
static uint32_t *l2_table_alloc(struct pgtable *pt, uint32_t va) {
    uint32_t *l2 = l2_table(pt, va);
    void *virt;
    uint32_t phys;
    if (l2 != NULL) {
        return l2;
    }
    if (pt->mem.alloc(pt->mem.self, PT_L2_BYTES, &virt, &phys) != KERR_OK) {
        return NULL;
    }
    pt->l1[l1_index(va)] = (phys & L1_L2_MASK) | L1_COARSE;
    return virt;
}

int pgtable_create(struct pgtable *pt, const struct pt_mem *mem) {
    void *virt;
    uint32_t phys;
    if (mem->alloc(mem->self, PT_L1_BYTES, &virt, &phys) != KERR_OK) {
        return KERR_NOMEM;
    }
    *pt = (struct pgtable){.l1 = virt, .l1_phys = phys, .mem = *mem};
    return KERR_OK;
}

int pgtable_map(struct pgtable *pt, uint32_t va, uint32_t pa, uint32_t prot) {
    uint32_t *l2;
    if ((va & ~PAGE_MASK) != 0 || va >= USER_LIMIT || prot == 0) {
        return KERR_INVAL;
    }
    l2 = l2_table_alloc(pt, va);
    if (l2 == NULL) {
        return KERR_NOMEM;
    }
    l2[l2_index(va)] = (pa & PAGE_MASK) | page_bits(prot);
    return KERR_OK;
}

/* The descriptor slot for a mapped va, or NULL. */
static uint32_t *mapped_slot(const struct pgtable *pt, uint32_t va) {
    uint32_t *l2 = va < USER_LIMIT ? l2_table(pt, va) : NULL;
    if (l2 == NULL || !is_page(l2[l2_index(va)])) {
        return NULL;
    }
    return &l2[l2_index(va)];
}

int pgtable_lookup(const struct pgtable *pt, uint32_t va, uint32_t *pa, uint32_t *prot) {
    uint32_t *slot = mapped_slot(pt, va);
    if (slot == NULL) {
        return KERR_NOENT;
    }
    *pa = *slot & PAGE_MASK;
    *prot = page_prot(*slot);
    return KERR_OK;
}

int pgtable_protect(struct pgtable *pt, uint32_t va, uint32_t prot) {
    uint32_t *slot = mapped_slot(pt, va);
    if (slot == NULL) {
        return KERR_NOENT;
    }
    /* A protection of none is a protection, not an error: the page stays
     * where it is and user space loses every way of reaching it. */
    *slot = (*slot & PAGE_MASK) | page_bits(prot);
    return KERR_OK;
}

uint32_t pgtable_unmap_range(struct pgtable *pt, uint32_t va, uint32_t end,
                             void (*release)(void *ctx, uint32_t pa), void *ctx) {
    uint32_t count = 0;
    end = end > USER_LIMIT ? USER_LIMIT : end;
    for (va &= PAGE_MASK; va < end;) {
        uint32_t *l2 = l2_table(pt, va);
        if (l2 == NULL) {
            va = (va & 0xfff00000u) + 0x100000u;  /* next MiB */
            if (va == 0) {
                break;
            }
            continue;
        }
        if (is_page(l2[l2_index(va)])) {
            release(ctx, l2[l2_index(va)] & PAGE_MASK);
            l2[l2_index(va)] = 0;
            count++;
        }
        va += PT_PAGE_SIZE;
    }
    return count;
}

void pgtable_destroy(struct pgtable *pt) {
    for (uint32_t i = 0; i < PT_L1_ENTRIES; i++) {
        uint32_t *l2 = l2_table(pt, i << 20);
        if (l2 != NULL) {
            pt->mem.free(pt->mem.self, l2, PT_L2_BYTES);
        }
    }
    pt->mem.free(pt->mem.self, pt->l1, PT_L1_BYTES);
    pt->l1 = NULL;
}
