#include "kernel/mem.h"

#include "arch/arm/layout.h"
#include "core/errno.h"
#include "core/kheap.h"
#include "core/pmm.h"
#include "kernel/panic.h"

#define RAM_PAGES (RAM_MAP_SIZE / PMM_PAGE_SIZE)
#define L2_PER_PAGE (PMM_PAGE_SIZE / PT_L2_BYTES)

extern uint8_t __kernel_end[];

static uint32_t bitmap[PMM_BITMAP_WORDS(RAM_PAGES)];
static struct pmm pmm;
static struct kheap heap;

/* Spare 1 KiB second-level tables, linked through their first word. */
static uint32_t *l2_spares;

static void zero_pages(void *virt, uint32_t npages) {
    uint32_t *w = virt;
    for (uint32_t i = 0; i < npages * PMM_PAGE_SIZE / 4u; i++) {
        w[i] = 0;
    }
}

void *page_alloc(uint32_t npages, uint32_t align_pages) {
    uint32_t phys;
    void *virt;
    if (pmm_alloc(&pmm, npages, align_pages, &phys) != KERR_OK) {
        return NULL;
    }
    virt = phys_to_virt(phys);
    zero_pages(virt, npages);
    return virt;
}

void page_free(void *virt, uint32_t npages) {
    if (pmm_free(&pmm, virt_to_phys(virt), npages) != KERR_OK) {
        panic("page_free: bad free of %p (%u pages)", virt, npages);
    }
}

int page_alloc_phys(uint32_t *phys) {
    void *virt = page_alloc(1, 1);
    if (virt == NULL) {
        return KERR_NOMEM;
    }
    *phys = virt_to_phys(virt);
    return KERR_OK;
}

void page_free_phys(uint32_t phys) {
    page_free(phys_to_virt(phys), 1);
}

static int heap_pages_alloc(void *self, uint32_t npages, void **virt) {
    (void)self;
    *virt = page_alloc(npages, 1);
    return *virt == NULL ? KERR_NOMEM : KERR_OK;
}

static void heap_pages_free(void *self, void *virt, uint32_t npages) {
    (void)self;
    page_free(virt, npages);
}

void *kmalloc(size_t n) {
    return kheap_alloc(&heap, n);
}

void *kzalloc(size_t n) {
    return kheap_zalloc(&heap, n);
}

void kfree(void *p) {
    if (kheap_free(&heap, p) != KERR_OK) {
        panic("kfree: bad free of %p", p);
    }
}

uint32_t mem_free_pages(void) {
    return pmm.free;
}

/* A 1 KiB L2 table from the spares, carving a fresh page when empty. */
static uint32_t *l2_take(void) {
    uint32_t *t;
    if (l2_spares == NULL) {
        uint8_t *page = page_alloc(1, 1);
        for (uint32_t i = 0; page != NULL && i < L2_PER_PAGE; i++) {
            t = (uint32_t *)(page + i * PT_L2_BYTES);
            t[0] = (uint32_t)(uintptr_t)l2_spares;
            l2_spares = t;
        }
    }
    t = l2_spares;
    if (t != NULL) {
        l2_spares = (uint32_t *)(uintptr_t)t[0];
        t[0] = 0;
    }
    return t;
}

static int table_alloc(void *self, uint32_t bytes, void **virt, uint32_t *phys) {
    (void)self;
    *virt = bytes == PT_L2_BYTES ? (void *)l2_take() : page_alloc(PT_L1_BYTES / PMM_PAGE_SIZE, 2);
    if (*virt == NULL) {
        return KERR_NOMEM;
    }
    *phys = virt_to_phys(*virt);
    return KERR_OK;
}

static void table_free(void *self, void *virt, uint32_t bytes) {
    uint32_t *t = virt;
    (void)self;
    if (bytes == PT_L2_BYTES) {
        for (uint32_t i = 0; i < PT_L2_ENTRIES; i++) {
            t[i] = 0;
        }
        t[0] = (uint32_t)(uintptr_t)l2_spares;
        l2_spares = t;
        return;
    }
    page_free(virt, PT_L1_BYTES / PMM_PAGE_SIZE);
}

static void *table_to_virt(void *self, uint32_t phys) {
    (void)self;
    return phys_to_virt(phys);
}

const struct pt_mem *mem_table_port(void) {
    static const struct pt_mem port = {.alloc = table_alloc, .free = table_free, .to_virt = table_to_virt};
    return &port;
}

void mem_init(const struct bootinfo *info, uint32_t bootblock_phys, uint32_t bootblock_len) {
    static const struct page_port heap_port = {.alloc = heap_pages_alloc, .free = heap_pages_free};
    pmm_init(&pmm, RAM_PHYS_BASE, RAM_PAGES, bitmap);
    pmm_release(&pmm, info->mem_base, info->mem_size);
    pmm_reserve(&pmm, RAM_PHYS_BASE, virt_to_phys(__kernel_end) - RAM_PHYS_BASE);
    pmm_reserve(&pmm, bootblock_phys, bootblock_len);
    if (info->initrd_end > info->initrd_start) {
        pmm_reserve(&pmm, info->initrd_start, info->initrd_end - info->initrd_start);
    }
    kheap_init(&heap, &heap_port);
}
