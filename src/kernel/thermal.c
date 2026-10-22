#include "kernel/thermal.h"

#include "core/console.h"
#include "core/errno.h"
#include "kernel/sched.h"
#include "kernel/time.h"

/* The governor's whole state. One instance: there is one CPU complex on an
 * H3, and all four cores share its clock and its voltage. */
static struct {
    struct thermal_port sensor;
    struct cpuclock_port clock;
    struct cpufreq_policy policy;
    uint32_t interval_ms;
    uint32_t ticks_left;
    uint32_t busy_ticks;  /* ticks in this interval with something to run */
    uint32_t total_ticks;
    uint32_t opp;
    int32_t temp_mC;
    uint32_t unbelievable; /* readings no die could produce (see plausible) */
    bool running;
    bool critical;        /* already reported: a hot chip must not flood the log */
} gov;

/* The share of the interval on which a core had work, as a percentage. An
 * interval with no ticks counted is reported idle rather than divided by
 * zero. */
static uint32_t load_pct(void) {
    return gov.total_ticks == 0 ? 0 : gov.busy_ticks * 100u / gov.total_ticks;
}

/* Applies an operating point, reporting what it did and why. A failure is
 * logged and the recorded point is left alone, so the next decision starts
 * from where the CPU actually is. */
static void apply(uint32_t want, enum cpufreq_reason why) {
    const struct cpu_opp *opp = &gov.policy.opps[want];
    int rc = gov.clock.set(gov.clock.self, opp->khz, opp->mv);
    if (rc != KERR_OK) {
        kprintf("cpufreq: %u kHz refused (%d)\n", opp->khz, rc);
        return;
    }
    kprintf("cpufreq: %u kHz at %u mV (%s, %d mC, load %u%%)\n", opp->khz, opp->mv, cpufreq_reason_name(why),
            gov.temp_mC, load_pct());
    gov.opp = want;
}

/* Says so, once, when the chip is critically hot with nothing left to give
 * up. There is no frequency change to log in that case — the CPU is already
 * at its lowest point — and silence would be the wrong answer to the one
 * reading an operator most needs to see. It is said again only after the
 * temperature has come back below the throttle point. */
static void report_if_critical(enum cpufreq_reason why) {
    if (why != CPUFREQ_CRITICAL) {
        gov.critical = gov.temp_mC < gov.policy.throttle_mC ? false : gov.critical;
        return;
    }
    if (!gov.critical) {
        kprintf("cpufreq: critical temperature, %d mC at %u kHz (the lowest point)\n", gov.temp_mC,
                gov.policy.opps[0].khz);
        gov.critical = true;
    }
}

/* True when a reading could have come from a real die.
 *
 * A read that *fails* was always treated as the worst case; a read that
 * **succeeds with an impossible number** was not, and that is the dangerous
 * one: this chip's conversion turns the sample from an absent or unclocked
 * sensor into -264 °C, and the governor would have answered that by going to
 * the top of the table and leaving it there with nothing watching the
 * temperature. The band is deliberately wide — a board can legitimately be
 * cold — and only excludes what no silicon reports. */
static bool plausible(int32_t mC) {
    return mC > -40000 && mC < 150000;
}

void thermal_step(void) {
    enum cpufreq_reason why;
    uint32_t want;
    if (gov.sensor.read_mC(gov.sensor.self, &gov.temp_mC) != KERR_OK ||
        !plausible(gov.temp_mC)) {
        /* A sensor that cannot be read, or cannot be believed, is treated as
         * the worst case: the one thing worse than throttling needlessly is
         * not throttling. */
        gov.unbelievable += plausible(gov.temp_mC) ? 0u : 1u;
        gov.temp_mC = gov.policy.critical_mC;
    }
    want = cpufreq_decide(&gov.policy, gov.opp, gov.temp_mC, load_pct(), &why);
    if (want != gov.opp) {
        apply(want, why);
    }
    report_if_critical(why);
    gov.busy_ticks = 0;
    gov.total_ticks = 0;
}

/* Counts the tick, and decides once an interval's worth have passed. */
static void on_tick(void *self, uint64_t now_ns) {
    (void)self;
    (void)now_ns;
    gov.total_ticks++;
    gov.busy_ticks += sched_has_work() ? 1u : 0u;
    if (--gov.ticks_left == 0) {
        gov.ticks_left = gov.interval_ms;
        thermal_step();
    }
}

uint32_t thermal_unbelievable_readings(void) {
    return gov.unbelievable;
}

int thermal_start(const struct thermal_port *sensor, const struct cpuclock_port *clock,
                  const struct cpufreq_policy *policy, uint32_t interval_ms) {
    struct tick_hook hook = {.tick = on_tick, .self = NULL};
    if (!cpufreq_policy_valid(policy) || interval_ms == 0) {
        return KERR_INVAL;
    }
    gov.sensor = *sensor;
    gov.clock = *clock;
    gov.policy = *policy;
    gov.interval_ms = interval_ms;
    gov.ticks_left = interval_ms;
    gov.opp = 0;
    gov.running = true;
    /* Start at the lowest point rather than assuming where the boot loader
     * left the CPU: the first decision then raises it if the load asks. */
    apply(0, CPUFREQ_HOLD);
    return time_add_hook(&hook);
}

void thermal_state(int32_t *temp_mC, uint32_t *khz, uint32_t *opp) {
    *temp_mC = gov.temp_mC;
    *khz = gov.running ? gov.policy.opps[gov.opp].khz : 0;
    *opp = gov.opp;
}

void thermal_report(void) {
    if (!gov.running) {
        kprintf("cpufreq: not running\n");
        return;
    }
    kprintf("cpufreq: %u kHz (point %u of %u), %d mC, load %u%%\n", gov.policy.opps[gov.opp].khz, gov.opp + 1u,
            gov.policy.nopps, gov.temp_mC, load_pct());
}
