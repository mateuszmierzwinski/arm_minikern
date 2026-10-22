/* exec — starts a program: a static ELF from the root filesystem becomes a
 * new process with one thread, ready to run. Only the first process (init)
 * is started this way for now; execve() is a later step. */
#ifndef MINIKERN_KERNEL_EXEC_H
#define MINIKERN_KERNEL_EXEC_H

#include <stdint.h>

struct proc;

/* Loads path with argv/envp (argc/envc entries) and makes its first thread
 * runnable, with descriptors 0-2 on the console. Returns KERR_OK or a
 * negative errno; *out is the new process. */
int exec_start(const char *path, const char *const *argv, uint32_t argc, const char *const *envp, uint32_t envc,
               struct proc **out);

#endif
