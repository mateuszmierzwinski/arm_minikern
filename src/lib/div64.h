/* div64 — 64-bit division for a 32-bit kernel. The ARM EABI says the
 * compiler calls __aeabi_uldivmod / __aeabi_ldivmod for it; the toolchain's
 * libgcc is built hard-float and cannot be linked into this soft-float
 * kernel, so the routines are ours (shift-subtract, written here). */
#ifndef MINIKERN_LIB_DIV64_H
#define MINIKERN_LIB_DIV64_H

#include <stdint.h>

/* n / d with the remainder in *rem. d == 0 yields 0 (and rem = n) rather
 * than a trap: the kernel never divides by a value it has not checked. */
uint64_t div64_u(uint64_t n, uint64_t d, uint64_t *rem);

/* Signed, truncating toward zero as C does; the remainder takes n's sign. */
int64_t div64_s(int64_t n, int64_t d, int64_t *rem);

#endif
