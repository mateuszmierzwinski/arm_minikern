/* dma — memory a device reads and writes behind the CPU's back. One 1 MiB
 * section of RAM is mapped a second time, non-cacheable, at DMA_VIRT_BASE,
 * and handed out by a bump allocator: descriptor rings and packet buffers
 * live there, so no cache maintenance is needed around ownership changes.
 * Nothing is ever freed; drivers allocate once at start. */
#ifndef MINIKERN_KERNEL_DMA_H
#define MINIKERN_KERNEL_DMA_H

#include <stdint.h>

/* Reserves and maps the window. KERR_OK or KERR_NOMEM. */
int dma_init(void);

/* bytes of zeroed DMA memory, aligned to align (a power of two). Returns
 * the kernel pointer and sets *phys, or NULL when the window is used up. */
void *dma_alloc(uint32_t bytes, uint32_t align, uint32_t *phys);

#endif
