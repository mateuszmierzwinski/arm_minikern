#include "kernel/mm.h"

#include "arch/arm/layout.h"
#include "arch/arm/mmu.h"
#include "core/errno.h"
#include "kernel/mem.h"

#define PAGE 4096u
#define PAGE_MASK 0xfffff000u

static uint32_t page_down(uint32_t a) {
    return a & PAGE_MASK;
}

static bool page_up(uint32_t a, uint32_t *out) {
    if (a > 0xfffff000u) {
        return false;
    }
    *out = (a + PAGE - 1u) & PAGE_MASK;
    return true;
}

static void release_frame(void *ctx, uint32_t pa) {
    (void)ctx;
    page_free_phys(pa);
}

/* vma_remove's callback: unmaps and frees the pages of a removed range. */
static void drop_range(void *ctx, uint32_t lo, uint32_t hi) {
    struct mm *mm = ctx;
    pgtable_unmap_range(&mm->pt, lo, hi, release_frame, NULL);
}

struct mm *mm_create(void) {
    struct mm *mm = kzalloc(sizeof *mm);
    if (mm == NULL) {
        return NULL;
    }
    if (pgtable_create(&mm->pt, mem_table_port()) != KERR_OK) {
        kfree(mm);
        return NULL;
    }
    mm->refs = 1;
    return mm;
}

void mm_put(struct mm *mm) {
    if (mm == NULL || --mm->refs > 0) {
        return;
    }
    pgtable_unmap_range(&mm->pt, 0, USER_TOP, release_frame, NULL);
    pgtable_destroy(&mm->pt);
    kfree(mm);
}

/* A user range [addr, addr+len) rounded out to pages, inside user space. */
static int user_range(uint32_t addr, uint32_t len, uint32_t *start, uint32_t *end) {
    if ((addr & ~PAGE_MASK) != 0 || len == 0 || !page_up(len, &len) || addr >= USER_TOP ||
        len > USER_TOP - addr) {
        return KERR_INVAL;
    }
    *start = addr;
    *end = addr + len;
    return KERR_OK;
}

/* Picks the address for a non-fixed mapping. */
static int place(struct mm *mm, uint32_t hint, uint32_t len, uint32_t *addr) {
    uint32_t h = page_down(hint);
    return vma_find_free(&mm->vmas, len, h, MM_MMAP_LO, MM_MMAP_TOP, addr) == KERR_OK ? KERR_OK : KERR_NOMEM;
}

int32_t mm_map(struct mm *mm, uint32_t addr, uint32_t len, uint32_t prot, uint32_t flags) {
    uint32_t start, end, at = addr;
    bool fixed = (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) != 0;
    if (!page_up(len, &len) || len == 0 || (fixed && user_range(addr, len, &start, &end) != KERR_OK)) {
        return KERR_INVAL;
    }
    if (!fixed && place(mm, addr, len, &at) != KERR_OK) {
        return KERR_NOMEM;
    }
    if ((flags & MAP_FIXED_NOREPLACE) && vma_find_free(&mm->vmas, len, at, at, at + len, &start) != KERR_OK) {
        return KERR_EXIST;
    }
    if ((flags & MAP_FIXED) && vma_remove(&mm->vmas, at, at + len, drop_range, mm) != KERR_OK) {
        return KERR_NOMEM;
    }
    mmu_flush_tlb();
    if (vma_insert(&mm->vmas, at, at + len, prot & (PT_READ | PT_WRITE | PT_EXEC), 0) != KERR_OK) {
        return KERR_NOMEM;
    }
    return (int32_t)at;
}

int mm_unmap(struct mm *mm, uint32_t addr, uint32_t len) {
    uint32_t start, end;
    int rc = user_range(addr, len, &start, &end);
    if (rc == KERR_OK) {
        rc = vma_remove(&mm->vmas, start, end, drop_range, mm);
        mmu_flush_tlb();
    }
    return rc;
}

/* Re-applies a range's region rights to the pages already mapped in it.
 * PROT_NONE cannot be expressed by a present page, so those pages are
 * dropped (their contents with them: a documented limitation). */
static void reprotect_pages(struct mm *mm, uint32_t start, uint32_t end, uint32_t prot) {
    for (uint32_t va = start; va < end; va += PAGE) {
        uint32_t pa, old;
        if (pgtable_lookup(&mm->pt, va, &pa, &old) != KERR_OK) {
            continue;
        }
        if (prot == 0) {
            pgtable_unmap_range(&mm->pt, va, va + PAGE, release_frame, NULL);
        } else {
            pgtable_protect(&mm->pt, va, prot);
        }
    }
}

