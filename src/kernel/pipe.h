/* pipe — an anonymous pipe: a bounded ring buffer with one read end and one
 * write end, each an ordinary open file.
 *
 * It exists because `exec.Command` needs one: Go's child reports a failed
 * execve to its parent through a close-on-exec pipe, and reads the child's
 * output through two more. A pipe is therefore part of running a program,
 * not a convenience.
 *
 * The semantics are Linux's, and each of them is a test:
 *   - a read with the buffer empty blocks while any writer is open, and
 *     returns 0 — end of file — once the last one closes;
 *   - a write with the buffer full blocks while any reader is open, and
 *     fails with EPIPE once the last one closes;
 *   - O_NONBLOCK turns both blocks into EAGAIN;
 *   - a write of at most PIPE_BUF is all-or-nothing, so two writers never
 *     interleave within one message.
 */
#ifndef MINIKERN_KERNEL_PIPE_H
#define MINIKERN_KERNEL_PIPE_H

#include <stdint.h>

#define PIPE_BUF 4096u

struct file;

/* Creates a pipe and its two ends. flags takes O_NONBLOCK and O_CLOEXEC;
 * the caller installs the descriptors and applies O_CLOEXEC itself, because
 * that flag belongs to the descriptor rather than to the file. */
int pipe_create(uint32_t flags, struct file **read_end, struct file **write_end);

#endif
