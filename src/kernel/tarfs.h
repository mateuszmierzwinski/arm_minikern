/* tarfs — the read-only root filesystem: a ustar archive in RAM (the
 * initrd), served in place. */
#ifndef MINIKERN_KERNEL_TARFS_H
#define MINIKERN_KERNEL_TARFS_H

#include <stdint.h>

struct file;

void tarfs_mount(const uint8_t *archive, uint32_t len);

/* Opens a file or directory of the archive; writes are refused (EACCES),
 * a symlink is followed once. */
int tarfs_open(const char *path, uint32_t flags, struct file **out);

const uint8_t *tarfs_archive(uint32_t *len);

#endif
