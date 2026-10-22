/* errno.h — the kernel's error codes. The values are Linux's (asm-generic
 * errno-base.h), because the same numbers are returned to user space as
 * -errno by the syscall layer: a Go binary reads them unchanged. */
#ifndef MINIKERN_CORE_ERRNO_H
#define MINIKERN_CORE_ERRNO_H

#define KERR_OK      0
#define KERR_PERM    (-1)   /* operation not permitted */
#define KERR_NOENT   (-2)   /* no such entry */
#define KERR_SRCH    (-3)   /* no such process or thread */
#define KERR_INTR    (-4)   /* interrupted by a signal */
#define KERR_IO      (-5)
#define KERR_NOEXEC  (-8)   /* not an executable format we run */
#define KERR_BADF    (-9)   /* not an open file descriptor */
#define KERR_AGAIN   (-11)  /* try again (also EWOULDBLOCK) */
#define KERR_NOMEM   (-12)  /* out of memory */
#define KERR_ACCES   (-13)  /* permission denied */
#define KERR_FAULT   (-14)  /* bad address */
#define KERR_EXIST   (-17)
#define KERR_NOTDIR  (-20)
#define KERR_ISDIR   (-21)
#define KERR_INVAL   (-22)  /* invalid argument */
#define KERR_MFILE   (-24)  /* too many open files */
#define KERR_NOTTY   (-25)  /* not a terminal */
#define KERR_SPIPE   (-29)  /* not seekable */
#define KERR_RANGE   (-34)
#define KERR_NAMETOOLONG (-36)
#define KERR_NOSYS   (-38)  /* not implemented */
#define KERR_TIMEDOUT (-110)

#endif
