/* exec — starts a program: a static ELF from the root filesystem becomes a
 * new process with one thread, ready to run, or replaces the image of the
 * process that is already running (execve). */
#ifndef MINIKERN_KERNEL_EXEC_H
#define MINIKERN_KERNEL_EXEC_H

#include <stdint.h>

struct proc;

/* Loads path with argv/envp (argc/envc entries) and makes its first thread
 * runnable, with descriptors 0-2 on the console. Returns KERR_OK or a
 * negative errno; *out is the new process. */
int exec_start(const char *path, const char *const *argv, uint32_t argc, const char *const *envp, uint32_t envc,
               struct proc **out);

struct trapframe;

/* Replaces the calling process's image, as execve does: a fresh address
 * space holding path's segments, stack and signal page, descriptors kept
 * except those marked close-on-exec, handled signals back to their default,
 * and tf rewritten so the return to user mode lands on the new entry point.
 *
 * **argv, envp and path must already be in kernel memory**: the address
 * space they came from stops existing part way through this, and a pointer
 * into it is then a pointer into a freed page table. That is why the copying
 * belongs to the caller (sys_proc.c) and not here.
 *
 * On failure nothing has changed and the caller returns the error to a
 * process that is still running its old image — the one property that makes
 * a failed exec harmless. That is why the image is built before the other
 * threads are ended: a multi-threaded caller loses its siblings (the new
 * image must not share an address space with threads running the old one),
 * and that cannot be undone, so it happens only once the exec is certain to
 * go through. A sibling that will not stop within two seconds gives
 * KERR_AGAIN. */
int exec_replace(const char *path, const char *const *argv, uint32_t argc, const char *const *envp, uint32_t envc,
                 struct trapframe *tf);

#endif
