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

/* One /dev entry. mode of 0 means an ordinary character device; a size of
 * 0 means a stream, which is what every built-in node is. */
struct dev_node {
    const char *name;
    const struct file_ops *ops;
    uint32_t rdev;
    uint32_t mode;
    uint32_t size;
};

/* Nodes registered at run time (devfs_register). Four is more than the
 * board has devices to discover. */
#define DEV_DYNAMIC_MAX 4u

static struct dev_node dynamic[DEV_DYNAMIC_MAX];
static uint32_t dynamic_count;

static const struct dev_node nodes[] = {
    {.name = "console", .ops = &console_ops, .rdev = (5u << 8) | 1u},
    {.name = "tty", .ops = &console_ops, .rdev = (5u << 8) | 0u},
    {.name = "ttyS0", .ops = &console_ops, .rdev = (DEV_TTY_MAJOR << 8) | 64u},
    {.name = "null", .ops = &null_ops, .rdev = (DEV_MEM_MAJOR << 8) | 3u},
    {.name = "zero", .ops = &zero_ops, .rdev = (DEV_MEM_MAJOR << 8) | 5u},
    {.name = "full", .ops = &full_ops, .rdev = (DEV_MEM_MAJOR << 8) | 7u},
    {.name = "random", .ops = &random_ops, .rdev = (DEV_MEM_MAJOR << 8) | 8u},
    {.name = "urandom", .ops = &random_ops, .rdev = (DEV_MEM_MAJOR << 8) | 9u},
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
    struct file *f = file_new_memory(n->ops, NULL, n->size, n->mode != 0 ? n->mode : S_IFCHR | 0666u);
    if (f != NULL) {
        f->rdev = n->rdev;
        f->flags = flags;
    }
    return f;
}

/* The node called name, built-in or registered, or NULL. */
static const struct dev_node *find_node(const char *name) {
    for (uint32_t i = 0; i < sizeof nodes / sizeof nodes[0]; i++) {
        if (str_eq(name, nodes[i].name)) {
            return &nodes[i];
        }
    }
    for (uint32_t i = 0; i < dynamic_count; i++) {
        if (str_eq(name, dynamic[i].name)) {
            return &dynamic[i];
        }
    }
    return NULL;
}

int devfs_register(const char *name, const struct file_ops *ops, uint32_t rdev, uint32_t mode, uint32_t size) {
    if (find_node(name) != NULL) {
        return KERR_EXIST;
    }
    if (dynamic_count == DEV_DYNAMIC_MAX) {
        return KERR_NOMEM;
    }
    dynamic[dynamic_count++] =
        (struct dev_node){.name = name, .ops = ops, .rdev = rdev, .mode = mode, .size = size};
    return KERR_OK;
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
    const struct dev_node *node = find_node(name);
    if (node != NULL) {
        *out = open_node(node, flags);
        return *out == NULL ? KERR_NOMEM : KERR_OK;
    }
    if (name[0] == '\0') {
        *out = file_new_memory(&null_ops, NULL, 0, S_IFDIR | 0755u);
        return *out == NULL ? KERR_NOMEM : KERR_OK;
    }
    return KERR_NOENT;
}
