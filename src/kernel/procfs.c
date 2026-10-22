#include "kernel/procfs.h"

#include "core/errno.h"
#include "kernel/mem.h"
#include "kernel/proc.h"
#include "kernel/vfs.h"
#include "lib/kfmt.h"

#define STAT_CAP 512u
#define RLIM_INFINITY64 0xffffffffffffffffull

static const struct file_ops proc_file_ops = {.read = file_memory_read};

/* Fields 1-52 of proc(5)'s stat line; only pid, comm, state, num_threads,
 * vsize-ish values and startstack (28) carry information here. */
uint32_t procfs_format_stat(const struct proc *p, char *buf, uint32_t cap) {
    return (uint32_t)kfmt_snprintf(buf, cap,
                                   "%u (%s) R 0 %u %u 0 -1 4194560 0 0 0 0 0 0 0 0 20 0 %u 0 0 0 0 %llu "
                                   "65536 65536 %u 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n",
                                   p->pid, p->name, p->pid, p->pid, p->nthreads, RLIM_INFINITY64,
                                   p->start_stack);
}

static int open_generated(char *text, uint32_t len, struct file **out) {
    *out = file_new_memory(&proc_file_ops, (const uint8_t *)text, len, S_IFREG | 0444u);
    if (*out == NULL) {
        kfree(text);
        return KERR_NOMEM;
    }
    (*out)->owned = text;
    return KERR_OK;
}

static int str_eq(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

int procfs_open(const char *name, uint32_t flags, struct file **out) {
    struct proc *p = proc_current();
    char *text;
    if ((flags & O_ACCMODE) != O_RDONLY) {
        return KERR_ACCES;
    }
    if (p == NULL || !str_eq(name, "self/stat")) {
        return KERR_NOENT;
    }
    text = kmalloc(STAT_CAP);
    if (text == NULL) {
        return KERR_NOMEM;
    }
    return open_generated(text, procfs_format_stat(p, text, STAT_CAP), out);
}
