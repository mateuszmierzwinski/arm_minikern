#include "kernel/dma.h"

#include "arch/arm/layout.h"
#include "arch/arm/mmu.h"
#include "core/errno.h"
#include "kernel/mem.h"

#define SECTION_PAGES (DMA_MAP_SIZE / 4096u)

static uint32_t window_phys;
static uint32_t used;

/* Gives the window to the device, which is three steps and not one.
 *
 * page_alloc zeroed it through the cacheable linear map, so the CPU is
 * holding up to a cache's worth of dirty lines full of zeroes for memory a
 * device is about to own. Those are written back at whatever moment the
 * cache chooses to evict them — in the middle of a packet, days later — so
 * they are cleaned out and dropped first.
 *
 * Then the cacheable alias itself goes. Two mappings of one physical page
 * with different cacheability is a mismatched-attributes alias, which the
 * architecture does not promise to keep coherent (ARM ARM B2.9), and
 * nothing needs the window by its linear address: dma_alloc hands out the
 * non-cacheable one. Unmapping it means an accidental access faults loudly
 * instead of corrupting a frame quietly. */
static void hand_to_device(void *window) {
    mmu_flush_dcache_range(window, DMA_MAP_SIZE);
    mmu_unmap_kernel_section((uint32_t)(uintptr_t)window);
    mmu_map_kernel_section(DMA_VIRT_BASE, virt_to_phys(window), SECT_KERNEL_DMA);
}

int dma_init(void) {
    void *section = page_alloc(SECTION_PAGES, SECTION_PAGES);
    if (section == NULL) {
        return KERR_NOMEM;
    }
    window_phys = virt_to_phys(section);
    hand_to_device(section);
    return KERR_OK;
}

uint32_t dma_window_phys(void) {
    return window_phys;
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
