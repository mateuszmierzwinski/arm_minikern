/* The frequency policy, which is the half of DVFS that can be tested here:
 * QEMU models neither the H3's thermal sensor nor its PLLs, so the adapters
 * are unverified until the board runs, and every decision they will act on
 * is pinned down here instead. */
#include "core/cpufreq.h"
#include "ktest.h"

/* The H3's published operating points, lowest first (CLAUDE.md §12). */
static struct cpufreq_policy h3_policy(void) {
    struct cpufreq_policy p = {
        .opps = {{480000, 1040}, {648000, 1040}, {816000, 1100}, {912000, 1200}, {1008000, 1300}},
        .nopps = 5,
        .throttle_mC = 80000,
        .critical_mC = 95000,
        .hysteresis_mC = 5000,
        .load_up_pct = 80,
        .load_down_pct = 20,
    };
    return p;
}

static void a_sane_policy_is_accepted(void) {
    struct cpufreq_policy p = h3_policy();
    KTEST_CHECK(cpufreq_policy_valid(&p));
}

static void points_must_rise(void) {
    struct cpufreq_policy p = h3_policy();
    p.opps[2].khz = p.opps[1].khz; /* not ascending */
    KTEST_CHECK(!cpufreq_policy_valid(&p));
}

static void a_higher_point_may_not_need_less_voltage(void) {
    struct cpufreq_policy p = h3_policy();
    p.opps[3].mv = 1000;
    KTEST_CHECK(!cpufreq_policy_valid(&p));
}

static void thresholds_must_be_ordered(void) {
    struct cpufreq_policy p = h3_policy();
    p.critical_mC = p.throttle_mC;
    KTEST_CHECK(!cpufreq_policy_valid(&p));
    p = h3_policy();
    p.hysteresis_mC = 0;
    KTEST_CHECK(!cpufreq_policy_valid(&p));
}

static void load_limits_must_not_overlap(void) {
    struct cpufreq_policy p = h3_policy();
    p.load_down_pct = 90;
    KTEST_CHECK(!cpufreq_policy_valid(&p));
}

static void an_empty_policy_is_refused(void) {
    struct cpufreq_policy p = h3_policy();
    p.nopps = 0;
    KTEST_CHECK(!cpufreq_policy_valid(&p));
    p.nopps = CPUFREQ_MAX_OPPS + 1;
    KTEST_CHECK(!cpufreq_policy_valid(&p));
}

static void load_raises_one_step_at_a_time(void) {
    struct cpufreq_policy p = h3_policy();
    KTEST_CHECK((cpufreq_pick(&p, 0, 40000, 100)) == (1u));
    KTEST_CHECK((cpufreq_pick(&p, 1, 40000, 100)) == (2u));
    KTEST_CHECK((cpufreq_pick(&p, 4, 40000, 100)) == (4u)); /* already at the top */
}

static void idleness_lowers_one_step_at_a_time(void) {
    struct cpufreq_policy p = h3_policy();
    KTEST_CHECK((cpufreq_pick(&p, 4, 40000, 0)) == (3u));
    KTEST_CHECK((cpufreq_pick(&p, 1, 40000, 0)) == (0u));
    KTEST_CHECK((cpufreq_pick(&p, 0, 40000, 0)) == (0u)); /* already at the bottom */
}

static void a_middling_load_holds(void) {
    struct cpufreq_policy p = h3_policy();
    KTEST_CHECK((cpufreq_pick(&p, 2, 40000, 50)) == (2u));
}

/* Temperature overrules load: this is the whole point of doing thermal
 * before DVFS, so it is asserted with the load asking for the opposite. */
static void heat_beats_a_full_load(void) {
    struct cpufreq_policy p = h3_policy();
    KTEST_CHECK((cpufreq_pick(&p, 3, 85000, 100)) == (2u));
    KTEST_CHECK((cpufreq_pick(&p, 2, 85000, 100)) == (1u));
}

