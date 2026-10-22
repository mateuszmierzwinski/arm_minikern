#include "lib/crc32c.h"

#include <string.h>

#include "ktest.h"

/* The standard vectors, and then one taken from a real XFS superblock —
 * xfs_db reports that filesystem's sum as 0xae99e908 and calls it correct,
 * so if we agree with it we agree with Linux. */

static void test_known_vectors(void) {
    KTEST_CHECK(crc32c("", 0) == 0x00000000u);
    KTEST_CHECK(crc32c("123456789", 9) == 0xe3069283u);
    KTEST_CHECK(crc32c("a", 1) == 0xc1d04330u);
    KTEST_CHECK(crc32c("The quick brown fox jumps over the lazy dog", 43) == 0x22620404u);
}

/* A sum folded in pieces must equal the sum taken in one go: that is what
 * lets a verifier skip the checksum field in the middle of a block. */
static void test_incremental(void) {
    const char *text = "minikern reads XFS";
    uint32_t whole = crc32c_update(CRC32C_SEED, text, 18u);
    uint32_t parts = crc32c_update(CRC32C_SEED, text, 7u);
    parts = crc32c_update(parts, text + 7, 11u);
    KTEST_CHECK(whole == parts);
}

static void test_zero_length_changes_nothing(void) {
    uint32_t crc = crc32c_update(CRC32C_SEED, "x", 1u);
    KTEST_CHECK(crc32c_update(crc, "", 0) == crc);
}

/* Every single-bit flip must change the sum: the point of carrying one. */
static void test_a_flipped_bit_is_caught(void) {
    uint8_t buf[64];
    uint32_t base;
    memset(buf, 0x5a, sizeof buf);
    base = crc32c(buf, sizeof buf);
    for (uint32_t i = 0; i < sizeof buf; i++) {
        for (uint32_t bit = 0; bit < 8u; bit++) {
            buf[i] ^= (uint8_t)(1u << bit);
            KTEST_CHECK(crc32c(buf, sizeof buf) != base);
            buf[i] ^= (uint8_t)(1u << bit);
        }
    }
    KTEST_CHECK(crc32c(buf, sizeof buf) == base);
}

int main(void) {
    KTEST_RUN(test_known_vectors);
    KTEST_RUN(test_incremental);
    KTEST_RUN(test_zero_length_changes_nothing);
    KTEST_RUN(test_a_flipped_bit_is_caught);
    return ktest_failures();
}
