#include "kernel/genfs.h"

#include <stddef.h>

#include "core/errno.h"
#include "kernel/mem.h"
#include "kernel/net.h"
#include "kernel/vfs.h"
#include "lib/kfmt.h"

/* How big a generated file may be. They are a line or two. */
#define GENFS_MAX 256u

/* One generated file: where it appears, and what renders it. render writes
 * at most cap bytes and returns how many it wrote. */
struct genfile {
    const char *dir;
    const char *name;
    const char *path;
    uint32_t (*render)(char *buf, uint32_t cap);
};

/* resolv.conf, from the DHCP lease. A board with no lease, or a lease with
 * no resolver in it, gets an empty file rather than a made-up address:
 * Go then reports that it has no name server, which is the truth. */
static uint32_t render_resolv_conf(char *buf, uint32_t cap) {
    struct netif *nif = knet_interface();
    char ip[16];
    if (nif == NULL || nif->dns == 0) {
        return 0;
    }
    return (uint32_t)kfmt_snprintf(buf, cap, "nameserver %s\n", ip4_format(nif->dns, ip));
}

/* An empty hosts file, so a resolver that reads it finds nothing rather
 * than failing to open it. "files" before "dns" is Go's default order, and
 * an absent file and an empty one are not the same thing to it. */
static uint32_t render_hosts(char *buf, uint32_t cap) {
    return (uint32_t)kfmt_snprintf(buf, cap, "127.0.0.1 localhost\n");
}

static const struct genfile FILES[] = {
    {"/etc", "resolv.conf", "/etc/resolv.conf", render_resolv_conf},
    {"/etc", "hosts", "/etc/hosts", render_hosts},
};

#define GENFS_COUNT (sizeof FILES / sizeof FILES[0])

/* A generated file is memory-backed and read-only, like procfs's. */
static const struct file_ops genfs_ops = {.read = file_memory_read};

static bool path_equal(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

uint32_t genfs_count(void) {
    return GENFS_COUNT;
}

const char *genfs_name(uint32_t index, const char **dir) {
    if (index >= GENFS_COUNT) {
        return NULL;
    }
    *dir = FILES[index].dir;
    return FILES[index].name;
}

/* Renders one file into memory the file owns, so it is freed with it. */
static int open_generated(const struct genfile *g, uint32_t flags, struct file **out) {
    char *text = kmalloc(GENFS_MAX);
    uint32_t len;
    if (text == NULL) {
        return KERR_NOMEM;
    }
    len = g->render(text, GENFS_MAX);
    *out = file_new_memory(&genfs_ops, (const uint8_t *)text, len, S_IFREG | 0444u);
    if (*out == NULL) {
        kfree(text);
        return KERR_NOMEM;
    }
    (*out)->owned = text;
    (*out)->flags = flags;
    return KERR_OK;
}

bool genfs_open(const char *path, uint32_t flags, struct file **out, int *rc) {
    for (uint32_t i = 0; i < GENFS_COUNT; i++) {
        if (path_equal(path, FILES[i].path)) {
            *rc = open_generated(&FILES[i], flags, out);
            return true;
        }
    }
    return false;
}