int mm_protect(struct mm *mm, uint32_t addr, uint32_t len, uint32_t prot) {
    uint32_t start, end;
    int rc = user_range(addr, len, &start, &end);
    prot &= PT_READ | PT_WRITE | PT_EXEC;
    if (rc == KERR_OK) {
        rc = vma_protect(&mm->vmas, start, end, prot);
    }
    if (rc == KERR_OK) {
        reprotect_pages(mm, start, end, prot);
        mmu_flush_tlb();
    }
    return rc;
}

int mm_discard(struct mm *mm, uint32_t addr, uint32_t len) {
    uint32_t start, end;
    int rc = user_range(addr, len, &start, &end);
    if (rc == KERR_OK) {
        pgtable_unmap_range(&mm->pt, start, end, release_frame, NULL);
        mmu_flush_tlb();
    }
    return rc;
}

int mm_fault(struct mm *mm, uint32_t addr, uint32_t access) {
    const struct vma *v = vma_find(&mm->vmas, addr);
    uint32_t pa, prot;
    if (v == NULL || (v->prot & access) != access) {
        return KERR_FAULT;
    }
    if (pgtable_lookup(&mm->pt, page_down(addr), &pa, &prot) == KERR_OK) {
        return (prot & access) == access ? KERR_OK : KERR_FAULT;
    }
    if (page_alloc_phys(&pa) != KERR_OK) {
        return KERR_NOMEM;
    }
    if (pgtable_map(&mm->pt, page_down(addr), pa, v->prot) != KERR_OK) {
        page_free_phys(pa);
        return KERR_NOMEM;
    }
    return KERR_OK;
}

uint32_t mm_brk(struct mm *mm, uint32_t want) {
    uint32_t old_end, new_end;
    if (want < mm->brk_start || !page_up(want, &new_end) || !page_up(mm->brk, &old_end) || new_end >= MM_MMAP_LO) {
        return mm->brk;
    }
    if (new_end > old_end && vma_insert(&mm->vmas, old_end, new_end, PT_READ | PT_WRITE, 0) != KERR_OK) {
        return mm->brk;
    }
    if (new_end < old_end && vma_remove(&mm->vmas, new_end, old_end, drop_range, mm) != KERR_OK) {
        return mm->brk;
    }
    mmu_flush_tlb();
    mm->brk = want;
    return want;
}

void *mm_user_page(struct mm *mm, uint32_t uaddr, uint32_t access) {
    uint32_t pa, prot;
    if (uaddr >= USER_TOP || mm_fault(mm, uaddr, access) != KERR_OK ||
        pgtable_lookup(&mm->pt, page_down(uaddr), &pa, &prot) != KERR_OK) {
        return NULL;
    }
    return (uint8_t *)phys_to_virt(pa) + (uaddr & ~PAGE_MASK);
}

/* The bytes left in uaddr's page, capped at n. */
static uint32_t chunk(uint32_t uaddr, uint32_t n) {
    uint32_t room = PAGE - (uaddr & ~PAGE_MASK);
    return n < room ? n : room;
}

static void copy_bytes(uint8_t *dst, const uint8_t *src, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        dst[i] = src[i];
    }
}

int mm_copy_out(struct mm *mm, uint32_t uaddr, const void *src, uint32_t n) {
    const uint8_t *s = src;
    while (n > 0) {
        uint32_t c = chunk(uaddr, n);
        uint8_t *k = mm_user_page(mm, uaddr, PT_WRITE);
        if (k == NULL) {
            return KERR_FAULT;
        }
        copy_bytes(k, s, c);
        s += c;
        uaddr += c;
        n -= c;
    }
    return KERR_OK;
}

int mm_copy_in(struct mm *mm, void *dst, uint32_t uaddr, uint32_t n) {
    uint8_t *d = dst;
    while (n > 0) {
        uint32_t c = chunk(uaddr, n);
        const uint8_t *k = mm_user_page(mm, uaddr, PT_READ);
        if (k == NULL) {
            return KERR_FAULT;
        }
        copy_bytes(d, k, c);
        d += c;
        uaddr += c;
        n -= c;
    }
    return KERR_OK;
}

int mm_copy_string_in(struct mm *mm, char *dst, uint32_t uaddr, uint32_t cap) {
    for (uint32_t i = 0; i < cap; i++) {
        const char *k = mm_user_page(mm, uaddr + i, PT_READ);
        if (k == NULL) {
            return KERR_FAULT;
        }
        dst[i] = *k;
        if (*k == '\0') {
            return (int)i;
        }
    }
    return KERR_NAMETOOLONG;
}

bool mm_resident(const struct mm *mm, uint32_t addr) {
    uint32_t pa, prot;
    return pgtable_lookup(&mm->pt, page_down(addr), &pa, &prot) == KERR_OK;
}
