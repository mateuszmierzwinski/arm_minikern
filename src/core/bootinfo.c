#include "core/bootinfo.h"

#include "core/errno.h"
#include "core/fdt.h"

/* Tag numbers and sizes are the ARM Linux boot protocol (an ABI, not code). */
#define ATAG_NONE    0x00000000u
#define ATAG_CORE    0x54410001u
#define ATAG_MEM     0x54410002u
#define ATAG_INITRD2 0x54420005u  /* physical start, size */
#define ATAG_CMDLINE 0x54410009u
#define FDT_MAGIC_LE 0xedfe0dd0u  /* 0xd00dfeed, stored big-endian */
#define ATAG_HEADER_WORDS 2u

enum bootinfo_source bootinfo_classify(const uint32_t *words) {
    if (words == NULL) {
        return BOOTINFO_NONE;
    }
    if (words[0] == FDT_MAGIC_LE) {
        return BOOTINFO_FDT;
    }
    if (words[1] == ATAG_CORE && words[0] >= ATAG_HEADER_WORDS) {
        return BOOTINFO_ATAGS;
    }
    return BOOTINFO_NONE;
}

/* Applies one tag's payload; tags we do not need are skipped. */
static void apply_tag(const uint32_t *tag, struct bootinfo *out) {
    uint32_t words = tag[0];
    if (tag[1] == ATAG_MEM && words >= 4) {
        out->mem_size = tag[2];
        out->mem_base = tag[3];
    } else if (tag[1] == ATAG_CMDLINE && words > ATAG_HEADER_WORDS) {
        out->cmdline = (const char *)&tag[2];
    } else if (tag[1] == ATAG_INITRD2 && words >= 4) {
        out->initrd_start = tag[2];
        out->initrd_end = tag[2] + tag[3];
    }
}

int bootinfo_from_atags(const uint32_t *tags, size_t max_words, struct bootinfo *out) {
    size_t at = 0;
    *out = (struct bootinfo){.source = BOOTINFO_ATAGS, .cmdline = ""};
    if (bootinfo_classify(tags) != BOOTINFO_ATAGS) {
        return KERR_INVAL;
    }
    while (at + ATAG_HEADER_WORDS <= max_words) {
        uint32_t words = tags[at];
        if (words == 0 || tags[at + 1] == ATAG_NONE) {
            out->block_size = (uint32_t)(at + ATAG_HEADER_WORDS) * 4u;
            return KERR_OK;
        }
        if (words < ATAG_HEADER_WORDS || at + words > max_words) {
            return KERR_INVAL;
        }
        apply_tag(&tags[at], out);
        at += words;
    }
    return KERR_INVAL;
}

/* /memory reg: the first (base, size) pair, in the root's cell sizes. */
static int fdt_read_memory(const struct fdt *fdt, struct bootinfo *out) {
    uint32_t acells = fdt_get_u32(fdt, "/", "#address-cells", 2);
    uint32_t scells = fdt_get_u32(fdt, "/", "#size-cells", 1);
    struct fdt_prop reg;
    int rc = fdt_get_prop(fdt, "/memory", "reg", &reg);
    if (rc == KERR_NOENT) {
        return KERR_OK;
    }
    if (rc != KERR_OK || fdt_prop_cells(&reg, 0, acells, &out->mem_base) != KERR_OK ||
        fdt_prop_cells(&reg, acells, scells, &out->mem_size) != KERR_OK) {
        return KERR_INVAL;
    }
    return KERR_OK;
}

/* /chosen bootargs, accepted only when NUL-terminated inside its value. */
static void fdt_read_bootargs(const struct fdt *fdt, struct bootinfo *out) {
    struct fdt_prop p;
    if (fdt_get_prop(fdt, "/chosen", "bootargs", &p) == KERR_OK && p.len > 0 && p.value[p.len - 1] == '\0') {
        out->cmdline = (const char *)p.value;
    }
}

/* Reads a 1- or 2-cell /chosen number; the cell count is the value's size. */
static int fdt_read_chosen_number(const struct fdt *fdt, const char *prop, uint32_t *v) {
    struct fdt_prop p;
    if (fdt_get_prop(fdt, "/chosen", prop, &p) != KERR_OK) {
        return KERR_NOENT;
    }
    return fdt_prop_cells(&p, 0, p.len / 4u, v);
}

/* The initrd counts only when both ends are present, valid and ordered. */
static void fdt_read_initrd(const struct fdt *fdt, struct bootinfo *out) {
    uint32_t start, end;
    if (fdt_read_chosen_number(fdt, "linux,initrd-start", &start) == KERR_OK &&
        fdt_read_chosen_number(fdt, "linux,initrd-end", &end) == KERR_OK && end > start) {
        out->initrd_start = start;
        out->initrd_end = end;
    }
}

int bootinfo_from_fdt(const void *blob, size_t max_size, struct bootinfo *out) {
    struct fdt fdt;
    struct bootinfo info = {.source = BOOTINFO_FDT, .cmdline = ""};
    if (fdt_open(&fdt, blob, max_size) != KERR_OK || fdt_read_memory(&fdt, &info) != KERR_OK) {
        return KERR_INVAL;
    }
    info.block_size = fdt.total_size;
    fdt_read_bootargs(&fdt, &info);
    fdt_read_initrd(&fdt, &info);
    *out = info;
    return KERR_OK;
}

void bootinfo_clamp_memory(struct bootinfo *info, uint32_t board_max) {
    if (info->mem_size == 0 || info->mem_size > board_max) {
        info->mem_size = board_max;
    }
}
