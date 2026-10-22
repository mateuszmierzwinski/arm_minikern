/* layout.h — the kernel's virtual address map. Included by C and by
 * assembly (so plain numbers only, no C suffixes or casts).
 *
 *   0x00000000 - 0x7fffffff  user space, per process, through TTBR0
 *   0xc0000000 - 0xdfffffff  all RAM, linearly: virt = phys + KERNEL_VIRT_OFFSET
 *   0xe0000000 - 0xe00fffff  the DMA window: 1 MiB of RAM again, normal non-cacheable
 *   0xf0000000 - 0xf1ffffff  on-chip I/O, phys 0x00000000 - 0x01ffffff, device memory
 *
 * TTBCR.N = 1 splits translation at 2 GiB: the upper half (the kernel) is
 * one table shared by every process, through TTBR1. The H3's peripherals,
 * GIC included, all sit below 32 MiB physical. */
#ifndef MINIKERN_ARCH_ARM_LAYOUT_H
#define MINIKERN_ARCH_ARM_LAYOUT_H

#define KERNEL_VIRT_OFFSET 0x80000000
#define RAM_PHYS_BASE      0x40000000
#define RAM_VIRT_BASE      0xc0000000
#define RAM_MAP_SIZE       0x20000000  /* 512 MiB, the NEO's DRAM */
#define IO_PHYS_BASE       0x00000000
#define IO_VIRT_BASE       0xf0000000
#define IO_MAP_SIZE        0x02000000
#define USER_TOP           0x80000000
#define DMA_VIRT_BASE      0xe0000000
#define DMA_MAP_SIZE       0x00100000

#define SECTION_SHIFT 20

/* Short-descriptor 1 MiB section entries (ARM ARM B3.5.1). Normal memory is
 * write-back write-allocate (TEX=001 C=1 B=1), shareable, kernel RW only.
 * Device memory is shared device (TEX=000 C=0 B=1), never executable. */
#define SECT_TYPE      0x2
#define SECT_B         0x4
#define SECT_C         0x8
#define SECT_XN        0x10
#define SECT_AP_KRW    0x400
#define SECT_TEX1      0x1000
#define SECT_S         0x10000
#define SECT_KERNEL_RAM 0x1140e   /* TYPE|B|C|AP_KRW|TEX1|S */
#define SECT_KERNEL_IO  0x416     /* TYPE|B|XN|AP_KRW */
#define SECT_KERNEL_DMA 0x11412   /* TYPE|XN|AP_KRW|TEX1|S: normal, non-cacheable */

/* TTBRn walk attributes: inner and outer write-back write-allocate, shareable. */
#define TTBR_WALK_ATTRS 0x4a

#ifndef __ASSEMBLER__
#include <stdint.h>

/* The kernel's view of a physical RAM address, and back. */
static inline void *phys_to_virt(uint32_t phys) {
    return (void *)(uintptr_t)(phys + (uint32_t)KERNEL_VIRT_OFFSET);
}

static inline uint32_t virt_to_phys(const void *virt) {
    return (uint32_t)(uintptr_t)virt - (uint32_t)KERNEL_VIRT_OFFSET;
}

/* The kernel's address of an on-chip peripheral register block. */
static inline uintptr_t io_virt(uint32_t phys) {
    return (uintptr_t)(phys - (uint32_t)IO_PHYS_BASE + (uint32_t)IO_VIRT_BASE);
}

/* True when [phys, phys+len) lies in the RAM the kernel maps. */
static inline int phys_in_ram(uint32_t phys, uint32_t len) {
    return phys >= (uint32_t)RAM_PHYS_BASE && len <= (uint32_t)RAM_MAP_SIZE &&
           phys - (uint32_t)RAM_PHYS_BASE <= (uint32_t)RAM_MAP_SIZE - len;
}
#endif

#endif
