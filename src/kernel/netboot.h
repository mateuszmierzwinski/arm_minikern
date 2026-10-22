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

#include <stddef.h>
#include <stdint.h>

#include "core/net/net.h"

/* Waits for a DHCP lease, then runs a session. board_id identifies this
 * board to the server (the chip's unique id); pinned, when not 0, is the
 * only address probed. Returns KERR_OK with *image and *len (kernel
 * addresses, valid forever), or a negative errno; it logs its own progress
 * and the reason for a failure. */
int knetboot_fetch(const uint32_t board_id[4], ip4_t pinned, const uint8_t **image, uint32_t *len);

/* Accepts only a root filesystem with this digest, given as 64 hex
 * characters (`minikern.rootfs.sha256=`). Returns KERR_INVAL for anything
 * that is not exactly that, because a pin that silently did not parse would
 * be worse than no pin at all: the boot would look protected and not be.
 *
 * What it protects against is the one that matters — the first machine to
 * answer the sweep deciding what the board runs (core/net/netboot.h). */
int knetboot_expect(const char *hex, size_t len);

/* Accepts only offers signed by the holder of this public key, given as 64
 * hex characters (`minikern.netboot.key=`). KERR_INVAL for anything that is
 * not exactly that, for the same reason as the digest pin: a key that
 * silently did not parse would leave the boot looking protected without
 * being so.
 *
 * This is the one that authenticates the **server** rather than an image
 * (core/net/netboot.h), so the server can change what it serves. */
int knetboot_require_key(const char *hex, size_t len);

#endif
