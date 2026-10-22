#include "arch/arm/psci.h"

#include <stdint.h>

#include "arch/arm/cpu.h"

#define PSCI_SYSTEM_OFF 0x84000008u  /* PSCI 0.2 function id, SMC32 */

static uint32_t psci_call(uint32_t function) {
    register uint32_t r0 __asm__("r0") = function;
    __asm__ volatile(".arch_extension sec\n\tsmc #0" : "+r"(r0) : : "r1", "r2", "r3", "memory");
    return r0;
}

static void psci_idle(void *self) {
    (void)self;
    arm_wfi();
}

static void psci_poweroff(void *self) {
    (void)self;
    (void)psci_call(PSCI_SYSTEM_OFF);
}

struct power_port psci_power_port(void) {
    return (struct power_port){.idle = psci_idle, .poweroff = psci_poweroff, .self = 0};
}
