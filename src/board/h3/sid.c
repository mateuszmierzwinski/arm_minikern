#include "board/h3/sid.h"

#include "arch/arm/layout.h"
#include "board/h3/h3.h"
#include "core/errno.h"

#define SID_PRCTL      0x40u
#define SID_RDKEY      0x60u
#define PRCTL_OP_LOCK  0xac00u      /* the "operation" key the block requires */
#define PRCTL_READ     (1u << 1)    /* start a read; clears when done */
#define PRCTL_INDEX_SHIFT 16u
#define SID_SPINS      100000u

static volatile uint32_t *reg(uint32_t off) {
    return (volatile uint32_t *)io_virt(H3_SID_BASE + off);
}

static int read_word(uint32_t byte_offset, uint32_t *out) {
    *reg(SID_PRCTL) = (byte_offset << PRCTL_INDEX_SHIFT) | PRCTL_OP_LOCK | PRCTL_READ;
    for (uint32_t i = 0; i < SID_SPINS; i++) {
        if ((*reg(SID_PRCTL) & PRCTL_READ) == 0) {
            *out = *reg(SID_RDKEY);
            *reg(SID_PRCTL) = 0;
            return KERR_OK;
        }
    }
    return KERR_TIMEDOUT;
}

int h3_sid_read(uint32_t id[4]) {
    for (uint32_t i = 0; i < 4; i++) {
        int rc = read_word(i * 4u, &id[i]);
        if (rc != KERR_OK) {
            return rc;
        }
    }
    return KERR_OK;
}
