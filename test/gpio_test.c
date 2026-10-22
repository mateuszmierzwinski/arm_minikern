#include "core/gpio.h"

#include "ktest.h"

static void a_name_with_the_p_parses(void) {
    struct gpio_pin p = {9, 9};
    KTEST_CHECK(gpio_parse("PA10", 4, &p));
    KTEST_CHECK(p.port == 0 && p.pin == 10);
    /* PL used to be refused here as "past PH on this chip", which is true
     * of the main pin controller and wrong about the chip: PL is port 0 of
     * the always-on controller, and it is where a board's CPU-supply switch
     * lives. The assertion encoded the bug it was meant to guard against —
     * see test_the_port_the_supply_is_on. */
    KTEST_CHECK(gpio_parse("PL0", 3, &p) && p.port == GPIO_PORT_L && p.pin == 0);
}

static void the_p_is_optional(void) {
    struct gpio_pin p = {9, 9};
    KTEST_CHECK(gpio_parse("C7", 2, &p));
    KTEST_CHECK(p.port == 2 && p.pin == 7);
}

static void lower_case_is_accepted(void) {
    struct gpio_pin p = {9, 9};
    KTEST_CHECK(gpio_parse("ph3", 3, &p));
    KTEST_CHECK(p.port == 7 && p.pin == 3);
}

static void every_port_and_pin_boundary(void) {
    struct gpio_pin p = {9, 9};
    KTEST_CHECK(gpio_parse("PA0", 3, &p) && p.port == 0 && p.pin == 0);
    KTEST_CHECK(gpio_parse("PH31", 4, &p) && p.port == 7 && p.pin == 31);
    KTEST_CHECK(gpio_parse("PH32", 4, &p) == false);
    KTEST_CHECK(gpio_parse("PI0", 3, &p) == false);
}

/* A mis-typed pin must be refused rather than interpreted: driving the
 * wrong line is the failure this guards against. */
static void rubbish_is_refused(void) {
    struct gpio_pin p = {9, 9};
    KTEST_CHECK(gpio_parse("", 0, &p) == false);
    KTEST_CHECK(gpio_parse("P", 1, &p) == false);
    KTEST_CHECK(gpio_parse("PA", 2, &p) == false);
    KTEST_CHECK(gpio_parse("A", 1, &p) == false);
    KTEST_CHECK(gpio_parse("PA1x", 4, &p) == false);
    KTEST_CHECK(gpio_parse("PA 1", 4, &p) == false);
    KTEST_CHECK(gpio_parse("PA-1", 4, &p) == false);
    KTEST_CHECK(gpio_parse("10", 2, &p) == false);
    KTEST_CHECK(gpio_parse("PA999", 5, &p) == false);
}

/* The length is the bound, not a NUL: these come out of a kernel command
 * line, where the value runs up against the next option. */
static void the_length_bounds_it(void) {
    struct gpio_pin p = {9, 9};
    KTEST_CHECK(gpio_parse("PA10 minikern.other=1", 4, &p));
    KTEST_CHECK(p.port == 0 && p.pin == 10);
}

/* PL, which is the whole reason this parser exists.
 *
 * On every H3 board with a switchable CPU supply the pin that switches it
 * is on PL — the Orange Pi One's is PL6 — and this parser accepted only
 * PA..PH, so the one pin the feature was written for was rejected as a
 * typo. The letters between H and L have no controller on this chip and
 * must still be refused. */
static void test_the_port_the_supply_is_on(void) {
    struct gpio_pin pin = {.port = 0xff, .pin = 0xff};
    KTEST_CHECK(gpio_parse("PL6", 3, &pin));
    KTEST_CHECK(pin.port == GPIO_PORT_L && pin.pin == 6);
    KTEST_CHECK(gpio_is_r_port(pin));
    KTEST_CHECK(gpio_parse("pl10", 4, &pin) && pin.port == GPIO_PORT_L && pin.pin == 10);
    /* A main-controller pin must not look like an always-on one. */
    KTEST_CHECK(gpio_parse("PA10", 4, &pin) && !gpio_is_r_port(pin));
    /* The gaps are still gaps: no controller, so not a pin. */
    KTEST_CHECK(!gpio_parse("PI0", 3, &pin));
    KTEST_CHECK(!gpio_parse("PJ5", 3, &pin));
    KTEST_CHECK(!gpio_parse("PK5", 3, &pin));
    KTEST_CHECK(!gpio_parse("PM0", 3, &pin));
    KTEST_CHECK(!gpio_parse("PL32", 4, &pin));
}

int main(void) {
    KTEST_RUN(a_name_with_the_p_parses);
    KTEST_RUN(the_p_is_optional);
    KTEST_RUN(lower_case_is_accepted);
    KTEST_RUN(every_port_and_pin_boundary);
    KTEST_RUN(rubbish_is_refused);
    KTEST_RUN(the_length_bounds_it);
    KTEST_RUN(test_the_port_the_supply_is_on);
    return ktest_failures();
}
