/* bootinfo — what the boot loader told us in r2.
 *
 * U-Boot's bootm hands over either a flattened device tree (modern sunxi
 * builds) or an ATAG list (QEMU's direct -kernel boot). This module tells
 * them apart and reads either. Pure: it reads only the memory it is pointed
 * at, bounded by the caller. */
#ifndef MINIKERN_CORE_BOOTINFO_H
#define MINIKERN_CORE_BOOTINFO_H

#include <stddef.h>
#include <stdint.h>

enum bootinfo_source {
    BOOTINFO_NONE = 0,  /* r2 was 0 or unrecognised (e.g. QEMU ELF boot) */
    BOOTINFO_ATAGS,
    BOOTINFO_FDT,
};

struct bootinfo {
    enum bootinfo_source source;
    uint32_t mem_base;
    uint32_t mem_size;    /* 0 when the loader did not say */
    const char *cmdline;  /* "" when absent; points into the tag list or blob */
    uint32_t initrd_start;  /* physical; both 0 when the loader passed none */
    uint32_t initrd_end;    /* exclusive */
    uint32_t block_size;    /* bytes of the ATAG list or blob itself: kept while cmdline is used */
};

/* Classifies the block r2 points at by its first words. */
enum bootinfo_source bootinfo_classify(const uint32_t *words);

/* Reads an ATAG list into out. Returns KERR_OK, or KERR_INVAL when the list
 * does not start with ATAG_CORE or runs past max_words without ATAG_NONE. */
int bootinfo_from_atags(const uint32_t *tags, size_t max_words, struct bootinfo *out);

/* Reads a device tree blob of at most max_size bytes: /memory reg (honouring
 * the root's #address-cells and #size-cells), /chosen bootargs and
 * linux,initrd-start/-end. Returns KERR_OK, or KERR_INVAL for a blob the
 * reader rejects; a missing property is not an error. */
int bootinfo_from_fdt(const void *blob, size_t max_size, struct bootinfo *out);

/* Caps the memory size at what the board really has: QEMU's orangepi-pc
 * insists on 1 GiB while the NanoPi NEO carries 512 MiB. */
void bootinfo_clamp_memory(struct bootinfo *info, uint32_t board_max);

#endif
