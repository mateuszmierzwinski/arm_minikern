/* vectors.S — exception entry and exit, the thread switch, VFP state.
 *
 * Every exception lands on the current SVC stack: srsdb pushes the return
 * address and SPSR there whatever mode took the exception, so IRQ, abort and
 * undefined modes need no stacks of their own. The frame is struct
 * trapframe (trap.h), lowest address first:
 *
 *   r0..r12, lr_svc, sp_usr, lr_usr, pc, spsr      (18 words, 72 bytes)
 *
 * The kernel runs with IRQs masked (every exception masks them on entry),
 * so a trap from SVC mode only happens in the idle loop or on a kernel bug. */
    .syntax unified
    .arch armv7-a
    .fpu neon-vfpv4

    .equ MODE_SVC, 0x13
    .equ TF_LR_SVC, 52
    .equ TF_SP_USR, 56
    .equ TF_REGS_SIZE, 64              @ everything below pc/spsr

/* trap_entry kind, adjust: lr -= adjust gives the return address. */
.macro trap_entry kind, adjust
    .if \adjust
    sub   lr, lr, #\adjust
    .endif
    srsdb sp!, #MODE_SVC
    cps   #MODE_SVC
    sub   sp, sp, #TF_REGS_SIZE
    stmia sp, {r0-r12}
    str   lr, [sp, #TF_LR_SVC]
    add   r0, sp, #TF_SP_USR
    stmia r0, {sp, lr}^                @ the user-mode banked sp and lr
    mov   r0, sp
    mov   r1, #\kind
    bl    trap_dispatch
    b     trap_exit
.endm

    .section .text.vectors, "ax"
    .balign 32
    .global arm_vectors
arm_vectors:
    b     vec_reset
    b     vec_und
    b     vec_svc
    b     vec_pabt
    b     vec_dabt
    b     vec_reset
    b     vec_irq
    b     vec_fiq

/* Kinds match enum trap_kind (trap.h). */
vec_reset: trap_entry 0, 0
vec_und:   trap_entry 1, 4             @ pc = the undefined instruction itself
vec_svc:   trap_entry 2, 0             @ lr = instruction after svc
vec_pabt:  trap_entry 3, 4             @ return re-executes the faulting fetch
vec_dabt:  trap_entry 4, 8             @ return re-executes the faulting access
vec_irq:   trap_entry 6, 4
vec_fiq:   trap_entry 7, 4

    .text
/* trap_exit: sp = a trapframe; restores it and returns through rfe. New
 * threads start here too (arm_thread_start). */
    .global trap_exit
trap_exit:
    add   r0, sp, #TF_SP_USR
    ldmia r0, {sp, lr}^
    nop                                @ no banked-register access right after ldm ^
    ldr   lr, [sp, #TF_LR_SVC]
    ldmia sp, {r0-r12}
    add   sp, sp, #TF_REGS_SIZE
    rfeia sp!

/* void arm_switch(uint32_t *save_sp, uint32_t new_sp): saves the callee-saved
 * state on the current kernel stack, stores sp, and resumes the other
 * thread's. 10 words keep the stack 8-byte aligned. */
    .global arm_switch
arm_switch:
    push  {r4-r12, lr}
    str   sp, [r0]
    mov   sp, r1
    pop   {r4-r12, lr}
    bx    lr

/* The first "return" of a new thread: its switch frame's lr points here and
 * its trapframe sits right above, so it leaves for user space. r4 holds a
 * hook run first (thread_first_run), which may be 0. */
    .global arm_thread_start
arm_thread_start:
    cmp   r4, #0
    blxne r4
    b     trap_exit

/* VFP state: d0-d31 then FPSCR, 260 bytes (struct vfp_state, cpu.h). */
    .global arm_vfp_save
arm_vfp_save:
    vstmia r0!, {d0-d15}
    vstmia r0!, {d16-d31}
    vmrs  r1, fpscr
    str   r1, [r0]
    bx    lr

    .global arm_vfp_restore
arm_vfp_restore:
    vldmia r0!, {d0-d15}
    vldmia r0!, {d16-d31}
    ldr   r1, [r0]
    vmsr  fpscr, r1
    bx    lr

/* Turns the VFP on for every mode: CPACR cp10/cp11 full access, FPEXC.EN. */
    .global arm_vfp_enable
arm_vfp_enable:
    mrc   p15, 0, r0, c1, c0, 2
    orr   r0, r0, #(0xf << 20)
    mcr   p15, 0, r0, c1, c0, 2
    isb
    mov   r0, #(1 << 30)
    vmsr  fpexc, r0
    bx    lr

    .section .note.GNU-stack, "", %progbits
