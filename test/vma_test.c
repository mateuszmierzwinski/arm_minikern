#include "core/vma.h"

#include "core/errno.h"
#include "ktest.h"

#define P VMA_PAGE

static struct vma_set s;
static uint32_t gone_bytes;

static void count_gone(void *ctx, uint32_t lo, uint32_t hi) {
    (void)ctx;
    gone_bytes += hi - lo;
}

static void fresh(void) {
    s = (struct vma_set){0};
    gone_bytes = 0;
}

static void test_insert_find_merge(void) {
    fresh();
    KTEST_CHECK(vma_insert(&s, 10 * P, 20 * P, 3, 0) == KERR_OK);
    KTEST_CHECK(vma_insert(&s, 20 * P, 30 * P, 3, 0) == KERR_OK);  /* merges */
    KTEST_CHECK(s.n == 1 && s.v[0].start == 10 * P && s.v[0].end == 30 * P);
    KTEST_CHECK(vma_insert(&s, 30 * P, 31 * P, 1, 0) == KERR_OK);  /* different prot: no merge */
    KTEST_CHECK(s.n == 2);
    KTEST_CHECK(vma_find(&s, 15 * P)->prot == 3);
    KTEST_CHECK(vma_find(&s, 30 * P + 5)->prot == 1);
    KTEST_CHECK(vma_find(&s, 31 * P) == 0);
    KTEST_CHECK(vma_find(&s, 9 * P) == 0);
}

static void test_insert_rejects(void) {
    fresh();
    KTEST_CHECK(vma_insert(&s, 10 * P, 20 * P, 3, 0) == KERR_OK);
    KTEST_CHECK(vma_insert(&s, 19 * P, 21 * P, 3, 0) == KERR_INVAL);  /* overlap */
    KTEST_CHECK(vma_insert(&s, 5 * P, 11 * P, 3, 0) == KERR_INVAL);
    KTEST_CHECK(vma_insert(&s, 12 * P, 13 * P, 3, 0) == KERR_INVAL);  /* inside */
    KTEST_CHECK(vma_insert(&s, 30 * P + 1, 31 * P, 3, 0) == KERR_INVAL);
    KTEST_CHECK(vma_insert(&s, 40 * P, 40 * P, 3, 0) == KERR_INVAL);  /* empty */
}

static void test_remove_splits(void) {
    fresh();
    KTEST_CHECK(vma_insert(&s, 10 * P, 40 * P, 3, 0) == KERR_OK);
    KTEST_CHECK(vma_remove(&s, 20 * P, 25 * P, count_gone, 0) == KERR_OK);
    KTEST_CHECK(s.n == 2 && s.v[0].end == 20 * P && s.v[1].start == 25 * P);
    KTEST_CHECK(gone_bytes == 5 * P);
    KTEST_CHECK(vma_remove(&s, 0, 100 * P, count_gone, 0) == KERR_OK);
    KTEST_CHECK(s.n == 0 && gone_bytes == 30 * P);
    KTEST_CHECK(vma_remove(&s, 0, 100 * P, count_gone, 0) == KERR_OK);  /* nothing there: fine */
}

static void test_protect(void) {
    fresh();
    KTEST_CHECK(vma_insert(&s, 10 * P, 40 * P, 0, 0) == KERR_OK);  /* a PROT_NONE reservation */
    KTEST_CHECK(vma_protect(&s, 20 * P, 30 * P, 3) == KERR_OK);
    KTEST_CHECK(s.n == 3 && vma_find(&s, 25 * P)->prot == 3 && vma_find(&s, 35 * P)->prot == 0);
    KTEST_CHECK(vma_protect(&s, 10 * P, 40 * P, 0) == KERR_OK);  /* merges back */
    KTEST_CHECK(s.n == 1);
    KTEST_CHECK(vma_protect(&s, 30 * P, 50 * P, 3) == KERR_NOMEM);  /* runs into a hole */
    KTEST_CHECK(s.n == 1 && s.v[0].prot == 0);
}

static void test_find_free(void) {
    uint32_t a = 0;
    fresh();
    KTEST_CHECK(vma_find_free(&s, 4 * P, 0, 16 * P, 100 * P, &a) == KERR_OK && a == 96 * P);
    KTEST_CHECK(vma_insert(&s, 90 * P, 100 * P, 3, 0) == KERR_OK);
    KTEST_CHECK(vma_find_free(&s, 4 * P, 0, 16 * P, 100 * P, &a) == KERR_OK && a == 86 * P);
    KTEST_CHECK(vma_insert(&s, 20 * P, 88 * P, 3, 0) == KERR_OK);  /* 2-page gap at 88 */
    KTEST_CHECK(vma_find_free(&s, 2 * P, 0, 16 * P, 100 * P, &a) == KERR_OK && a == 88 * P);
    KTEST_CHECK(vma_find_free(&s, 4 * P, 0, 16 * P, 100 * P, &a) == KERR_OK && a == 16 * P);
    KTEST_CHECK(vma_find_free(&s, 5 * P, 0, 16 * P, 100 * P, &a) == KERR_NOMEM);
    KTEST_CHECK(vma_find_free(&s, 2 * P, 17 * P, 16 * P, 100 * P, &a) == KERR_OK && a == 17 * P);  /* hint */
    KTEST_CHECK(vma_find_free(&s, 2 * P, 19 * P, 16 * P, 100 * P, &a) == KERR_OK && a == 88 * P);  /* hint taken */
    KTEST_CHECK(vma_find_free(&s, 0, 0, 16 * P, 100 * P, &a) == KERR_NOMEM);
}

static void test_full_set(void) {
    fresh();
    for (uint32_t i = 0; i < VMA_MAX; i++) {
        KTEST_CHECK(vma_insert(&s, (2 * i) * P, (2 * i + 1) * P, i & 1, 0) == KERR_OK);
    }
    KTEST_CHECK(vma_insert(&s, 1000 * P, 1001 * P, 1, 0) == KERR_NOMEM);
    KTEST_CHECK(s.n == VMA_MAX);
    KTEST_CHECK(vma_remove(&s, 0, P, count_gone, 0) == KERR_OK);  /* whole region: no split */
    KTEST_CHECK(vma_insert(&s, 1000 * P, 1004 * P, 1, 0) == KERR_OK);
    KTEST_CHECK(vma_remove(&s, 1001 * P, 1002 * P, count_gone, 0) == KERR_NOMEM);  /* needs a slot */
    KTEST_CHECK(vma_find(&s, 1001 * P) != 0);  /* unchanged */
}

static void test_stack_is_not_merged(void) {
    fresh();
    KTEST_CHECK(vma_insert(&s, 10 * P, 20 * P, 3, VMA_GROWSDOWN) == KERR_OK);
    KTEST_CHECK(vma_insert(&s, 20 * P, 30 * P, 3, VMA_GROWSDOWN) == KERR_OK);
    KTEST_CHECK(s.n == 2);
}

int main(void) {
    KTEST_RUN(test_insert_find_merge);
    KTEST_RUN(test_insert_rejects);
    KTEST_RUN(test_remove_splits);
    KTEST_RUN(test_protect);
    KTEST_RUN(test_find_free);
    KTEST_RUN(test_full_set);
    KTEST_RUN(test_stack_is_not_merged);
    return ktest_failures();
}
