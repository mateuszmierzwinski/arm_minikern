/* hello.S — the smallest Linux ARM EABI program: write(1, msg, len), then
 * exit_group(42). It proves user mode, the syscall path and the start state
 * before any Go runtime is involved. It also checks the start state: argc
 * must be 1 and argv[0] must be non-null, or it exits 1. */
    .syntax unified
    .arch armv7-a
    .text
    .global _start
_start:
    ldr   r0, [sp]                 @ argc
    cmp   r0, #1
    bne   bad
    ldr   r0, [sp, #4]             @ argv[0]
    cmp   r0, #0
    beq   bad
    mov   r0, #1
    adr   r1, msg
    mov   r2, #(msg_end - msg)
    mov   r7, #4                   @ write
    svc   #0
    mov   r0, #42
    mov   r7, #248                 @ exit_group
    svc   #0
bad:
    mov   r0, #1
    mov   r7, #248
    svc   #0
msg:
    .ascii "hello from user space\n"
msg_end:

    .section .note.GNU-stack, "", %progbits
