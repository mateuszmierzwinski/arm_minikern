/* start.S — the first instructions. Entered from U-Boot's bootm (or QEMU's
 * -kernel) per the ARM Linux boot protocol: MMU and D-cache off, r0 = 0,
 * r1 = machine type, r2 = ATAG list or FDT (physical). QEMU enters a uImage
 * in HYP mode and an ELF in SVC (measured, CLAUDE.md §3).
 *
 * The code here runs at its physical address, before translation: it leaves
 * HYP for SVC, builds the boot translation table (layout.h), turns the MMU
 * and caches on, and jumps to the kernel's linked, high virtual address.
 * The table also maps RAM at its physical address, so the instructions
 * between "MMU on" and the jump keep running; that identity map is dropped
 * when TTBCR.N = 1 hands the low half to user space (mmu.c). */
#include "arch/arm/layout.h"

    .syntax unified
    .arch armv7-a
    .arch_extension virt

    .equ MODE_MASK, 0x1f
    .equ MODE_HYP,  0x1a
    .equ SVC_MASKED, 0xd3          @ SVC mode, IRQ and FIQ masked

/* map_sections virt, phys, count, attrs: fills boot_l1 entries, r0 = table. */
.macro map_sections virt, phys, count, attrs
    ldr   r1, =(\virt >> SECTION_SHIFT)
    ldr   r2, =(\phys >> SECTION_SHIFT)
    ldr   r3, =(\count >> SECTION_SHIFT)
    ldr   r8, =\attrs
1:  orr   r9, r8, r2, lsl #SECTION_SHIFT
    str   r9, [r0, r1, lsl #2]
    add   r1, r1, #1
    add   r2, r2, #1
    subs  r3, r3, #1
    bne   1b
.endm

    .section .text.boot, "ax"
    .global _start
_start:
    mov   r4, r0                   @ keep the boot registers in callee-saved regs
    mov   r5, r1
    mov   r6, r2
    mrs   r7, cpsr                 @ the entry mode, reported by kmain
    and   r3, r7, #MODE_MASK
    cmp   r3, #MODE_HYP
    bne   in_svc
    mrc   p15, 4, r3, c14, c1, 0   @ CNTHCTL: PL1 may read the counter and use the timers
    orr   r3, r3, #3
    mcr   p15, 4, r3, c14, c1, 0
    mov   r3, #0
    mcrr  p15, 4, r3, r3, c14      @ CNTVOFF = 0: virtual count = physical count
    mcr   p15, 4, r3, c1, c1, 2    @ HCPTR: trap no coprocessor (VFP must reach PL1/PL0)
    mcr   p15, 4, r3, c1, c1, 3    @ HSTR: trap no CP15 access
    mrs   r3, cpsr
    bic   r3, r3, #MODE_MASK
    orr   r3, r3, #SVC_MASKED
    msr   spsr_hyp, r3
    adr   r3, in_svc
    msr   elr_hyp, r3
    eret
in_svc:
    cpsid if
    bl    build_boot_table
    bl    enable_mmu
    ldr   pc, =high_start          @ from here on, linked (virtual) addresses

/* boot_l1: zeroed, then RAM twice (identity and high) plus the I/O window. */
build_boot_table:
    ldr   r0, =(boot_l1 - KERNEL_VIRT_OFFSET)
    mov   r1, #0
    mov   r2, #4096
2:  subs  r2, r2, #1
    str   r1, [r0, r2, lsl #2]
    bne   2b
    map_sections RAM_PHYS_BASE, RAM_PHYS_BASE, RAM_MAP_SIZE, SECT_KERNEL_RAM
    map_sections RAM_VIRT_BASE, RAM_PHYS_BASE, RAM_MAP_SIZE, SECT_KERNEL_RAM
    map_sections IO_VIRT_BASE, IO_PHYS_BASE, IO_MAP_SIZE, SECT_KERNEL_IO
    bx    lr

/* One table for both halves until mmu_init; domain 0 is "client" (checked).
 * ACTLR.SMP must be set on a Cortex-A7 before its caches work; firmware
 * usually has, and a write it forbids is ignored. */
enable_mmu:
    mov   r0, #0
    mcr   p15, 0, r0, c8, c7, 0    @ TLBIALL
    mcr   p15, 0, r0, c7, c5, 0    @ ICIALLU
    mcr   p15, 0, r0, c7, c5, 6    @ BPIALL
    dsb
    isb
    mcr   p15, 0, r0, c2, c0, 2    @ TTBCR = 0: TTBR0 translates everything
    ldr   r1, =(boot_l1 - KERNEL_VIRT_OFFSET + TTBR_WALK_ATTRS)
    mcr   p15, 0, r1, c2, c0, 0    @ TTBR0
    mcr   p15, 0, r1, c2, c0, 1    @ TTBR1
    mov   r1, #1
    mcr   p15, 0, r1, c3, c0, 0    @ DACR: domain 0 client
    mrc   p15, 0, r1, c1, c0, 1
    orr   r1, r1, #(1 << 6)        @ ACTLR.SMP
    mcr   p15, 0, r1, c1, c0, 1
    mrc   p15, 0, r1, c1, c0, 0    @ SCTLR
    ldr   r2, =((1 << 1) | (1 << 13) | (1 << 28) | (1 << 29))  @ A, V, TRE, AFE off
    bic   r1, r1, r2
    ldr   r2, =((1 << 0) | (1 << 2) | (1 << 11) | (1 << 12))   @ M, C, Z, I on
    orr   r1, r1, r2
    dsb
    mcr   p15, 0, r1, c1, c0, 0
    isb
    bx    lr

    .text
high_start:
    ldr   sp, =__stack_top
    ldr   r0, =__bss_start
    ldr   r1, =__bss_end
    mov   r2, #0
zero_bss:
    cmp   r0, r1
    strlo r2, [r0], #4
    blo   zero_bss
    mov   r0, r4
    mov   r1, r5
    mov   r2, r6
    mov   r3, r7
    bl    kmain                    @ kmain(r0, machine, bootinfo_phys, entry_cpsr)
halt:
    wfi
    b     halt

/* The kernel's translation table: TTBR1 for life. Outside .bss on purpose,
 * because zero_bss runs while it is live. 16 KiB, 16 KiB aligned. */
    .section .pgtable, "aw", %nobits
    .balign 16384
    .global boot_l1
boot_l1:
    .space 16384

    .section .note.GNU-stack, "", %progbits
