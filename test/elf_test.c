#include "core/elf.h"

#include "core/errno.h"
#include "ktest.h"

#define TOP 0x80000000u

static uint8_t f[8192];

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void phdr(int i, uint32_t type, uint32_t off, uint32_t vaddr, uint32_t filesz, uint32_t memsz, uint32_t flags) {
    uint8_t *ph = f + 52 + i * 32;
    put32(ph, type);
    put32(ph + 4, off);
    put32(ph + 8, vaddr);
    put32(ph + 12, vaddr);
    put32(ph + 16, filesz);
    put32(ph + 20, memsz);
    put32(ph + 24, flags);
    put32(ph + 28, 4096);
}

/* The shape TinyGo emits: text from offset 0 at 0x10000 (covering the
 * headers), data on the next page, plus a TLS header we ignore. */
static void make_exec(void) {
    memset(f, 0, sizeof f);
    memcpy(f, "\x7f" "ELF\x01\x01\x01", 7);
    put16(f + 16, 2);
    put16(f + 18, 40);
    put32(f + 20, 1);
    put32(f + 24, 0x10100u);
    put32(f + 28, 52);
    put16(f + 42, 32);
    put16(f + 44, 3);
    phdr(0, 1, 0, 0x10000u, 4096, 4096, 5);
    phdr(1, 1, 4096, 0x21000u, 100, 8000, 6);
    phdr(2, 7, 4096, 0x21000u, 4, 4, 4);
}

static void test_parses_static_exec(void) {
    struct elf_image img;
    make_exec();
    KTEST_CHECK(elf_parse(f, sizeof f, TOP, &img) == KERR_OK);
    KTEST_CHECK(img.entry == 0x10100u && img.nloads == 2);
    KTEST_CHECK(img.loads[0].prot == 5 && img.loads[1].prot == 3);
    KTEST_CHECK(img.loads[1].memsz == 8000 && img.loads[1].filesz == 100);
    KTEST_CHECK(img.phdr_vaddr == 0x10034u && img.phnum == 3 && img.phent == 32);
}

static void test_rejects_other_formats(void) {
    struct elf_image img;
    make_exec();
    f[4] = 2;  /* ELFCLASS64 */
    KTEST_CHECK(elf_parse(f, sizeof f, TOP, &img) == KERR_NOEXEC);
    make_exec();
    put16(f + 18, 62);  /* x86-64 */
    KTEST_CHECK(elf_parse(f, sizeof f, TOP, &img) == KERR_NOEXEC);
    make_exec();
    put16(f + 16, 3);  /* ET_DYN (PIE) */
    KTEST_CHECK(elf_parse(f, sizeof f, TOP, &img) == KERR_NOEXEC);
    make_exec();
    phdr(2, 3, 4096, 0, 10, 10, 4);  /* PT_INTERP: dynamic */
    KTEST_CHECK(elf_parse(f, sizeof f, TOP, &img) == KERR_NOEXEC);
    KTEST_CHECK(elf_parse(f, 10, TOP, &img) == KERR_NOEXEC);
    KTEST_CHECK(elf_parse(NULL, 100, TOP, &img) == KERR_NOEXEC);
}

static void test_rejects_hostile_headers(void) {
    struct elf_image img;
    make_exec();
    put16(f + 44, 300);  /* phnum past the file */
    KTEST_CHECK(elf_parse(f, 1000, TOP, &img) == KERR_INVAL);
    make_exec();
    phdr(1, 1, 8000, 0x21000u, 1000, 1000, 6);  /* file bytes beyond the end */
    KTEST_CHECK(elf_parse(f, sizeof f, TOP, &img) == KERR_INVAL);
    make_exec();
    phdr(1, 1, 4096, 0x7ffff000u, 100, 0x2000, 6);  /* crosses into the kernel */
    KTEST_CHECK(elf_parse(f, sizeof f, TOP, &img) == KERR_INVAL);
    make_exec();
    phdr(1, 1, 4096, 0xfffff000u, 100, 0x2000, 6);  /* wraps */
    KTEST_CHECK(elf_parse(f, sizeof f, TOP, &img) == KERR_INVAL);
}

static void test_rejects_bad_segments(void) {
    struct elf_image img;
    make_exec();
    phdr(1, 1, 4096, 0x21000u, 200, 100, 6);  /* filesz > memsz */
    KTEST_CHECK(elf_parse(f, sizeof f, TOP, &img) == KERR_INVAL);
    make_exec();
    phdr(0, 1, 0, 0, 4096, 4096, 5);  /* maps page 0 */
    KTEST_CHECK(elf_parse(f, sizeof f, TOP, &img) == KERR_INVAL);
    make_exec();
    phdr(1, 1, 4096, 0x21800u, 100, 100, 6);  /* offset and vaddr disagree mod page */
    KTEST_CHECK(elf_parse(f, sizeof f, TOP, &img) == KERR_INVAL);
    make_exec();
    put32(f + 24, 0x90000000u);  /* entry in kernel space */
    KTEST_CHECK(elf_parse(f, sizeof f, TOP, &img) == KERR_INVAL);
    make_exec();
    put32(f + 28, 0xffffffe0u);  /* phoff wraps */
    KTEST_CHECK(elf_parse(f, sizeof f, TOP, &img) == KERR_INVAL);
}

static void test_too_many_loads(void) {
    struct elf_image img;
    make_exec();
    put16(f + 44, ELF_MAX_LOADS + 1);
    for (uint32_t i = 0; i <= ELF_MAX_LOADS; i++) {
        phdr((int)i, 1, 0, 0x10000u + i * 0x10000u, 16, 16, 4);
    }
    KTEST_CHECK(elf_parse(f, sizeof f, TOP, &img) == KERR_INVAL);
}

int main(void) {
    KTEST_RUN(test_parses_static_exec);
    KTEST_RUN(test_rejects_other_formats);
    KTEST_RUN(test_rejects_hostile_headers);
    KTEST_RUN(test_rejects_bad_segments);
    KTEST_RUN(test_too_many_loads);
    return ktest_failures();
}
