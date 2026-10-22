#include "core/tar.h"

#include <stdio.h>
#include <stdlib.h>

#include "core/errno.h"
#include "ktest.h"

static uint8_t ustar[16384], pax[16384];
static size_t ustar_len, pax_len;

static size_t load(const char *path, uint8_t *buf, size_t cap) {
    FILE *f = fopen(path, "rb");
    size_t n;
    if (f == NULL) {
        fprintf(stderr, "cannot open %s\n", path);
        exit(2);
    }
    n = fread(buf, 1, cap, f);
    fclose(f);
    return n;
}

static void test_reads_gnu_ustar(void) {
    struct tar_entry e;
    size_t off = 0;
    KTEST_CHECK(tar_next(ustar, ustar_len, &off, &e) == KERR_OK);
    KTEST_CHECK_STR(e.name, "init");
    KTEST_CHECK(e.type == TAR_FILE && e.size == 3 && memcmp(e.data, "abc", 3) == 0);
    KTEST_CHECK(tar_next(ustar, ustar_len, &off, &e) == KERR_OK);
    KTEST_CHECK_STR(e.name, "etc");
    KTEST_CHECK(e.type == TAR_DIR);
    KTEST_CHECK(tar_lookup(ustar, ustar_len, "/etc/motd", &e) == KERR_OK);
    KTEST_CHECK(e.size == 3 && memcmp(e.data, "hi\n", 3) == 0);
    KTEST_CHECK(tar_lookup(ustar, ustar_len, "./init", &e) == KERR_OK);
    KTEST_CHECK(tar_lookup(ustar, ustar_len, "/nope", &e) == KERR_NOENT);
    KTEST_CHECK(tar_lookup(ustar, ustar_len, "/etc/", &e) == KERR_OK && e.type == TAR_DIR);
}

static void test_skips_pax_records(void) {
    struct tar_entry e;
    KTEST_CHECK(tar_lookup(pax, pax_len, "/init", &e) == KERR_OK);
    KTEST_CHECK(e.type == TAR_FILE && e.size == 3 && memcmp(e.data, "abc", 3) == 0);
}

static void test_bad_checksum_rejected(void) {
    uint8_t copy[16384];
    struct tar_entry e;
    size_t off = 0;
    memcpy(copy, ustar, ustar_len);
    copy[5] ^= 1;  /* flip a name byte: checksum no longer matches */
    KTEST_CHECK(tar_next(copy, ustar_len, &off, &e) == KERR_INVAL);
}

/* Fixes up a header's checksum after a deliberate edit. */
static void rechecksum(uint8_t *h) {
    uint32_t sum = 0;
    memset(h + 148, ' ', 8);
    for (int i = 0; i < 512; i++) {
        sum += h[i];
    }
    snprintf((char *)h + 148, 8, "%06o", sum);
}

static void test_size_past_end_rejected(void) {
    uint8_t copy[16384];
    struct tar_entry e;
    size_t off = 0;
    memcpy(copy, ustar, ustar_len);
    memcpy(copy + 124, "77777777777", 11);  /* ~8 GiB */
    rechecksum(copy);
    KTEST_CHECK(tar_next(copy, ustar_len, &off, &e) == KERR_INVAL);
    memcpy(copy + 124, "00000100000", 11);  /* 32 KiB: past the 10 KiB archive */
    rechecksum(copy);
    off = 0;
    KTEST_CHECK(tar_next(copy, ustar_len, &off, &e) == KERR_INVAL);
    memcpy(copy + 124, "0000000003x", 11);  /* not octal */
    rechecksum(copy);
    off = 0;
    KTEST_CHECK(tar_next(copy, ustar_len, &off, &e) == KERR_INVAL);
}

static void test_truncated_archive(void) {
    struct tar_entry e;
    size_t off = 0;
    KTEST_CHECK(tar_next(ustar, 600, &off, &e) == KERR_INVAL);  /* header, no full data block */
    off = 0;
    KTEST_CHECK(tar_next(ustar, 100, &off, &e) == KERR_NOENT);
    off = 99999;
    KTEST_CHECK(tar_next(ustar, ustar_len, &off, &e) == KERR_NOENT);
    KTEST_CHECK(tar_lookup(NULL, 0, "/init", &e) == KERR_NOENT);
}

static void test_normalise(void) {
    char out[TAR_NAME_MAX], longp[400];
    KTEST_CHECK(tar_normalise("/./a/b/", out) == KERR_OK);
    KTEST_CHECK_STR(out, "a/b");
    KTEST_CHECK(tar_normalise("/", out) == KERR_OK);
    KTEST_CHECK_STR(out, "");
    memset(longp, 'a', sizeof longp - 1);
    longp[sizeof longp - 1] = '\0';
    KTEST_CHECK(tar_normalise(longp, out) == KERR_NAMETOOLONG);
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: tar_test ustar.tar pax.tar\n");
        return 2;
    }
    ustar_len = load(argv[1], ustar, sizeof ustar);
    pax_len = load(argv[2], pax, sizeof pax);
    KTEST_RUN(test_reads_gnu_ustar);
    KTEST_RUN(test_skips_pax_records);
    KTEST_RUN(test_bad_checksum_rejected);
    KTEST_RUN(test_size_past_end_rejected);
    KTEST_RUN(test_truncated_archive);
    KTEST_RUN(test_normalise);
    return ktest_failures();
}
