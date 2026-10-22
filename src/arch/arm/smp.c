#include "arch/arm/smp.h"

#include "arch/arm/cpu.h"
#include "arch/arm/gic.h"
#include "arch/arm/layout.h"
#include "arch/arm/timer.h"
#include "arch/arm/trap.h"
#include "core/console.h"
#include "core/errno.h"
#include "kernel/sched.h"

/* PSCI function identifiers (PSCI 0.2, 32-bit calling convention). */
#define PSCI_CPU_ON      0x84000003u
#define PSCI_SUCCESS     0
#define PSCI_ALREADY_ON  (-4)

/* How long a core is given to report itself. A core that is not there
 * answers CPU_ON with an error immediately; one that is takes microseconds,
 * so this is only a bound on something going wrong. */
#define SMP_WAIT_SPINS 2000000u

/* Each secondary's stack, and the flag it sets once it is running. The
 * stacks are in .bss, which is RAM the pmm reserves, and 8 KiB is what a
 * kernel stack is everywhere else here. */
#define SMP_STACK_BYTES 8192u

static uint8_t stacks[SMP_MAX_CPUS][SMP_STACK_BYTES] __attribute__((aligned(8)));
static volatile uint32_t online_mask = 1u; /* this core, which is already running */
static volatile bool released;             /* the boot path has finished */

/* The bring-up translation table and its physical address with the walk
 * attributes, which _secondary_start loads before the MMU is on.
 *
 * It exists because the primary drops the identity map as soon as the low
 * half becomes user space, and a core entering with the MMU off has a
 * physical PC that nothing in the kernel's own table describes. */
static uint32_t smp_l1[4096] __attribute__((aligned(16384)));
uint32_t smp_l1_phys;

extern void _secondary_start(void);

uint32_t smp_cpu_id(void) {
    return arm_mpidr() & 0xffu;
}

uint32_t smp_online(void) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < SMP_MAX_CPUS; i++) {
        count += (online_mask >> i) & 1u;
    }
    return count;
}

bool smp_is_online(uint32_t cpu) {
    return cpu < SMP_MAX_CPUS && ((online_mask >> cpu) & 1u) != 0;
}

/* The top of a core's stack, called from the entry path with the core
 * number in r0. A number out of range would be a core this kernel does not
 * know about, which gets the last stack rather than a wild pointer. */
void *smp_stack_top(uint32_t cpu) {
    uint32_t at = cpu < SMP_MAX_CPUS ? cpu : SMP_MAX_CPUS - 1u;
    return &stacks[at][SMP_STACK_BYTES];
}

/* Builds the bring-up table: RAM at its physical address (so the
 * instruction after "MMU on" is still there) and RAM high (so the jump to
 * the kernel's own addresses lands), plus the I/O window. */
static void build_bringup_table(void) {
    uint32_t sections = RAM_MAP_SIZE >> SECTION_SHIFT;
    for (uint32_t i = 0; i < 4096u; i++) {
        smp_l1[i] = 0;
    }
    for (uint32_t i = 0; i < sections; i++) {
        uint32_t phys = RAM_PHYS_BASE + (i << SECTION_SHIFT);
        smp_l1[(RAM_PHYS_BASE >> SECTION_SHIFT) + i] = phys | SECT_KERNEL_RAM;
        smp_l1[(RAM_VIRT_BASE >> SECTION_SHIFT) + i] = phys | SECT_KERNEL_RAM;
    }
    for (uint32_t i = 0; i < (IO_MAP_SIZE >> SECTION_SHIFT); i++) {
        smp_l1[(IO_VIRT_BASE >> SECTION_SHIFT) + i] =
            (IO_PHYS_BASE + (i << SECTION_SHIFT)) | SECT_KERNEL_IO;
    }
    smp_l1_phys = virt_to_phys(smp_l1) | TTBR_WALK_ATTRS;
}

/* PSCI CPU_ON: start cpu at entry (a physical address). */
static int32_t psci_cpu_on(uint32_t cpu, uint32_t entry) {
    register uint32_t r0 __asm__("r0") = PSCI_CPU_ON;
    register uint32_t r1 __asm__("r1") = cpu;
    register uint32_t r2 __asm__("r2") = entry;
    register uint32_t r3 __asm__("r3") = 0; /* no context identifier */
    __asm__ volatile("smc #0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r3) : "memory");
    return (int32_t)r0;
}

/* Where a secondary arrives: running, with its own stack and the MMU on.
 *
 * It reports itself and then waits — touching nothing the kernel owns,
 * because the boot path is still in the kernel without the lock. Once let
 * in, it sets up the three things that are its own (a vector table, an
 * interrupt interface, a tick), adopts its idle context and schedules.
 * From there it is an ordinary core: it enters the kernel through
 * trap_dispatch like any other, under the one lock. */
void smp_secondary_main(void) {
    uint32_t cpu = smp_cpu_id();
    __atomic_or_fetch(&online_mask, 1u << (cpu & 31u), __ATOMIC_RELEASE);
    while (!released) {
        __asm__ volatile("wfe" ::: "memory");
    }
    /* Everything here is per core, and a core that skips any of it fails
     * in a way that is hard to see: no vector table means its first
     * exception goes to address zero, no interrupt interface or tick means
     * it never schedules, and **no VFP means the first floating-point
     * instruction faults** — which for a hard-float Go binary is
     * immediate, and which faulted inside the kernel while holding the
     * lock, so it deadlocked in silence rather than panicking. */
    trap_init_cpu();
    arm_vfp_enable();
    gic_init_cpu();
    timer_start_cpu();
    sched_init_cpu();
    sched_idle_loop();  /* never returns */
}

void smp_release(void) {
    released = true;
    __asm__ volatile("dsb\n\tsev" ::: "memory");
}

/* Waits for a core to report itself; false when it never does. */
static bool wait_for(uint32_t cpu) {
    for (uint32_t spin = 0; spin < SMP_WAIT_SPINS; spin++) {
        if (smp_is_online(cpu)) {
            return true;
        }
        __asm__ volatile("yield" ::: "memory");
    }
    return false;
}

uint32_t smp_start_secondaries(void) {
    uint32_t entry = virt_to_phys((const void *)_secondary_start);
    uint32_t self = smp_cpu_id();
    build_bringup_table();
    for (uint32_t cpu = 0; cpu < SMP_MAX_CPUS; cpu++) {
        int32_t rc;
        if (cpu == self) {
            continue;
        }
        rc = psci_cpu_on(cpu, entry);
        if (rc != PSCI_SUCCESS && rc != PSCI_ALREADY_ON) {
            kprintf("smp: cpu %u did not start (psci %d)\n", cpu, rc);
            continue;
        }
        if (!wait_for(cpu)) {
            kprintf("smp: cpu %u started but never reported itself\n", cpu);
        }
    }
    return smp_online();
}
