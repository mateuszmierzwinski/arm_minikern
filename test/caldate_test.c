/* The calendar arithmetic, against dates whose unix times are known — and
 * chosen where a nearly-right implementation fails: the epoch itself, the
 * end of a leap year, the 29th of February, and 2100, which is not a leap
 * year however much it looks like one. */
#include "core/caldate.h"

#include "ktest.h"

static void known_instants(void) {
    KTEST_CHECK(caldate_to_unix(1970, 1, 1, 0, 0, 0) == 0);
    KTEST_CHECK(caldate_to_unix(1970, 1, 1, 0, 0, 1) == 1);
    KTEST_CHECK(caldate_to_unix(1970, 1, 2, 0, 0, 0) == 86400);
    KTEST_CHECK(caldate_to_unix(2000, 1, 1, 0, 0, 0) == 946684800ull);
    KTEST_CHECK(caldate_to_unix(2001, 9, 9, 1, 46, 40) == 1000000000ull);
    KTEST_CHECK(caldate_to_unix(2026, 10, 22, 11, 5, 27) == 1792667127ull);
    KTEST_CHECK(caldate_to_unix(2038, 1, 19, 3, 14, 7) == 2147483647ull);
    /* Past what a 32-bit time can hold, because this returns 64 bits. */
    KTEST_CHECK(caldate_to_unix(2106, 2, 7, 6, 28, 16) == 4294967296ull);
}

static void leap_years(void) {
    KTEST_CHECK(caldate_is_leap(2024));
    KTEST_CHECK(caldate_is_leap(2000));
    KTEST_CHECK(!caldate_is_leap(2023));
    KTEST_CHECK(!caldate_is_leap(1900));
    KTEST_CHECK(!caldate_is_leap(2100)); /* the clause people leave out */
    /* The 29th of February exists in 2024 and does not in 2023. */
    KTEST_CHECK(caldate_to_unix(2024, 2, 29, 0, 0, 0) == 1709164800ull);
    KTEST_CHECK(caldate_to_unix(2023, 2, 29, 0, 0, 0) == 0);
    /* And a day on either side of a leap day is one day apart. */
    KTEST_CHECK(caldate_to_unix(2024, 3, 1, 0, 0, 0) - caldate_to_unix(2024, 2, 29, 0, 0, 0) == 86400);
}

static void every_month_has_its_own_length(void) {
    static const uint32_t LAST[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    for (uint32_t m = 1; m <= 12u; m++) {
        KTEST_CHECK(caldate_to_unix(2023, m, LAST[m - 1], 0, 0, 0) != 0);
        KTEST_CHECK(caldate_to_unix(2023, m, LAST[m - 1] + 1u, 0, 0, 0) == 0);
    }
}

/* Nonsense is 0 rather than a nearby instant: an unset clock reads as
 * zeroes, and a driver that turned that into 1969 or 1972 would set the
 * machine's time to a plausible-looking lie. */
static void nonsense_is_refused(void) {
    KTEST_CHECK(caldate_to_unix(0, 0, 0, 0, 0, 0) == 0);
    KTEST_CHECK(caldate_to_unix(1969, 12, 31, 23, 59, 59) == 0);
    KTEST_CHECK(caldate_to_unix(2026, 13, 1, 0, 0, 0) == 0);
    KTEST_CHECK(caldate_to_unix(2026, 0, 1, 0, 0, 0) == 0);
    KTEST_CHECK(caldate_to_unix(2026, 1, 0, 0, 0, 0) == 0);
    KTEST_CHECK(caldate_to_unix(2026, 1, 32, 0, 0, 0) == 0);
    KTEST_CHECK(caldate_to_unix(2026, 1, 1, 24, 0, 0) == 0);
    KTEST_CHECK(caldate_to_unix(2026, 1, 1, 0, 60, 0) == 0);
    KTEST_CHECK(caldate_to_unix(2026, 1, 1, 0, 0, 60) == 0);
}

/* Every day of a long stretch is exactly 86400 seconds after the one
 * before: one loop that would catch a month or a leap year being wrong
 * anywhere in it. */
static void consecutive_days_are_a_day_apart(void) {
    uint64_t previous = caldate_to_unix(1999, 1, 1, 0, 0, 0);
    for (uint32_t year = 1999; year <= 2101u; year++) {
        for (uint32_t month = 1; month <= 12u; month++) {
            for (uint32_t day = 1; day <= 31u; day++) {
                uint64_t at = caldate_to_unix(year, month, day, 0, 0, 0);
                if (at == 0) {
                    continue; /* a day that month does not have */
                }
                if (!(year == 1999 && month == 1 && day == 1)) {
                    KTEST_CHECK(at == previous + 86400ull);
                }
                previous = at;
            }
        }
    }
}

int main(void) {
    KTEST_RUN(known_instants);
    KTEST_RUN(leap_years);
    KTEST_RUN(every_month_has_its_own_length);
    KTEST_RUN(nonsense_is_refused);
    KTEST_RUN(consecutive_days_are_a_day_apart);
    return ktest_failures();
}
