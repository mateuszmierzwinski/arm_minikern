#include "lib/div64.h"

uint64_t div64_u(uint64_t n, uint64_t d, uint64_t *rem) {
    uint64_t q = 0, r = 0;
    if (d == 0) {
        *rem = n;
        return 0;
    }
    for (int bit = 63; bit >= 0; bit--) {
        r = (r << 1) | ((n >> bit) & 1u);
        if (r >= d) {
            r -= d;
            q |= 1ull << bit;
        }
    }
    *rem = r;
    return q;
}

int64_t div64_s(int64_t n, int64_t d, int64_t *rem) {
    uint64_t un = n < 0 ? 0u - (uint64_t)n : (uint64_t)n;
    uint64_t ud = d < 0 ? 0u - (uint64_t)d : (uint64_t)d;
    uint64_t ur;
    uint64_t uq = div64_u(un, ud, &ur);
    *rem = n < 0 ? (int64_t)(0u - ur) : (int64_t)ur;
    return (n < 0) != (d < 0) ? (int64_t)(0u - uq) : (int64_t)uq;
}
