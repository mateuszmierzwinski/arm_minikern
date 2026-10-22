#include "arch/arm/smp.h"

#include "arch/arm/cpu.h"
#include "arch/arm/gic.h"
#include "arch/arm/layout.h"
#include "arch/arm/mmu.h"
#include "arch/arm/timer.h"
#include "arch/arm/trap.h"
#include "core/console.h"
#include "core/errno.h"
#include "kernel/proc.h"
#include "kernel/sched.h"

/* PSCI function identifiers (PSCI 0.2, 32-bit calling convention). */
#define PSCI_CPU_ON      0x84000003u
#define PSCI_CPU_OFF     0x84000002u
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
static volatile uint32_t parked_mask;      /* cores that powered themselves off */
static volatile uint32_t wake_mask;        /* parked cores a waker wants back */
static bool parking_allowed;               /* minikern.cpuoff=1 */
static uint32_t parks;                     /* how many times a core went off */

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
    uint32_t bit = 1u << (cpu & 31u);
    __atomic_or_fetch(&online_mask, bit, __ATOMIC_RELEASE);
    /* A core that was parked and has been started again arrives here, so
     * these are cleared on the way in rather than by the waker: the waker
     * cannot know when the core actually came back. */
    __atomic_and_fetch(&parked_mask, ~bit, __ATOMIC_RELEASE);
    __atomic_and_fetch(&wake_mask, ~bit, __ATOMIC_RELEASE);
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
    mmu_init_cpu();
    /* It has just set its own translation registers from nothing, so any
     * record of what this core had installed before is stale (proc.h). A
     * core that has been parked and started again is the case that needs
     * it; at boot there is nothing to forget. */
    proc_forget_installed_mm();
    trap_init_cpu();
    arm_vfp_enable();
    gic_init_cpu();
    timer_start_cpu();
    sched_init_cpu();
    sched_idle_loop();  /* never returns */
}

/* PSCI CPU_OFF: this core powers itself down. It does not return when it
 * succeeds, and the core is then started again with CPU_ON — re-entering
 * at _secondary_start, which is why the whole per-core setup in
 * smp_secondary_main has to be repeatable. */
static int32_t psci_cpu_off(void) {
    register uint32_t r0 __asm__("r0") = PSCI_CPU_OFF;
    __asm__ volatile("smc #0" : "+r"(r0) :: "memory");
    return (int32_t)r0;
}

void smp_allow_parking(bool allow) {
    parking_allowed = allow;
}

bool smp_may_park(void) {
    /* The core that booted never parks. Something has to keep ticking to
     * retry a wake that raced a power-off, and that is the invariant which
     * makes a lost wake cost a millisecond instead of wedging the board. */
    return parking_allowed && released && smp_cpu_id() != 0u;
}

void smp_park_self(void) {
    uint32_t cpu = smp_cpu_id();
    uint32_t bit = 1u << (cpu & 31u);
    /* `online_mask` is deliberately left alone. A parked core has not gone
     * away — it comes back the moment there is work — and `smp_scheduling`
     * is what user space is told when it asks how many CPUs it has. Taking
     * the bit out made Go size GOMAXPROCS to 1 and then build a workload
     * that never needed the other cores, so they stayed off and the whole
     * run used one core: a power optimisation that removed the
     * parallelism it was meant to be saving power on. */
    __atomic_or_fetch(&parked_mask, bit, __ATOMIC_RELEASE);
    parks++;
    /* A waker may have asked for this core back between the decision and
     * here. Taking the ask now rather than powering off is the cheap half
     * of the race; the expensive half — a wake that arrives after this
     * point — is covered by the retry from the tick. */
    if ((__atomic_load_n(&wake_mask, __ATOMIC_ACQUIRE) & bit) != 0) {
        __atomic_and_fetch(&wake_mask, ~bit, __ATOMIC_RELEASE);
        __atomic_and_fetch(&parked_mask, ~bit, __ATOMIC_RELEASE);
        return;
    }
    (void)psci_cpu_off();
    /* Only reached if the firmware refused, in which case this core simply
     * carries on idling — which is what it was doing. */
    __atomic_and_fetch(&parked_mask, ~bit, __ATOMIC_RELEASE);
}

bool smp_any_parked(void) {
    return __atomic_load_n(&parked_mask, __ATOMIC_ACQUIRE) != 0;
}

void smp_wake_parked(void) {
    uint32_t still = __atomic_load_n(&parked_mask, __ATOMIC_ACQUIRE);
    for (uint32_t cpu = 1; cpu < SMP_MAX_CPUS; cpu++) {
        uint32_t bit = 1u << cpu;
        if ((still & bit) == 0) {
            continue;
        }
        /* Asked for before the call, so a core still on its way down sees
         * the ask and stays. */
        __atomic_or_fetch(&wake_mask, bit, __ATOMIC_RELEASE);
        /* The entry is a **physical** address: the core comes back with
         * its MMU off, exactly as it did at boot. Passing the kernel's
         * virtual address instead started nothing — PSCI refused it, the
         * core stayed parked, and the test still passed because every core
         * had one kernel entry from before it ever parked. */
        if (psci_cpu_on(cpu, virt_to_phys((const void *)_secondary_start)) != PSCI_SUCCESS) {
            continue; /* ALREADY_ON: still going down, so the tick retries */
        }
        __atomic_and_fetch(&wake_mask, ~bit, __ATOMIC_RELEASE);
        __atomic_and_fetch(&parked_mask, ~bit, __ATOMIC_RELEASE);
    }
}

uint32_t smp_parks(void) {
    return parks;
}

uint32_t smp_scheduling(void) {
    return released ? smp_online() : 1u;
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
