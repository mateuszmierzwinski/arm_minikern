#include "lib/sha256.h"

#include <stdlib.h>

#include "ktest.h"

/* The digest of data as hex. */
static const char *digest_of(const void *data, size_t len) {
    static char hex[65];
    uint8_t d[SHA256_DIGEST];
    sha256(data, len, d);
    sha256_format(d, hex);
    return hex;
}

/* FIPS 180-4 and the NIST byte-oriented test vectors. */
static void test_published_vectors(void) {
    KTEST_CHECK_STR(digest_of("", 0), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    KTEST_CHECK_STR(digest_of("abc", 3), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    KTEST_CHECK_STR(digest_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56),
                    "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    KTEST_CHECK_STR(digest_of("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopq"
                              "klmnopqrlmnopqrsmnopqrstnopqrstu", 112),
                    "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
}

/* A million 'a's: the long-message vector, and the length field past 2^32 bits. */
static void test_long_message(void) {
    char *big = malloc(1000000);
    memset(big, 'a', 1000000);
    KTEST_CHECK_STR(digest_of(big, 1000000), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    free(big);
}

/* Every chunking of a message must give the same digest: the padding and
 * the 56/64-byte boundaries are where a streaming hash goes wrong. */
static void test_streaming_matches_oneshot(void) {
    uint8_t data[300], want[SHA256_DIGEST], got[SHA256_DIGEST];
    for (uint32_t i = 0; i < sizeof data; i++) {
        data[i] = (uint8_t)(i * 7u + 1u);
    }
    for (uint32_t len = 0; len <= sizeof data; len += 7) {
        sha256(data, len, want);
        for (uint32_t split = 0; split <= len; split += 13) {
            struct sha256 s;
            sha256_init(&s);
            sha256_update(&s, data, split);
            sha256_update(&s, data + split, len - split);
            sha256_final(&s, got);
            KTEST_CHECK(memcmp(got, want, SHA256_DIGEST) == 0);
        }
    }
}

static void test_byte_at_a_time(void) {
    struct sha256 s;
    uint8_t got[SHA256_DIGEST];
    char hex[65];
    sha256_init(&s);
    for (uint32_t i = 0; i < 1000; i++) {
        sha256_update(&s, "a", 1);
    }
    sha256_final(&s, got);
    sha256_format(got, hex);
    KTEST_CHECK_STR(hex, "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
}

static void test_equal_is_exact(void) {
    uint8_t a[SHA256_DIGEST] = {0}, b[SHA256_DIGEST] = {0};
    KTEST_CHECK(sha256_equal(a, b) == 1);
    b[31] = 1;
    KTEST_CHECK(sha256_equal(a, b) == 0);
    b[31] = 0;
    b[0] = 0x80;
    KTEST_CHECK(sha256_equal(a, b) == 0);
}

int main(void) {
    KTEST_RUN(test_published_vectors);
    KTEST_RUN(test_long_message);
    KTEST_RUN(test_streaming_matches_oneshot);
    KTEST_RUN(test_byte_at_a_time);
    KTEST_RUN(test_equal_is_exact);
    return ktest_failures();
}
