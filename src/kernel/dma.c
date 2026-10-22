#include "kernel/dma.h"

#include "arch/arm/layout.h"
#include "arch/arm/mmu.h"
#include "core/errno.h"
#include "kernel/mem.h"

#define SECTION_PAGES (DMA_MAP_SIZE / 4096u)

static uint32_t window_phys;
static uint32_t used;

int dma_init(void) {
    void *section = page_alloc(SECTION_PAGES, SECTION_PAGES);
    if (section == NULL) {
        return KERR_NOMEM;
    }
    window_phys = virt_to_phys(section);
    mmu_map_kernel_section(DMA_VIRT_BASE, window_phys, SECT_KERNEL_DMA);
    return KERR_OK;
}

void *dma_alloc(uint32_t bytes, uint32_t align, uint32_t *phys) {
    uint32_t at = (used + align - 1u) & ~(align - 1u);
    uint8_t *p;
    if (window_phys == 0 || at > DMA_MAP_SIZE || bytes > DMA_MAP_SIZE - at) {
        return NULL;
    }
    used = at + bytes;
    p = (uint8_t *)(uintptr_t)(DMA_VIRT_BASE + at);
    for (uint32_t i = 0; i < bytes; i++) {
        p[i] = 0;
    }
    *phys = window_phys + at;
    return p;
}
