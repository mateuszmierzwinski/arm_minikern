#include "arch/arm/cpu.h"

const char *arm_mode_name(uint32_t cpsr) {
    switch (cpsr & ARM_MODE_MASK) {
    case 0x10: return "usr";
    case 0x11: return "fiq";
    case 0x12: return "irq";
    case 0x13: return "svc";
    case 0x16: return "mon";
    case 0x17: return "abt";
    case 0x1a: return "hyp";
    case 0x1b: return "und";
    case 0x1f: return "sys";
    default: return "?";
    }
}
