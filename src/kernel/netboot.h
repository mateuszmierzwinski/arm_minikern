/* netboot (kernel) — fetches the root filesystem over the network before
 * /init runs: the MKNB client (core/net/netboot.h) wired to the live
 * interface, with the image in a buffer taken from the page allocator and
 * kept for good (the archive is used in place).
 *
 * The boot waits for it by idling with interrupts open, so the timer and
 * the EMAC keep running; the state machine is therefore touched from the
 * boot loop and from interrupt context, which is safe here because the
 * kernel runs with IRQs masked outside the idle instruction (CLAUDE.md §2). */
#ifndef MINIKERN_KERNEL_NETBOOT_H
#define MINIKERN_KERNEL_NETBOOT_H

#include <stdint.h>

#include "core/net/net.h"

/* Waits for a DHCP lease, then runs a session. board_id identifies this
 * board to the server (the chip's unique id); pinned, when not 0, is the
 * only address probed. Returns KERR_OK with *image and *len (kernel
 * addresses, valid forever), or a negative errno; it logs its own progress
 * and the reason for a failure. */
int knetboot_fetch(const uint32_t board_id[4], ip4_t pinned, const uint8_t **image, uint32_t *len);

#endif
