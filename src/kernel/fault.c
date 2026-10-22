#include "kernel/fault.h"

#include "arch/arm/pgtable.h"
#include "core/console.h"
#include "core/errno.h"
#include "kernel/proc.h"
#include "kernel/signal.h"

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
        kprintf("%s[%u]: out of memory at %08x\n", p->name, sched_current()->tid, addr);
    }
    signal_force_fault(tf, SIGSEGV, vma_find(&p->mm->vmas, addr) != NULL ? SEGV_ACCERR : SEGV_MAPERR, addr,
                       status);
    return true;
}

void fault_undefined(struct trapframe *tf) {
    signal_force_fault(tf, SIGILL, ILL_ILLOPC, tf->pc, 0);
}
