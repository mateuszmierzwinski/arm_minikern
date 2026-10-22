/* emac — the H3's on-chip Ethernet MAC ("sun8i EMAC") with its internal
 * 10/100 PHY, as a netdev_port. Register facts: the H3 datasheet's EMAC
 * chapter, cross-checked against QEMU's model (CLAUDE.md §3); the clock,
 * reset and PHY-select bits are the datasheet's and are unverified on
 * silicon until TASKS.md P0.9. */
#ifndef MINIKERN_BOARD_H3_EMAC_H
#define MINIKERN_BOARD_H3_EMAC_H

#include <stdint.h>

#include "core/ports.h"

/* What the PHY said when the link came up. */
struct emac_link {
    uint32_t phy_id;  /* ID1 << 16 | ID2 */
    uint32_t phy_addr;
    int up;
    int speed_100;
    int full_duplex;
};

/* Powers the MAC and PHY, resets them, builds the DMA rings, sets mac and
 * brings the link up; received frames go to sink from the EMAC interrupt.
 * Returns KERR_OK (filling *link), KERR_NOMEM, KERR_IO (reset or PHY did
 * not answer) or KERR_TIMEDOUT (no link). */
int h3_emac_init(const uint8_t mac[6], const struct frame_sink *sink, struct emac_link *link);

/* The transmit side as a netdev_port (valid after h3_emac_init). */
struct netdev_port h3_emac_netdev(void);

/* A stable, locally administered MAC address derived from the chip ID. */
void h3_emac_mac_from_id(const uint32_t id[4], uint8_t mac[6]);

#endif