static void a_critical_temperature_goes_straight_to_the_bottom(void) {
    struct cpufreq_policy p = h3_policy();
    KTEST_CHECK((cpufreq_pick(&p, 4, 96000, 100)) == (0u));
    KTEST_CHECK((cpufreq_pick(&p, 2, 150000, 100)) == (0u));
}

/* The hysteresis band: just under the throttle point, a full load must not
 * raise the frequency, or the CPU flaps across the threshold for ever. */
static void the_hysteresis_band_refuses_to_rise(void) {
    struct cpufreq_policy p = h3_policy();
    KTEST_CHECK((cpufreq_pick(&p, 2, 79000, 100)) == (2u));
    KTEST_CHECK((cpufreq_pick(&p, 2, 76000, 100)) == (2u));
    KTEST_CHECK((cpufreq_pick(&p, 2, 74000, 100)) == (3u)); /* clear of the band */
}

/* A sweep from cold and idle to hot and busy must settle, not oscillate:
 * after the temperature stops changing, so must the operating point. */
static void a_steady_temperature_settles(void) {
    struct cpufreq_policy p = h3_policy();
    uint32_t at = 0, last = 99;
    uint32_t stable = 0;
    for (uint32_t round = 0; round < 50; round++) {
        at = cpufreq_pick(&p, at, 82000, 100);
        stable = at == last ? stable + 1 : 0;
        last = at;
    }
    KTEST_CHECK((at) == (0u));
    KTEST_CHECK(stable > 40);
}

static void the_reason_is_reported(void) {
    struct cpufreq_policy p = h3_policy();
    enum cpufreq_reason why = CPUFREQ_HOLD;
    (void)cpufreq_decide(&p, 3, 96000, 0, &why);
    KTEST_CHECK(((uint32_t)why) == ((uint32_t)CPUFREQ_CRITICAL));
    (void)cpufreq_decide(&p, 3, 85000, 100, &why);
    KTEST_CHECK(((uint32_t)why) == ((uint32_t)CPUFREQ_THROTTLE));
    (void)cpufreq_decide(&p, 1, 40000, 100, &why);
    KTEST_CHECK(((uint32_t)why) == ((uint32_t)CPUFREQ_LOAD_UP));
    (void)cpufreq_decide(&p, 1, 40000, 0, &why);
    KTEST_CHECK(((uint32_t)why) == ((uint32_t)CPUFREQ_LOAD_DOWN));
    (void)cpufreq_decide(&p, 2, 40000, 50, &why);
    KTEST_CHECK(((uint32_t)why) == ((uint32_t)CPUFREQ_HOLD));
    KTEST_CHECK(cpufreq_reason_name(CPUFREQ_THROTTLE)[0] == 't');
}

/* An index past the table must not read past it: the current point comes
 * from a board adapter, and a wrong one is a bug, not a crash. */
static void a_nonsense_current_point_is_clamped(void) {
    struct cpufreq_policy p = h3_policy();
    KTEST_CHECK((cpufreq_pick(&p, 99, 40000, 0)) == (3u));
    KTEST_CHECK((cpufreq_pick(&p, 99, 96000, 0)) == (0u));
}

int main(void) {
    KTEST_RUN(a_sane_policy_is_accepted);
    KTEST_RUN(points_must_rise);
    KTEST_RUN(a_higher_point_may_not_need_less_voltage);
    KTEST_RUN(thresholds_must_be_ordered);
    KTEST_RUN(load_limits_must_not_overlap);
    KTEST_RUN(an_empty_policy_is_refused);
    KTEST_RUN(load_raises_one_step_at_a_time);
    KTEST_RUN(idleness_lowers_one_step_at_a_time);
    KTEST_RUN(a_middling_load_holds);
    KTEST_RUN(heat_beats_a_full_load);
    KTEST_RUN(a_critical_temperature_goes_straight_to_the_bottom);
    KTEST_RUN(the_hysteresis_band_refuses_to_rise);
    KTEST_RUN(a_steady_temperature_settles);
    KTEST_RUN(the_reason_is_reported);
    KTEST_RUN(a_nonsense_current_point_is_clamped);
    return ktest_failures();
}
