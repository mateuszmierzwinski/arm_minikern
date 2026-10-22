#include "arch/arm/pgtable.h"

#include <stdlib.h>

#include "core/errno.h"
#include "ktest.h"

/* "Physical memory" is an arena; a table's phys is its offset + 0x40000000. */
#define ARENA_SIZE (1u << 20)
#define FAKE_PHYS  0x40000000u

static uint8_t *arena;
static uint32_t arena_used;
static int tables_live;

static int fake_alloc(void *self, uint32_t bytes, void **virt, uint32_t *phys) {
    (void)self;
    arena_used = (arena_used + bytes - 1u) & ~(bytes - 1u);  /* natural alignment */
    if (arena_used + bytes > ARENA_SIZE) {
        return KERR_NOMEM;
    }
    *virt = arena + arena_used;
    memset(*virt, 0, bytes);
    *phys = FAKE_PHYS + arena_used;
    arena_used += bytes;
    tables_live++;
    return KERR_OK;
}

static void fake_free(void *self, void *virt, uint32_t bytes) {
    (void)self;
    (void)virt;
    (void)bytes;
    tables_live--;
}

static void *fake_to_virt(void *self, uint32_t phys) {
    (void)self;
    return arena + (phys - FAKE_PHYS);
}

static const struct pt_mem mem = {.alloc = fake_alloc, .free = fake_free, .to_virt = fake_to_virt};
static struct pgtable pt;

static void fresh(void) {
    if (arena == NULL) {
        arena = aligned_alloc(PT_L1_BYTES, ARENA_SIZE);
    }
    arena_used = 0;
    tables_live = 0;
    KTEST_CHECK(pgtable_create(&pt, &mem) == KERR_OK);
}

static void test_map_and_lookup(void) {
    uint32_t pa, prot;
    fresh();
    KTEST_CHECK(pgtable_map(&pt, 0x10000u, 0x45678000u, PT_READ | PT_EXEC) == KERR_OK);
    KTEST_CHECK(pgtable_lookup(&pt, 0x10000u, &pa, &prot) == KERR_OK);
    KTEST_CHECK(pa == 0x45678000u && prot == (PT_READ | PT_EXEC));
    KTEST_CHECK(pgtable_lookup(&pt, 0x11000u, &pa, &prot) == KERR_NOENT);
    KTEST_CHECK(pgtable_lookup(&pt, 0x7ff00000u, &pa, &prot) == KERR_NOENT);
    KTEST_CHECK(pt.l1_phys % PT_L1_BYTES == 0);
}

/* The exact descriptor bits, checked against ARM ARM B3.5 by hand. */
static void test_descriptor_encoding(void) {
    uint32_t *l2;
    fresh();
    KTEST_CHECK(pgtable_map(&pt, 0x00200000u, 0x41000000u, PT_READ | PT_WRITE) == KERR_OK);
    KTEST_CHECK((pt.l1[2] & 3u) == 1u);  /* coarse table */
    l2 = fake_to_virt(NULL, pt.l1[2] & 0xfffffc00u);
    KTEST_CHECK(l2[0] == (0x41000000u | 0x800u | 0x44cu | 0x030u | 0x2u | 0x1u));  /* nG WB RW small XN */
    KTEST_CHECK(pgtable_protect(&pt, 0x00200000u, PT_READ) == KERR_OK);
    KTEST_CHECK(l2[0] == (0x41000000u | 0x800u | 0x44cu | 0x220u | 0x2u | 0x1u));  /* APX=1 AP=10 */
    KTEST_CHECK(pgtable_protect(&pt, 0x00200000u, PT_READ | PT_EXEC) == KERR_OK);
    KTEST_CHECK((l2[0] & 1u) == 0);
}

