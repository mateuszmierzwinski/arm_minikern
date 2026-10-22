/* hwreport — one boot that prints every fact this kernel took from the
 * datasheet and has never checked on silicon.
 *
 * It exists because of the shape of P0.9. Everything that an emulator can
 * judge has been judged: QEMU models the clock block, so the PLL path is
 * driven and read back; it models the watchdog, the RTC and the chip ID, so
 * those are tested. It models the system-control block and the PIO as
 * *stubs* — writes vanish, reads give zero — and the thermal sensor not at
 * all. So three things can only ever be settled by powering the board up:
 * the EMAC's clock/reset/PHY-select bits, the thermal sensor's constants,
 * and the pin that switches VDD_CPUX.
 *
 * Rather than leave that as "try it on hardware", this prints each one
 * **beside what the datasheet says it should be**, so a mismatch is
 * obvious to someone reading a serial log who has never seen this code.
 * It is one command for the operator and a single artefact to send back.
 *
 * Every line begins `hw:` so the whole report is one `grep`, and each line
 * that is a claim rather than a measurement says so.
 */
#ifndef MINIKERN_BOARD_H3_HWREPORT_H
#define MINIKERN_BOARD_H3_HWREPORT_H

#include <stdint.h>

/* Prints the report.
 *
 * It deliberately does not try to say whether it is running on silicon or
 * on an emulator. The obvious signal is the timer frequency, and that one
 * is unreliable in exactly the case that matters: booted through U-Boot,
 * QEMU reports the same 24 MHz a board does. So each line names what the
 * datasheet claims and what QEMU gives, and the reader — who has the board
 * in their hands — is the one who knows which they are looking at. */
void h3_hwreport(void);

#endif
