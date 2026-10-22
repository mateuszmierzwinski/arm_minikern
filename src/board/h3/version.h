/* version — the build half of MAJOR.MINOR.BUILD.
 *
 * The series (`MINIKERN_VERSION`) comes from the monorepo's one constant
 * through the build files; the build number is **the clock's** and comes
 * from Bazel's stamp, so two builds of one series are always ordered and
 * nobody picks a build number by hand (master Rule 28).
 *
 * It is **0** in a build without `--stamp`, which is the honest answer
 * rather than an invented number — and it is why a release build is a
 * `--stamp` build.
 */
#ifndef MINIKERN_BOARD_H3_VERSION_H
#define MINIKERN_BOARD_H3_VERSION_H

#include <stdint.h>

/* The unix time the kernel was built, or 0 when it was not stamped. */
extern const uint32_t minikern_build;

#endif
