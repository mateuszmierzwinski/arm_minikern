#include "kernel/devfs.h"

#include "core/errno.h"
#include "kernel/random.h"
#include "kernel/vfs.h"

#define DEV_TTY_MAJOR 4u
#define DEV_MEM_MAJOR 1u

static struct console_port console_out;
static struct console_input_port console_in;

static int32_t console_read(struct file *f, void *buf, uint32_t n) {
    (void)f;
    if (console_in.read == NULL) {
        return 0;
    }
    return console_in.read(console_in.self, buf, n);
}

static int32_t console_write(struct file *f, const void *buf, uint32_t n) {
    (void)f;
    if (console_out.write != NULL) {
        console_out.write(console_out.self, buf, n);
    }
    return (int32_t)n;
}

/* A terminal answers TCGETS; we have no termios state to give, so the
 * console says "not a terminal" and Go/TinyGo treat it as a plain stream. */
static int console_ioctl(struct file *f, uint32_t cmd, uint32_t arg) {
    (void)f;
    (void)cmd;
    (void)arg;
    return KERR_NOTTY;
}

static int32_t null_read(struct file *f, void *buf, uint32_t n) {
    (void)f;
    (void)buf;
    (void)n;
    return 0;
}

static int32_t sink_write(struct file *f, const void *buf, uint32_t n) {
    (void)f;
    (void)buf;
    return (int32_t)n;
}

static int32_t full_write(struct file *f, const void *buf, uint32_t n) {
    (void)f;
    (void)buf;
    (void)n;
    return -28;  /* ENOSPC */
}

static int32_t zero_read(struct file *f, void *buf, uint32_t n) {
    uint8_t *b = buf;
    (void)f;
    for (uint32_t i = 0; i < n; i++) {
        b[i] = 0;
    }
    return (int32_t)n;
}

static int32_t random_read(struct file *f, void *buf, uint32_t n) {
    (void)f;
    random_fill(buf, n);
    return (int32_t)n;
}

/* Output never blocks; input readiness is the tty's. */
static uint32_t console_poll(struct file *f) {
    (void)f;
    return EPOLLOUT | (console_in.ready != NULL && console_in.ready(console_in.self) ? EPOLLIN : 0u);
}

static const struct file_ops console_ops = {
    .read = console_read, .write = console_write, .ioctl = console_ioctl, .poll = console_poll,
};
static const struct file_ops null_ops = {.read = null_read, .write = sink_write};
static const struct file_ops zero_ops = {.read = zero_read, .write = sink_write};
static const struct file_ops full_ops = {.read = zero_read, .write = full_write};
static const struct file_ops random_ops = {.read = random_read, .write = sink_write};

/* One /dev entry: its name, ops and device number. */
struct dev_node {
    const char *name;
    const struct file_ops *ops;
    uint32_t rdev;
};

static const struct dev_node nodes[] = {
    {"console", &console_ops, (5u << 8) | 1u},
    {"tty", &console_ops, (5u << 8) | 0u},
    {"ttyS0", &console_ops, (DEV_TTY_MAJOR << 8) | 64u},
    {"null", &null_ops, (DEV_MEM_MAJOR << 8) | 3u},
    {"zero", &zero_ops, (DEV_MEM_MAJOR << 8) | 5u},
    {"full", &full_ops, (DEV_MEM_MAJOR << 8) | 7u},
    {"random", &random_ops, (DEV_MEM_MAJOR << 8) | 8u},
    {"urandom", &random_ops, (DEV_MEM_MAJOR << 8) | 9u},
};

static const char *const aliases[] = {"stdin", "stdout", "stderr"};

static int str_eq(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

void devfs_set_console(const struct console_port *out, const struct console_input_port *in) {
    console_out = *out;
    console_in = *in;
}

static struct file *open_node(const struct dev_node *n, uint32_t flags) {
    struct file *f = file_new_memory(n->ops, NULL, 0, S_IFCHR | 0666u);
    if (f != NULL) {
        f->rdev = n->rdev;
        f->flags = flags;
    }
    return f;
}

struct file *devfs_console_file(uint32_t flags) {
    return open_node(&nodes[0], flags);
}

int devfs_open(const char *name, uint32_t flags, struct file **out, int *alias_fd) {
    for (uint32_t i = 0; i < sizeof aliases / sizeof aliases[0]; i++) {
        if (str_eq(name, aliases[i])) {
            *alias_fd = (int)i;
            return VFS_ALIAS;
        }
    }
    for (uint32_t i = 0; i < sizeof nodes / sizeof nodes[0]; i++) {
        if (str_eq(name, nodes[i].name)) {
            *out = open_node(&nodes[i], flags);
            return *out == NULL ? KERR_NOMEM : KERR_OK;
        }
    }
    if (name[0] == '\0') {
        *out = file_new_memory(&null_ops, NULL, 0, S_IFDIR | 0755u);
        return *out == NULL ? KERR_NOMEM : KERR_OK;
    }
    return KERR_NOENT;
}
