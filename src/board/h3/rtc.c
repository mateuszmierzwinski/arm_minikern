#include "board/h3/rtc.h"

#include "arch/arm/layout.h"
#include "core/caldate.h"

/* The sun6i real-time clock. Measured against the host's clock under QEMU
 * (rtc.h), which is the only reason the field positions are not guesses. */
#define H3_RTC_BASE 0x01F00000u
#define RTC_DATE    0x10u
#define RTC_TIME    0x14u

/* The date register: day in bits 0-4, month in 8-11, year in 16-21 counted
 * from 1970. */
#define DATE_DAY_SHIFT   0u
#define DATE_DAY_MASK    0x1fu
#define DATE_MONTH_SHIFT 8u
#define DATE_MONTH_MASK  0x0fu
#define DATE_YEAR_SHIFT  16u
#define DATE_YEAR_MASK   0x3fu
#define RTC_YEAR_BASE    1970u

/* The time register: seconds in 0-5, minutes in 8-13, hours in 16-20. */
#define TIME_SEC_SHIFT  0u
#define TIME_SEC_MASK   0x3fu
#define TIME_MIN_SHIFT  8u
#define TIME_MIN_MASK   0x3fu
#define TIME_HOUR_SHIFT 16u
#define TIME_HOUR_MASK  0x1fu

static uint32_t rtc_read(uint32_t off) {
    return *(volatile uint32_t *)io_virt(H3_RTC_BASE + off);
}

uint64_t h3_rtc_now(void) {
    /* Both registers, then the date again: a read that straddles midnight
     * would otherwise pair a new day with an old time. If the date moved,
     * the pair is taken again — once is enough, since it cannot move twice
     * in the time this takes. */
    uint32_t date = rtc_read(RTC_DATE);
    uint32_t time = rtc_read(RTC_TIME);
    if (rtc_read(RTC_DATE) != date) {
        date = rtc_read(RTC_DATE);
        time = rtc_read(RTC_TIME);
    }
    return caldate_to_unix(RTC_YEAR_BASE + ((date >> DATE_YEAR_SHIFT) & DATE_YEAR_MASK),
                           (date >> DATE_MONTH_SHIFT) & DATE_MONTH_MASK,
                           (date >> DATE_DAY_SHIFT) & DATE_DAY_MASK,
                           (time >> TIME_HOUR_SHIFT) & TIME_HOUR_MASK,
                           (time >> TIME_MIN_SHIFT) & TIME_MIN_MASK,
                           (time >> TIME_SEC_SHIFT) & TIME_SEC_MASK);
}
