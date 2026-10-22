/* procfs — the generated files under /proc that Go runtimes read:
 * self/stat (bdwgc, TinyGo's collector, takes its stack bottom from field
 * 28) and self/exe's target via readlink. Contents are built at open. */
#ifndef MINIKERN_KERNEL_PROCFS_H
#define MINIKERN_KERNEL_PROCFS_H

#include <stdint.h>

struct file;
struct proc;

/* name is the part after "/proc/". */
int procfs_open(const char *name, uint32_t flags, struct file **out);

/* /proc/self/stat's line for p into buf; returns its length. Exposed for
 * the boot test's sanity check of the format. */
uint32_t procfs_format_stat(const struct proc *p, char *buf, uint32_t cap);

#endif
