#include "core/gpio.h"

#include "ktest.h"

static void a_name_with_the_p_parses(void) {
    struct gpio_pin p = {9, 9};
    KTEST_CHECK(gpio_parse("PA10", 4, &p));
    KTEST_CHECK(p.port == 0 && p.pin == 10);
    KTEST_CHECK(gpio_parse("PL0", 3, &p) == false); /* PL is past PH on this chip */
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

int main(void) {
    KTEST_RUN(a_name_with_the_p_parses);
    KTEST_RUN(the_p_is_optional);
    KTEST_RUN(lower_case_is_accepted);
    KTEST_RUN(every_port_and_pin_boundary);
    KTEST_RUN(rubbish_is_refused);
    KTEST_RUN(the_length_bounds_it);
    return ktest_failures();
}
