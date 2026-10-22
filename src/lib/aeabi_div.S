/* aeabi_div.S — the EABI entry points for 64-bit division (RTABI §4.3.1):
 * numerator in r0:r1, denominator in r2:r3; quotient back in r0:r1 and
 * remainder in r2:r3. Each forwards to div64_u/div64_s in C, passing a
 * stack slot for the remainder as the third argument. */
    .syntax unified
    .arch armv7-a
    .text

.macro divmod name, target
    .global \name
    .type \name, %function
\name:
    push  {r4, lr}
    sub   sp, sp, #16             @ [sp]: 3rd argument, [sp+8]: the remainder
    add   r4, sp, #8
    str   r4, [sp]
    bl    \target
    ldr   r2, [sp, #8]
    ldr   r3, [sp, #12]
    add   sp, sp, #16
    pop   {r4, pc}
.endm

divmod __aeabi_uldivmod, div64_u
divmod __aeabi_ldivmod, div64_s

    .section .note.GNU-stack, "", %progbits
