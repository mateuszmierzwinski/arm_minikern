/* psci — the ARM Power State Coordination Interface, over SMC. Mainline
 * U-Boot provides it on sun8i (it is how Linux starts the other three
 * cores); QEMU's orangepi-pc emulates it (measured: SYSTEM_OFF ends QEMU). */
#ifndef MINIKERN_ARCH_ARM_PSCI_H
#define MINIKERN_ARCH_ARM_PSCI_H

#include "core/ports.h"

/* A power_port over PSCI: idle is WFI, poweroff is PSCI SYSTEM_OFF. */
struct power_port psci_power_port(void);

#endif
