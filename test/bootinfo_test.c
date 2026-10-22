#include "core/bootinfo.h"

#include <stdint.h>

#include "core/errno.h"
#include "ktest.h"

/* The list QEMU 10.1 builds for orangepi-pc with -append (measured). */
static uint32_t qemu_atags[] = {
    5, 0x54410001u, 0, 0, 0,                   /* CORE */
    4, 0x54410002u, 0x40000000u, 0x40000000u,  /* MEM: 1 GiB at DRAM base */
    5, 0x54410009u, 0, 0, 0,                   /* CMDLINE, filled below */
    0, 0,                                      /* NONE */
};

static void fill_cmdline(void) {
    memcpy(&qemu_atags[11], "a=b x", 6);
}

static void test_classify(void) {
    uint32_t fdt[2] = {0xedfe0dd0u, 0};
    uint32_t junk[2] = {0x12345678u, 0x9abcdef0u};
    KTEST_CHECK(bootinfo_classify(qemu_atags) == BOOTINFO_ATAGS);
    KTEST_CHECK(bootinfo_classify(fdt) == BOOTINFO_FDT);
    KTEST_CHECK(bootinfo_classify(junk) == BOOTINFO_NONE);
    KTEST_CHECK(bootinfo_classify(NULL) == BOOTINFO_NONE);
}

static void test_reads_qemu_list(void) {
    struct bootinfo info;
    fill_cmdline();
    KTEST_CHECK(bootinfo_from_atags(qemu_atags, 64, &info) == KERR_OK);
    KTEST_CHECK(info.mem_base == 0x40000000u && info.mem_size == 0x40000000u);
    KTEST_CHECK_STR(info.cmdline, "a=b x");
    KTEST_CHECK(info.block_size == sizeof qemu_atags);
}

static void test_clamps_to_board(void) {
    struct bootinfo info = {.mem_size = 0x40000000u};
    bootinfo_clamp_memory(&info, 0x20000000u);
    KTEST_CHECK(info.mem_size == 0x20000000u);
    info.mem_size = 0;
    bootinfo_clamp_memory(&info, 0x20000000u);
    KTEST_CHECK(info.mem_size == 0x20000000u);
    info.mem_size = 0x10000000u;
    bootinfo_clamp_memory(&info, 0x20000000u);
    KTEST_CHECK(info.mem_size == 0x10000000u);
}

static void test_unterminated_list_is_rejected(void) {
    struct bootinfo info;
    KTEST_CHECK(bootinfo_from_atags(qemu_atags, 14, &info) == KERR_INVAL);
}

static void test_tag_running_past_bound_is_rejected(void) {
    uint32_t bad[] = {5, 0x54410001u, 0, 0, 0, 200, 0x54410009u, 0};
    struct bootinfo info;
    KTEST_CHECK(bootinfo_from_atags(bad, 8, &info) == KERR_INVAL);
}

static void test_undersized_tag_is_rejected(void) {
    uint32_t bad[] = {5, 0x54410001u, 0, 0, 0, 1, 0x54410002u, 0, 0, 0};
    struct bootinfo info;
    KTEST_CHECK(bootinfo_from_atags(bad, 10, &info) == KERR_INVAL);
}

static void test_not_atags_is_rejected(void) {
    uint32_t fdt[4] = {0xedfe0dd0u, 0, 0, 0};
    struct bootinfo info;
    KTEST_CHECK(bootinfo_from_atags(fdt, 4, &info) == KERR_INVAL);
}

static void test_reads_initrd2(void) {
    uint32_t tags[] = {2, 0x54410001u, 4, 0x54420005u, 0x48000000u, 0x1000u, 0, 0};
    struct bootinfo info;
    KTEST_CHECK(bootinfo_from_atags(tags, 8, &info) == KERR_OK);
    KTEST_CHECK(info.initrd_start == 0x48000000u && info.initrd_end == 0x48001000u);
}

int main(void) {
    KTEST_RUN(test_reads_initrd2);
    KTEST_RUN(test_classify);
    KTEST_RUN(test_reads_qemu_list);
    KTEST_RUN(test_clamps_to_board);
    KTEST_RUN(test_unterminated_list_is_rejected);
    KTEST_RUN(test_tag_running_past_bound_is_rejected);
    KTEST_RUN(test_undersized_tag_is_rejected);
    KTEST_RUN(test_not_atags_is_rejected);
    return ktest_failures();
}
