#include "core/pmm.h"

#include "core/errno.h"
#include "ktest.h"

#define BASE 0x40000000u
#define NPAGES 100u

static uint32_t bits[PMM_BITMAP_WORDS(NPAGES)];
static struct pmm p;

static void fresh(void) {
    pmm_init(&p, BASE, NPAGES, bits);
    pmm_release(&p, BASE, NPAGES * PMM_PAGE_SIZE);
}

static void test_starts_all_used(void) {
    uint32_t phys;
    pmm_init(&p, BASE, NPAGES, bits);
    KTEST_CHECK(p.free == 0);
    KTEST_CHECK(pmm_alloc(&p, 1, 0, &phys) == KERR_NOMEM);
}

static void test_release_rounds_inward(void) {
    pmm_init(&p, BASE, NPAGES, bits);
    pmm_release(&p, BASE + 100, 3 * PMM_PAGE_SIZE);  /* touches 4 pages, 2 whole */
    KTEST_CHECK(p.free == 2);
    pmm_release(&p, BASE - PMM_PAGE_SIZE, 2 * PMM_PAGE_SIZE);  /* half outside */
    KTEST_CHECK(p.free == 3);
    pmm_release(&p, 0xfffff000u, 0x2000u);  /* wraps past 4 GiB: ignored, no overflow */
    KTEST_CHECK(p.free == 3);
}

static void test_reserve_rounds_outward(void) {
    fresh();
    pmm_reserve(&p, BASE + 100, 10);
    KTEST_CHECK(p.free == NPAGES - 1);
    pmm_reserve(&p, BASE + PMM_PAGE_SIZE - 1, 2);  /* straddles pages 0 and 1 */
    KTEST_CHECK(p.free == NPAGES - 2);
}

static void test_alloc_free_roundtrip(void) {
    uint32_t a, b;
    fresh();
    KTEST_CHECK(pmm_alloc(&p, 1, 0, &a) == KERR_OK);
    KTEST_CHECK(pmm_alloc(&p, 1, 0, &b) == KERR_OK);
    KTEST_CHECK(a != b && a >= BASE && b >= BASE);
    KTEST_CHECK(p.free == NPAGES - 2);
    KTEST_CHECK(pmm_free(&p, a, 1) == KERR_OK);
    KTEST_CHECK(pmm_free(&p, a, 1) == KERR_INVAL);  /* double free */
    KTEST_CHECK(p.free == NPAGES - 1);
}

static void test_aligned_runs(void) {
    uint32_t a, b;
    fresh();
    KTEST_CHECK(pmm_alloc(&p, 1, 0, &a) == KERR_OK);  /* frame 0 */
    KTEST_CHECK(pmm_alloc(&p, 2, 2, &b) == KERR_OK);
    KTEST_CHECK(((b - BASE) >> PMM_PAGE_SHIFT) % 2 == 0 && b != a);
    KTEST_CHECK(pmm_alloc(&p, 4, 4, &b) == KERR_OK);
    KTEST_CHECK(((b - BASE) >> PMM_PAGE_SHIFT) % 4 == 0);
}

static void test_exhaustion(void) {
    uint32_t a;
    uint32_t n = 0;
    fresh();
    while (pmm_alloc(&p, 1, 0, &a) == KERR_OK) {
        n++;
    }
    KTEST_CHECK(n == NPAGES);
    KTEST_CHECK(pmm_free(&p, BASE + 50 * PMM_PAGE_SIZE, 1) == KERR_OK);
    KTEST_CHECK(pmm_alloc(&p, 1, 0, &a) == KERR_OK && a == BASE + 50 * PMM_PAGE_SIZE);
    KTEST_CHECK(pmm_alloc(&p, 101, 0, &a) == KERR_NOMEM);
    KTEST_CHECK(pmm_alloc(&p, 0, 0, &a) == KERR_NOMEM);
}

static void test_bad_frees(void) {
    fresh();
    KTEST_CHECK(pmm_free(&p, BASE - PMM_PAGE_SIZE, 1) == KERR_INVAL);
    KTEST_CHECK(pmm_free(&p, BASE + 1, 1) == KERR_INVAL);
    KTEST_CHECK(pmm_free(&p, BASE + NPAGES * PMM_PAGE_SIZE, 1) == KERR_INVAL);
    KTEST_CHECK(pmm_free(&p, BASE, NPAGES + 1) == KERR_INVAL);
    KTEST_CHECK(pmm_free(&p, BASE, 1) == KERR_INVAL);  /* free already */
}

int main(void) {
    KTEST_RUN(test_starts_all_used);
    KTEST_RUN(test_release_rounds_inward);
    KTEST_RUN(test_reserve_rounds_outward);
    KTEST_RUN(test_alloc_free_roundtrip);
    KTEST_RUN(test_aligned_runs);
    KTEST_RUN(test_exhaustion);
    KTEST_RUN(test_bad_frees);
    return ktest_failures();
}
