/* net (kernel) — the one network interface, wired up: frames from the
 * device's interrupt into the stack (core/net), the stack's 10 ms tick from
 * the timer, address configuration by DHCP at boot, a ping of the gateway
 * to prove the path, and the RFC 862 UDP echo service on port 7. */
#ifndef MINIKERN_KERNEL_NET_H
#define MINIKERN_KERNEL_NET_H

#include <stdint.h>

#include "core/net/net.h"
#include "core/ports.h"

#define NET_ECHO_PORT 7u

/* Creates the interface over dev with mac. Returns the sink the device
 * driver must deliver received frames to. */
struct frame_sink knet_init(const uint8_t mac[ETH_ALEN], const struct netdev_port *dev);

/* Starts DHCP and the tick (once the device is up). */
void knet_start(uint32_t xid);

/* Waits, with interrupts open, until DHCP has given the interface an
 * address. KERR_OK, or KERR_TIMEDOUT after timeout_ms.
 *
 * Anything that dials out at boot must call this first: the network comes
 * up in the background, so without it the first SYN leaves with no source
 * address and is never answered (CLAUDE.md §10). */
int knet_wait_address(uint32_t timeout_ms);

/* The interface, for later layers (sockets). */
struct netif *knet_interface(void);

/* The TCP stack running on it. */
struct tcp_stack *knet_tcp(void);

/* Prints the interface and TCP counters (minikern.netstat=1). */
void knet_report(void);

#endif
