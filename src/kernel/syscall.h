/* syscall — the Linux ARM EABI system call interface.
 *
 * svc #0 with the number in r7 and arguments in r0-r6; the result goes back
 * in r0, a negative errno on failure (-4095..-1). A 64-bit argument takes
 * an even/odd register pair. Numbers 0xf0000+ are ARM's private calls
 * (set_tls, cacheflush). The table is built once at boot from each
 * domain's entries (sys_*.c); a number nobody claims is -ENOSYS and logged
 * once, which is how the next syscall a program needs is found. */
#ifndef MINIKERN_KERNEL_SYSCALL_H
#define MINIKERN_KERNEL_SYSCALL_H

#include <stdint.h>

#include "arch/arm/trap.h"

#define SYSCALL_MAX 460u
#define SYSCALL_ARM_BASE 0xf0000u
#define SYSCALL_ARM_MAX 8u

/* One system call: its number, its name for the log, and its body. The
 * body reads its arguments from tf->r[] and returns r0's new value. */
struct syscall_entry {
    uint32_t nr;
    const char *name;
    int32_t (*call)(struct trapframe *tf);
};

/* Each domain's table, terminated by an entry with call == NULL. */
extern const struct syscall_entry sys_proc_table[];
extern const struct syscall_entry sys_mem_table[];
extern const struct syscall_entry sys_time_table[];
extern const struct syscall_entry sys_signal_table[];
extern const struct syscall_entry sys_file_table[];
extern const struct syscall_entry sys_futex_table[];
extern const struct syscall_entry sys_dir_table[];
extern const struct syscall_entry sys_poll_table[];
extern const struct syscall_entry sys_socket_table[];

/* The open file at descriptor fd of the calling process, or NULL. */
struct file *fd_file(int32_t fd);

void syscall_init(void);

/* The trap layer's syscall hook. */
void syscall_dispatch(struct trapframe *tf);

/* Logs every syscall made (minikern.strace=1). */
void syscall_set_trace(int on);

#endif
