/* sid — the H3's Security ID block: the chip's 128-bit unique ID. It is
 * read through the key-read interface (PRCTL/RDKEY), which both silicon
 * and QEMU implement; the efuse words are not reliably mapped for direct
 * reads (QEMU does not map them at all). */
#ifndef MINIKERN_BOARD_H3_SID_H
#define MINIKERN_BOARD_H3_SID_H

#include <stdint.h>

/* The four ID words. KERR_OK, or KERR_TIMEDOUT if the block never answers. */
int h3_sid_read(uint32_t id[4]);

#endif
