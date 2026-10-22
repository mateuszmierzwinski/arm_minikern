/* sys_futex — futex and futex_time64: WAIT, WAKE and their BITSET forms
 * (a bitset of all ones, the only one Go and musl pass, is plain WAIT/WAKE). */
#include "core/errno.h"
#include "kernel/futex.h"
#include "kernel/proc.h"
#include "kernel/syscall.h"

#include <stddef.h>
#include "kernel/time.h"

#define FUTEX_WAIT 0u
#define FUTEX_WAKE 1u
#define FUTEX_WAIT_BITSET 9u
#define FUTEX_WAKE_BITSET 10u
#define FUTEX_CMD_MASK 0x7fu  /* strips FUTEX_PRIVATE_FLAG and FUTEX_CLOCK_REALTIME */

struct ts32 {
    int32_t sec;
    int32_t nsec;
};

struct ts64 {
    int64_t sec;
    int64_t nsec;
};

/* The timeout as a monotonic deadline (0 = none). WAIT's is relative,
 * WAIT_BITSET's absolute on CLOCK_MONOTONIC. */
static int deadline_of(uint32_t uaddr, bool wide, bool absolute, uint64_t *deadline) {
    struct mm *mm = proc_current()->mm;
    struct ts64 t = {0};
    struct ts32 t32;
    int rc;
    *deadline = 0;
    if (uaddr == 0) {
        return KERR_OK;
    }
    rc = wide ? mm_copy_in(mm, &t, uaddr, sizeof t) : mm_copy_in(mm, &t32, uaddr, sizeof t32);
    if (!wide && rc == KERR_OK) {
        t = (struct ts64){.sec = t32.sec, .nsec = t32.nsec};
    }
    if (rc != KERR_OK || t.sec < 0 || t.nsec < 0 || t.nsec >= (int64_t)NS_PER_SEC) {
        return rc != KERR_OK ? rc : KERR_INVAL;
    }
    *deadline = (uint64_t)t.sec * NS_PER_SEC + (uint64_t)t.nsec + (absolute ? 0u : time_monotonic_ns());
    *deadline = *deadline == 0 ? 1 : *deadline;
    return KERR_OK;
}

/* futex(uaddr, op, val, timeout, uaddr2, val3). */
static int32_t futex_common(struct trapframe *tf, bool wide) {
    struct mm *mm = proc_current()->mm;
    uint32_t cmd = tf->r[1] & FUTEX_CMD_MASK;
    uint64_t deadline;
    int rc;
    if (cmd == FUTEX_WAKE || cmd == FUTEX_WAKE_BITSET) {
        return (int32_t)futex_wake(mm, tf->r[0], tf->r[2]);
    }
    if (cmd != FUTEX_WAIT && cmd != FUTEX_WAIT_BITSET) {
        return KERR_NOSYS;
    }
    rc = deadline_of(tf->r[3], wide, cmd == FUTEX_WAIT_BITSET, &deadline);
    if (rc != KERR_OK) {
        return rc;
    }
    return futex_wait(mm, tf->r[0], tf->r[2], deadline);
}

static int32_t sys_futex(struct trapframe *tf) {
    return futex_common(tf, false);
}

static int32_t sys_futex64(struct trapframe *tf) {
    return futex_common(tf, true);
}

const struct syscall_entry sys_futex_table[] = {
    {240, "futex", sys_futex},
    {422, "futex_time64", sys_futex64},
    {0, NULL, NULL},
};
