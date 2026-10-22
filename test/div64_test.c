#include "lib/div64.h"

#include "ktest.h"

static void check_u(uint64_t n, uint64_t d) {
    uint64_t r;
    uint64_t q = div64_u(n, d, &r);
    KTEST_CHECK(q == n / d && r == n % d);
}

static void check_s(int64_t n, int64_t d) {
    int64_t r;
    int64_t q = div64_s(n, d, &r);
    KTEST_CHECK(q == n / d && r == n % d);
}

static void test_unsigned_matches_c(void) {
    uint64_t x = 0x9e3779b97f4a7c15ull;
    check_u(0, 1);
    check_u(1, 1);
    check_u(~0ull, 1);
    check_u(~0ull, ~0ull);
    check_u(1000000000ull * 3600, 62500000);
    check_u(123456789012345ull, 1000000000ull);
    for (int i = 0; i < 2000; i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        check_u(x, (x >> (i % 64)) | 1u);
    }
}

static void test_signed_matches_c(void) {
    check_s(-7, 2);
    check_s(7, -2);
    check_s(-7, -2);
    check_s(INT64_MIN + 1, 3);
    check_s(-1000000000000ll, 7);
}

static void test_zero_divisor(void) {
    uint64_t r;
    KTEST_CHECK(div64_u(42, 0, &r) == 0 && r == 42);
}

int main(void) {
    KTEST_RUN(test_unsigned_matches_c);
    KTEST_RUN(test_signed_matches_c);
    KTEST_RUN(test_zero_divisor);
    return ktest_failures();
}
