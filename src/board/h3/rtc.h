/* rtc — the H3's real-time clock, so that a boot does not start in 1970.
 *
 * Unlike the thermal sensor and the PLLs, **QEMU models this one**
 * (`allwinner-rtc-sun6i`), so what the registers hold was measured rather
 * than assumed: the date register carries the day, the month and the year
 * **as an offset from 1970**, and the time register the seconds, minutes
 * and hours, with the weekday in its top bits. A boot under QEMU reads the
 * host's UTC time, which is what made the layout checkable.
 *
 * On the NEO the clock keeps time only while the board has power — there is
 * no battery on the board — so it is useful across a reboot and not across
 * a power cut. `minikern.epoch=` remains for when it reads as unset, and an
 * NTP client in Go remains the way to be right (P8.9).
 */
#ifndef MINIKERN_BOARD_H3_RTC_H
#define MINIKERN_BOARD_H3_RTC_H

#include <stdint.h>

/* The wall clock in seconds since 1970, or 0 when the clock does not hold a
 * real date — which is how an unset one reads, and must not be turned into
 * a plausible-looking lie. */
uint64_t h3_rtc_now(void);

#endif
