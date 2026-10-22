/* sys_time — clocks and sleeping: clock_gettime(64), clock_getres(64),
 * gettimeofday, nanosleep, clock_nanosleep(64). Every clock is derived from
 * the generic timer; the CPU-time clocks report elapsed time, which is an
 * upper bound and all a single-core runtime uses them for. */
#include "core/errno.h"
#include "kernel/proc.h"
#include "kernel/sched.h"
#include "kernel/syscall.h"

#include <stddef.h>
#include "kernel/time.h"

#define CLOCK_REALTIME 0u
#define CLOCK_MONOTONIC 1u
#define CLOCK_REALTIME_COARSE 5u
#define CLOCK_TAI 11u
#define TIMER_ABSTIME 1u
#define CLOCK_RESOLUTION_NS 1u

/* struct timespec with 32-bit fields (the pre-time64 calls). */
struct timespec32 {
    int32_t sec;
    int32_t nsec;
};

/* struct __kernel_timespec (the *_time64 calls). */
struct timespec64 {
    int64_t sec;
    int64_t nsec;
};

static int sleep_chan;

static bool is_realtime(uint32_t clock) {
    return clock == CLOCK_REALTIME || clock == CLOCK_REALTIME_COARSE || clock == CLOCK_TAI;
}

/* The clock's current value in ns, or KERR_INVAL for an unknown clock. */
static int clock_now(uint32_t clock, uint64_t *ns) {
    if (clock > CLOCK_TAI) {
        return KERR_INVAL;
    }
    *ns = is_realtime(clock) ? time_realtime_ns() : time_monotonic_ns();
    return KERR_OK;
}

static int put_time(uint32_t uaddr, uint64_t ns, bool wide) {
    struct timespec64 t64 = {.sec = (int64_t)(ns / NS_PER_SEC), .nsec = (int64_t)(ns % NS_PER_SEC)};
    struct timespec32 t32 = {.sec = (int32_t)t64.sec, .nsec = (int32_t)t64.nsec};
    struct mm *mm = proc_current()->mm;
    return wide ? mm_copy_out(mm, uaddr, &t64, sizeof t64) : mm_copy_out(mm, uaddr, &t32, sizeof t32);
}

/* Reads a timespec into ns; KERR_INVAL for a negative or overlong one. */
static int get_time(uint32_t uaddr, bool wide, uint64_t *ns) {
    struct mm *mm = proc_current()->mm;
    struct timespec64 t = {0};
    struct timespec32 t32;
    int rc = wide ? mm_copy_in(mm, &t, uaddr, sizeof t) : mm_copy_in(mm, &t32, uaddr, sizeof t32);
    if (rc != KERR_OK) {
        return rc;
    }
    if (!wide) {
        t = (struct timespec64){.sec = t32.sec, .nsec = t32.nsec};
    }
    if (t.sec < 0 || t.nsec < 0 || t.nsec >= (int64_t)NS_PER_SEC) {
        return KERR_INVAL;
    }
    *ns = (uint64_t)t.sec * NS_PER_SEC + (uint64_t)t.nsec;
    return KERR_OK;
}

static int32_t clock_gettime_common(struct trapframe *tf, bool wide) {
    uint64_t ns;
    int rc = clock_now(tf->r[0], &ns);
    return rc != KERR_OK ? rc : put_time(tf->r[1], ns, wide);
}

static int32_t sys_clock_gettime(struct trapframe *tf) {
    return clock_gettime_common(tf, false);
}

static int32_t sys_clock_gettime64(struct trapframe *tf) {
    return clock_gettime_common(tf, true);
}

static int32_t clock_getres_common(struct trapframe *tf, bool wide) {
    uint64_t ns;
    if (clock_now(tf->r[0], &ns) != KERR_OK) {
        return KERR_INVAL;
    }
    return tf->r[1] == 0 ? KERR_OK : put_time(tf->r[1], CLOCK_RESOLUTION_NS, wide);
}

static int32_t sys_clock_getres(struct trapframe *tf) {
    return clock_getres_common(tf, false);
}

static int32_t sys_clock_getres64(struct trapframe *tf) {
    return clock_getres_common(tf, true);
}

/* gettimeofday(tv, tz): struct timeval {int32 sec, int32 usec}. */
static int32_t sys_gettimeofday(struct trapframe *tf) {
    uint64_t ns = time_realtime_ns();
    struct timespec32 tv = {.sec = (int32_t)(ns / NS_PER_SEC), .nsec = (int32_t)((ns % NS_PER_SEC) / 1000u)};
    return tf->r[0] == 0 ? KERR_OK : mm_copy_out(proc_current()->mm, tf->r[0], &tv, sizeof tv);
}

/* settimeofday(tv, tz) and clock_settime(id, ts): set the wall clock.
 *
 * Only CLOCK_REALTIME can be set — the monotonic clock is the generic
 * timer, and a clock that can jump backwards is not monotonic. There is no
 * privilege check because there is one process; a system with users would
 * want one here. */
static int32_t set_realtime(uint64_t ns) {
    time_set_realtime_ns(ns);
    return KERR_OK;
}

