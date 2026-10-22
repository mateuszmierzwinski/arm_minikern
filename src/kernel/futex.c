#include "kernel/futex.h"

#include "core/errno.h"
#include "kernel/sched.h"

int futex_wait(struct mm *mm, uint32_t uaddr, uint32_t val, uint64_t deadline_ns) {
    uint32_t now;
    if ((uaddr & 3u) != 0) {
        return KERR_INVAL;
    }
    if (mm_copy_in(mm, &now, uaddr, 4) != KERR_OK) {
        return KERR_FAULT;
    }
    if (now != val) {
        return KERR_AGAIN;
    }
    return sched_sleep(mm, uaddr, deadline_ns, true);
}

uint32_t futex_wake(struct mm *mm, uint32_t uaddr, uint32_t n) {
    return sched_wake(mm, uaddr, n);
}
