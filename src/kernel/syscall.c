#include "kernel/syscall.h"

#include "core/console.h"
#include "core/errno.h"
#include "kernel/panic.h"
#include "kernel/proc.h"

static const struct syscall_entry *table[SYSCALL_MAX];
static const struct syscall_entry *arm_table[SYSCALL_ARM_MAX];
static uint32_t logged[(SYSCALL_MAX + 31u) / 32u];
static int trace;

static void install(const struct syscall_entry *entries) {
    for (const struct syscall_entry *e = entries; e->call != NULL; e++) {
        if (e->nr >= SYSCALL_ARM_BASE && e->nr - SYSCALL_ARM_BASE < SYSCALL_ARM_MAX) {
            arm_table[e->nr - SYSCALL_ARM_BASE] = e;
        } else if (e->nr < SYSCALL_MAX && table[e->nr] == NULL) {
            table[e->nr] = e;
        } else {
            panic("syscall: bad or duplicate entry %u (%s)", e->nr, e->name);
        }
    }
}

void syscall_init(void) {
    install(sys_proc_table);
    install(sys_mem_table);
    install(sys_time_table);
    install(sys_signal_table);
    install(sys_file_table);
    install(sys_futex_table);
    install(sys_dir_table);
    install(sys_poll_table);
    install(sys_socket_table);
}

void syscall_set_trace(int on) {
    trace = on;
}

static const struct syscall_entry *lookup(uint32_t nr) {
    if (nr >= SYSCALL_ARM_BASE && nr - SYSCALL_ARM_BASE < SYSCALL_ARM_MAX) {
        return arm_table[nr - SYSCALL_ARM_BASE];
    }
    return nr < SYSCALL_MAX ? table[nr] : NULL;
}

/* The first call of each unknown number is logged, later ones are not. */
static void log_unknown(uint32_t nr, const struct trapframe *tf) {
    uint32_t slot = nr < SYSCALL_MAX ? nr : SYSCALL_MAX - 1u;
    if ((logged[slot / 32u] & (1u << (slot % 32u))) == 0) {
        logged[slot / 32u] |= 1u << (slot % 32u);
        kprintf("syscall: %u not implemented (r0 %08x r1 %08x r2 %08x) from %s\n", nr, tf->r[0], tf->r[1],
                tf->r[2], proc_current() != NULL ? proc_current()->name : "?");
    }
}

void syscall_dispatch(struct trapframe *tf) {
    uint32_t nr = tf->r[7];
    const struct syscall_entry *e = lookup(nr);
    int32_t rc;
    if (e == NULL) {
        log_unknown(nr, tf);
        tf->r[0] = (uint32_t)KERR_NOSYS;
        return;
    }
    if (trace) {
        kprintf("[%u] %s(%x, %x, %x, %x)", sched_current()->tid, e->name, tf->r[0], tf->r[1], tf->r[2], tf->r[3]);
    }
    rc = e->call(tf);
    if (trace) {
        kprintf(" = %d\n", rc);
    }
    tf->r[0] = (uint32_t)rc;
}
