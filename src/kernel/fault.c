#include "kernel/fault.h"

#include "arch/arm/pgtable.h"
#include "core/console.h"
#include "core/errno.h"
#include "kernel/proc.h"
#include "kernel/signal.h"
#include "kernel/vfp.h"

/* Short-descriptor fault status (ARM ARM B3.13.3): FS[3:0] in bits 0-3,
 * FS[4] in bit 10, WnR (a write) in bit 11. */
#define FSR_WNR        (1u << 11)
#define FS_ALIGNMENT   0x01u
#define FS_TRANS_SECT  0x05u
#define FS_TRANS_PAGE  0x07u
#define FS_PERM_SECT   0x0du
#define FS_PERM_PAGE   0x0fu
#define FS_ACCESS_SECT 0x03u
#define FS_ACCESS_PAGE 0x06u

static uint32_t fault_status(uint32_t fsr) {
    return (fsr & 0xfu) | ((fsr >> 6) & 0x10u);
}

static bool is_mmu_fault(uint32_t fs) {
    return fs == FS_TRANS_SECT || fs == FS_TRANS_PAGE || fs == FS_PERM_SECT || fs == FS_PERM_PAGE ||
           fs == FS_ACCESS_SECT || fs == FS_ACCESS_PAGE;
}

bool fault_handle(struct trapframe *tf, enum trap_kind kind, uint32_t addr, uint32_t status) {
    struct proc *p = proc_current();
    uint32_t fs = fault_status(status);
    uint32_t access = kind == TRAP_PREFETCH_ABORT ? PT_EXEC : (status & FSR_WNR) ? PT_WRITE : PT_READ;
    int rc;
    if (!trap_from_user(tf) || p == NULL) {
        return false;
    }
    if (fs == FS_ALIGNMENT) {
        signal_force_fault(tf, SIGBUS, 1, addr, status);  /* BUS_ADRALN */
        return true;
    }
    rc = is_mmu_fault(fs) ? mm_fault(p->mm, addr, access) : KERR_FAULT;
    if (rc == KERR_OK) {
        return true;
    }
    if (rc == KERR_NOMEM) {
        /* Out of memory is **not** a segmentation fault, and sending one
         * says the wrong thing: a runtime that catches SIGSEGV — Go's does —
         * turns it into "invalid memory address", and the real cause is
         * lost. The process is killed outright instead, with a signal it
         * cannot catch or misread, and the reason is printed once.
         *
         * This is the policy, and it has two halves: memory is handed out
         * optimistically (a Go runtime reserves half a gigabyte of address
         * space it will never touch, so refusing by size would refuse every
         * Go program), and the process that finally cannot be given a page
         * is the one that dies. */
        kprintf("%s[%u]: out of memory at %08x, killed\n", p->name, sched_current()->tid, addr);
        proc_exit_group(SIGKILL);
    }
    signal_force_fault(tf, SIGSEGV, vma_find(&p->mm->vmas, addr) != NULL ? SEGV_ACCERR : SEGV_MAPERR, addr,
                       status);
    return true;
}

void fault_undefined(struct trapframe *tf) {
    /* A floating-point instruction on a thread whose unit is off arrives
     * here, and is not an error: the state is moved and the instruction
     * retried (kernel/vfp.h). Only what is left is really undefined. */
    if (vfp_claim(sched_current())) {
        return;
    }
    signal_force_fault(tf, SIGILL, ILL_ILLOPC, tf->pc, 0);
}
