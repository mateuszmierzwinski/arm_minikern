#include "core/kheap.h"

#include <stdlib.h>

#include "core/errno.h"
#include "core/pmm.h"
#include "ktest.h"

/* A page port over aligned_alloc, with a budget so exhaustion is testable. */
struct fake_pages {
    int budget;
    int live;
};

static int fake_alloc(void *self, uint32_t npages, void **virt) {
    struct fake_pages *f = self;
    if (f->budget < (int)npages) {
        return KERR_NOMEM;
    }
    *virt = aligned_alloc(PMM_PAGE_SIZE, npages * PMM_PAGE_SIZE);
    f->budget -= (int)npages;
    f->live += (int)npages;
    return KERR_OK;
}

static void fake_free(void *self, void *virt, uint32_t npages) {
    struct fake_pages *f = self;
    f->live -= (int)npages;
    f->budget += (int)npages;
    free(virt);
}

static struct fake_pages fp;
static struct kheap h;

static void fresh(int budget) {
    struct page_port port = {.alloc = fake_alloc, .free = fake_free, .self = &fp};
    fp = (struct fake_pages){.budget = budget};
    kheap_init(&h, &port);
}

static void test_small_objects(void) {
    char *a, *b;
    fresh(16);
    a = kheap_alloc(&h, 10);
    b = kheap_alloc(&h, 10);
    KTEST_CHECK(a != NULL && b != NULL && a != b);
    KTEST_CHECK(((uintptr_t)a & 7u) == 0);
    memset(a, 0xaa, 10);
    memset(b, 0xbb, 10);
    KTEST_CHECK(a[9] == (char)0xaa && b[0] == (char)0xbb);
    KTEST_CHECK(fp.live == 1);  /* both from one 16-byte-class page */
    KTEST_CHECK(kheap_free(&h, a) == KERR_OK);
    KTEST_CHECK(kheap_alloc(&h, 12) == a);  /* same class: reused */
}

static void test_class_boundaries(void) {
    fresh(64);
    for (size_t n = 1; n <= KHEAP_MAX_SMALL; n = n * 2 + 1) {
        uint8_t *p = kheap_alloc(&h, n);
        KTEST_CHECK(p != NULL);
        memset(p, 0x5a, n);
        KTEST_CHECK(kheap_free(&h, p) == KERR_OK);
    }
    KTEST_CHECK(h.live_bytes == 0);
}

static void test_large_objects(void) {
    void *p;
    fresh(16);
    p = kheap_alloc(&h, 3 * PMM_PAGE_SIZE);
    KTEST_CHECK(p != NULL && fp.live == 4);  /* 3 pages + header spills into a 4th */
    KTEST_CHECK(kheap_free(&h, p) == KERR_OK);
    KTEST_CHECK(fp.live == 0 && h.live_bytes == 0);
}

static void test_zalloc_zeroes(void) {
    uint8_t *p;
    fresh(4);
    p = kheap_alloc(&h, 100);
    memset(p, 0xff, 100);
    kheap_free(&h, p);
    p = kheap_zalloc(&h, 100);
    for (int i = 0; i < 100; i++) {
        KTEST_CHECK(p[i] == 0);
    }
}

static void test_misuse_is_caught(void) {
    uint64_t on_stack[4] = {0};
    void *p;
    fresh(4);
    p = kheap_alloc(&h, 32);
    KTEST_CHECK(kheap_free(&h, NULL) == KERR_OK);
    KTEST_CHECK(kheap_free(&h, p) == KERR_OK);
    KTEST_CHECK(kheap_free(&h, p) == KERR_INVAL);  /* double free */
    KTEST_CHECK(kheap_free(&h, &on_stack[2]) == KERR_INVAL);
    KTEST_CHECK(kheap_free(&h, (char *)p + 1) == KERR_INVAL);
}

static void test_exhaustion_returns_null(void) {
    fresh(1);
    KTEST_CHECK(kheap_alloc(&h, 2 * PMM_PAGE_SIZE) == NULL);
    KTEST_CHECK(kheap_alloc(&h, 0) == NULL);
    KTEST_CHECK(kheap_alloc(&h, (size_t)-1) == NULL);
    KTEST_CHECK(kheap_alloc(&h, 100) != NULL);
    KTEST_CHECK(kheap_alloc(&h, 3000) == NULL);  /* the one page went to the 128 class */
}

int main(void) {
    KTEST_RUN(test_small_objects);
    KTEST_RUN(test_class_boundaries);
    KTEST_RUN(test_large_objects);
    KTEST_RUN(test_zalloc_zeroes);
    KTEST_RUN(test_misuse_is_caught);
    KTEST_RUN(test_exhaustion_returns_null);
    return ktest_failures();
}
