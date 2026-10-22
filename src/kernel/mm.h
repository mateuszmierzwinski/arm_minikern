/* mm — one process's user address space: its translation table plus the
 * region set (core/vma.h). Anonymous memory is lazy: a region costs
 * nothing until touched, and the first touch of each page faults in a
 * zeroed frame (CLAUDE.md §7: TinyGo's 1 GiB heap mmap needs exactly this).
 *
 * The kernel never dereferences a user pointer. mm_copy_in/out check the
 * region rights, fault the pages in, and copy through the kernel's linear
 * map of RAM, so a bad pointer from user space is an -EFAULT, not a kernel
 * data abort. */
#ifndef MINIKERN_KERNEL_MM_H
#define MINIKERN_KERNEL_MM_H

#include <stdbool.h>
#include <stdint.h>

#include "arch/arm/pgtable.h"
#include "core/vma.h"

#define MM_MMAP_LO      0x00400000u  /* mmap never goes below 4 MiB */
#define MM_STACK_TOP    0x7fff0000u
#define MM_STACK_SIZE   0x00800000u  /* 8 MiB, RLIMIT_STACK */
#define MM_MMAP_TOP     (MM_STACK_TOP - MM_STACK_SIZE - 0x00010000u)
#define MM_SIGPAGE      0x7fff8000u  /* the signal-return trampolines, read+exec */
#define MM_SIGPAGE_ARM_RT   0x00u    /* offsets of the trampolines in the page */
#define MM_SIGPAGE_THUMB_RT 0x10u

/* Linux mmap flags this kernel honours. */
#define MAP_SHARED      0x01u
#define MAP_PRIVATE     0x02u
#define MAP_FIXED       0x10u
#define MAP_ANONYMOUS   0x20u
#define MAP_FIXED_NOREPLACE 0x100000u

struct mm {
    struct pgtable pt;
    struct vma_set vmas;
    uint32_t brk_start;  /* the heap's base, just past the loaded image */
    uint32_t brk;        /* its current end */
    uint32_t refs;       /* threads sharing it */
};

struct mm *mm_create(void);

/* Drops a reference; the last one frees every page and table. */
void mm_put(struct mm *mm);

/* Anonymous mapping. With MAP_FIXED at exactly addr (replacing what was
 * there); otherwise at addr if free, else the highest free gap. Returns the
 * address, or a negative errno. */
int32_t mm_map(struct mm *mm, uint32_t addr, uint32_t len, uint32_t prot, uint32_t flags);

int mm_unmap(struct mm *mm, uint32_t addr, uint32_t len);
int mm_protect(struct mm *mm, uint32_t addr, uint32_t len, uint32_t prot);

/* MADV_DONTNEED: drops the pages; the next touch sees zeroes. */
int mm_discard(struct mm *mm, uint32_t addr, uint32_t len);

/* Resolves a fault at addr for access (PT_READ, PT_WRITE or PT_EXEC).
 * KERR_OK when a page was mapped (or already allowed it), KERR_FAULT when
 * the access is illegal: the caller signals SIGSEGV. */
int mm_fault(struct mm *mm, uint32_t addr, uint32_t access);

/* Moves the program break; returns the new break (unchanged on failure). */
uint32_t mm_brk(struct mm *mm, uint32_t want);

/* User copies. KERR_OK or KERR_FAULT. */
int mm_copy_out(struct mm *mm, uint32_t uaddr, const void *src, uint32_t n);
int mm_copy_in(struct mm *mm, void *dst, uint32_t uaddr, uint32_t n);

/* Copies a NUL-terminated string of at most cap-1 bytes. Returns its
 * length, KERR_FAULT, or KERR_NAMETOOLONG. */
int mm_copy_string_in(struct mm *mm, char *dst, uint32_t uaddr, uint32_t cap);

/* The kernel address of user byte uaddr, faulting the page in for access;
 * NULL when illegal. Valid up to the end of that page. */
void *mm_user_page(struct mm *mm, uint32_t uaddr, uint32_t access);

/* True when the page holding addr is resident (mincore). */
bool mm_resident(const struct mm *mm, uint32_t addr);

#endif
