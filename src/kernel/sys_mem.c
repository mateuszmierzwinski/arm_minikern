/* sys_mem — memory syscalls: mmap2, munmap, mprotect, madvise, brk, mincore. */
#include "core/errno.h"
#include "kernel/mm.h"
#include "kernel/proc.h"
#include "kernel/syscall.h"

#include <stddef.h>

#define MADV_DONTNEED 4u
#define MADV_FREE 8u
#define ENODEV (-19)

static struct mm *cur_mm(void) {
    return proc_current()->mm;
}

/* mmap2(addr, len, prot, flags, fd, pgoff): anonymous memory only. */
static int32_t sys_mmap2(struct trapframe *tf) {
    uint32_t flags = tf->r[3];
    if ((flags & MAP_ANONYMOUS) == 0) {
        return ENODEV;
    }
    return mm_map(cur_mm(), tf->r[0], tf->r[1], tf->r[2], flags);
}

static int32_t sys_munmap(struct trapframe *tf) {
    return mm_unmap(cur_mm(), tf->r[0], tf->r[1]);
}

static int32_t sys_mprotect(struct trapframe *tf) {
    return mm_protect(cur_mm(), tf->r[0], tf->r[1], tf->r[2]);
}

/* Only the advice that changes contents matters; the rest is a hint. */
static int32_t sys_madvise(struct trapframe *tf) {
    uint32_t advice = tf->r[2];
    if (advice == MADV_DONTNEED || advice == MADV_FREE) {
        return mm_discard(cur_mm(), tf->r[0], tf->r[1]);
    }
    return KERR_OK;
}

static int32_t sys_brk(struct trapframe *tf) {
    return (int32_t)mm_brk(cur_mm(), tf->r[0]);
}

/* mincore(addr, len, vec): one byte per page, 1 when resident. */
static int32_t sys_mincore(struct trapframe *tf) {
    struct mm *mm = cur_mm();
    uint32_t addr = tf->r[0], pages = (tf->r[1] + 4095u) / 4096u;
    if ((addr & 4095u) != 0) {
        return KERR_INVAL;
    }
    for (uint32_t i = 0; i < pages; i++) {
        uint8_t in = mm_resident(mm, addr + i * 4096u) ? 1 : 0;
        if (vma_find(&mm->vmas, addr + i * 4096u) == NULL) {
            return KERR_NOMEM;
        }
        if (mm_copy_out(mm, tf->r[2] + i, &in, 1) != KERR_OK) {
            return KERR_FAULT;
        }
    }
    return KERR_OK;
}

const struct syscall_entry sys_mem_table[] = {
    {45, "brk", sys_brk},
    {91, "munmap", sys_munmap},
    {125, "mprotect", sys_mprotect},
    {192, "mmap2", sys_mmap2},
    {219, "mincore", sys_mincore},
    {220, "madvise", sys_madvise},
    {0, NULL, NULL},
};