static void test_rejects_bad_requests(void) {
    fresh();
    KTEST_CHECK(pgtable_map(&pt, 0x10001u, 0x40000000u, PT_READ) == KERR_INVAL);
    KTEST_CHECK(pgtable_map(&pt, 0x80000000u, 0x40000000u, PT_READ) == KERR_INVAL);
    KTEST_CHECK(pgtable_map(&pt, 0xc0000000u, 0x40000000u, PT_READ) == KERR_INVAL);
    KTEST_CHECK(pgtable_map(&pt, 0x10000u, 0x40000000u, 0) == KERR_INVAL);
    KTEST_CHECK(pgtable_protect(&pt, 0x10000u, PT_READ) == KERR_NOENT);
}

static void count_release(void *ctx, uint32_t pa) {
    (void)pa;
    (*(int *)ctx)++;
}

static void test_unmap_range(void) {
    int released = 0;
    uint32_t pa, prot;
    fresh();
    for (uint32_t va = 0x000ff000u; va < 0x00102000u; va += PT_PAGE_SIZE) {  /* straddles a MiB */
        KTEST_CHECK(pgtable_map(&pt, va, 0x40000000u + va, PT_READ) == KERR_OK);
    }
    KTEST_CHECK(pgtable_map(&pt, 0x7ffff000u, 0x40001000u, PT_READ) == KERR_OK);
    KTEST_CHECK(pgtable_unmap_range(&pt, 0x10000u, 0x80000000u, count_release, &released) == 4u);
    KTEST_CHECK(released == 4);
    KTEST_CHECK(pgtable_lookup(&pt, 0x00100000u, &pa, &prot) == KERR_NOENT);
    KTEST_CHECK(pgtable_unmap_range(&pt, 0, 0xffffffffu, count_release, &released) == 0u);
}

static void test_destroy_frees_tables(void) {
    fresh();
    KTEST_CHECK(pgtable_map(&pt, 0x10000u, 0x40000000u, PT_READ) == KERR_OK);
    KTEST_CHECK(pgtable_map(&pt, 0x7f000000u, 0x40000000u, PT_READ) == KERR_OK);
    KTEST_CHECK(tables_live == 3);
    pgtable_destroy(&pt);
    KTEST_CHECK(tables_live == 0 && pt.l1 == NULL);
}

static void test_out_of_tables(void) {
    fresh();
    arena_used = ARENA_SIZE;
    KTEST_CHECK(pgtable_map(&pt, 0x10000u, 0x40000000u, PT_READ) == KERR_NOMEM);
}

/* A page with no permissions stays mapped: that is what lets a program
 * protect a region and unprotect it without losing what it holds, and it is
 * the whole difference between this and dropping the page. */
static void a_page_with_no_permissions_is_still_mapped(void) {
    uint32_t pa = 0, prot = 0xff;
    fresh();
    KTEST_CHECK(pgtable_map(&pt, 0x20000u, 0x41000000u, PT_READ | PT_WRITE) == KERR_OK);
    KTEST_CHECK(pgtable_protect(&pt, 0x20000u, 0) == KERR_OK);
    KTEST_CHECK(pgtable_lookup(&pt, 0x20000u, &pa, &prot) == KERR_OK);
    KTEST_CHECK(pa == 0x41000000u); /* the same frame, still there */
    KTEST_CHECK(prot == 0);         /* and nothing user space may do with it */
    /* And back again, with the frame unchanged. */
    KTEST_CHECK(pgtable_protect(&pt, 0x20000u, PT_READ | PT_WRITE) == KERR_OK);
    KTEST_CHECK(pgtable_lookup(&pt, 0x20000u, &pa, &prot) == KERR_OK);
    KTEST_CHECK(pa == 0x41000000u && prot == (PT_READ | PT_WRITE));
}

int main(void) {
    KTEST_RUN(test_map_and_lookup);
    KTEST_RUN(a_page_with_no_permissions_is_still_mapped);
    KTEST_RUN(test_descriptor_encoding);
    KTEST_RUN(test_rejects_bad_requests);
    KTEST_RUN(test_unmap_range);
    KTEST_RUN(test_destroy_frees_tables);
    KTEST_RUN(test_out_of_tables);
    return ktest_failures();
}