static int32_t sys_settimeofday(struct trapframe *tf) {
    struct timespec32 tv;
    if (tf->r[0] == 0) {
        return KERR_OK; /* only the timezone, which this kernel does not keep */
    }
    if (mm_copy_in(proc_current()->mm, &tv, tf->r[0], sizeof tv) != KERR_OK) {
        return KERR_FAULT;
    }
    if (tv.sec < 0 || tv.nsec < 0 || tv.nsec >= 1000000) {
        return KERR_INVAL; /* a struct timeval carries microseconds */
    }
    return set_realtime((uint64_t)tv.sec * NS_PER_SEC + (uint64_t)tv.nsec * 1000u);
}

/* clock_settime (32-bit time) and clock_settime64 share everything but the
 * width of the seconds field. */
static int32_t clock_settime_common(struct trapframe *tf, bool wide) {
    struct timespec32 narrow;
    struct timespec64 wide_ts;
    int64_t sec;
    int32_t nsec;
    if (!is_realtime(tf->r[0])) {
        /* A monotonic clock that could be set would not be monotonic; any
         * other identifier is simply not a clock here. */
        return tf->r[0] == CLOCK_MONOTONIC ? KERR_PERM : KERR_INVAL;
    }
    if (wide) {
        if (mm_copy_in(proc_current()->mm, &wide_ts, tf->r[1], sizeof wide_ts) != KERR_OK) {
            return KERR_FAULT;
        }
        sec = wide_ts.sec;
        nsec = (int32_t)wide_ts.nsec;
    } else {
        if (mm_copy_in(proc_current()->mm, &narrow, tf->r[1], sizeof narrow) != KERR_OK) {
            return KERR_FAULT;
        }
        sec = narrow.sec;
        nsec = narrow.nsec;
    }
    if (sec < 0 || nsec < 0 || nsec >= (int32_t)NS_PER_SEC) {
        return KERR_INVAL;
    }
    return set_realtime((uint64_t)sec * NS_PER_SEC + (uint64_t)nsec);
}

static int32_t sys_clock_settime(struct trapframe *tf) {
    return clock_settime_common(tf, false);
}

static int32_t sys_clock_settime64(struct trapframe *tf) {
    return clock_settime_common(tf, true);
}

/* Sleeps until the monotonic deadline; a signal ends it early with the
 * time left written to rem (when given). */
static int sleep_until(uint64_t deadline, uint32_t rem_uaddr, bool wide) {
    struct task *t = sched_current();
    int rc = sched_sleep(&sleep_chan, t->tid, deadline == 0 ? 1 : deadline, true);
    uint64_t now;
    if (rc != KERR_INTR) {
        return KERR_OK;
    }
    now = time_monotonic_ns();
    if (rem_uaddr != 0) {
        (void)put_time(rem_uaddr, deadline > now ? deadline - now : 0, wide);
    }
    return KERR_INTR;
}

static int32_t sys_nanosleep(struct trapframe *tf) {
    uint64_t ns;
    int rc = get_time(tf->r[0], false, &ns);
    return rc != KERR_OK ? rc : sleep_until(time_monotonic_ns() + ns, tf->r[1], false);
}

/* clock_nanosleep(clock, flags, req, rem): TIMER_ABSTIME is a point on
 * that clock, converted to the monotonic deadline the scheduler uses. */
static int32_t clock_nanosleep_common(struct trapframe *tf, bool wide) {
    uint64_t ns, now, deadline;
    int rc = get_time(tf->r[2], wide, &ns);
    if (rc != KERR_OK || clock_now(tf->r[0], &now) != KERR_OK) {
        return rc != KERR_OK ? rc : KERR_INVAL;
    }
    if ((tf->r[1] & TIMER_ABSTIME) == 0) {
        deadline = time_monotonic_ns() + ns;
    } else {
        deadline = time_monotonic_ns() + (ns > now ? ns - now : 0);
    }
    rc = sleep_until(deadline, (tf->r[1] & TIMER_ABSTIME) ? 0 : tf->r[3], wide);
    return rc;
}

static int32_t sys_clock_nanosleep(struct trapframe *tf) {
    return clock_nanosleep_common(tf, false);
}

static int32_t sys_clock_nanosleep64(struct trapframe *tf) {
    return clock_nanosleep_common(tf, true);
}

const struct syscall_entry sys_time_table[] = {
    {78, "gettimeofday", sys_gettimeofday},
    {79, "settimeofday", sys_settimeofday},
    {162, "nanosleep", sys_nanosleep},
    {262, "clock_settime", sys_clock_settime},
    {263, "clock_gettime", sys_clock_gettime},
    {264, "clock_getres", sys_clock_getres},
    {265, "clock_nanosleep", sys_clock_nanosleep},
    {403, "clock_gettime64", sys_clock_gettime64},
    {404, "clock_settime64", sys_clock_settime64},
    {406, "clock_getres_time64", sys_clock_getres64},
    {407, "clock_nanosleep_time64", sys_clock_nanosleep64},
    {0, NULL, NULL},
};
