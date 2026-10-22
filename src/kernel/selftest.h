/* selftest — deliberate faults, chosen on the command line, so a QEMU test
 * can prove the exception path end to end (minikern.selftest=<name>). */
#ifndef MINIKERN_KERNEL_SELFTEST_H
#define MINIKERN_KERNEL_SELFTEST_H

/* Runs the named fault ("undef", "dabt", "pabt"); unknown names are
 * reported and ignored. Returns only when nothing faulted. */
void selftest_run(const char *name, unsigned len);

#endif
