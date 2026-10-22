/* h3.h — the Allwinner H3 memory map as minikern uses it (H3 datasheet
 * v1.2 §3.1; cross-checked against QEMU hw/arm/allwinner-h3.c). Only what is
 * driven so far is named here; TASKS.md adds the rest as it is used. */
#ifndef MINIKERN_BOARD_H3_H
#define MINIKERN_BOARD_H3_H

#define H3_DRAM_BASE   0x40000000u
#define NANOPI_NEO_DRAM_SIZE 0x20000000u  /* 512 MiB: the NEO 512 */

#define H3_SYSCON_BASE 0x01C00000u  /* system control: EMAC_EPHY_CLK_REG at +0x30 */
#define H3_CCU_BASE    0x01C20000u  /* clock control: bus gates and resets */
#define H3_UART0_BASE  0x01C28000u  /* 16550-compatible, registers 4 bytes apart */
#define H3_EMAC_BASE   0x01C30000u  /* sun8i EMAC — P1 */
#define H3_SID_BASE    0x01C14000u  /* security ID: the chip's unique ID via PRCTL/RDKEY */
#define H3_GIC_DIST    0x01C81000u  /* GIC-400 distributor */
#define H3_GIC_CPU     0x01C82000u  /* GIC-400 CPU interface */

#define H3_IRQ_UART0   32u          /* SPI 0 */
#define H3_IRQ_EMAC    114u         /* SPI 82 */

#endif
