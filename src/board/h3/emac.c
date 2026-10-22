#include "board/h3/emac.h"

#include "arch/arm/layout.h"
#include "board/h3/h3.h"
#include "core/errno.h"
#include "kernel/dma.h"
#include "kernel/irq.h"
#include "kernel/time.h"

/* EMAC registers (offsets from H3_EMAC_BASE). */
#define BASIC_CTL0      0x00u
#define BASIC_CTL1      0x04u
#define INT_STA         0x08u
#define INT_EN          0x0cu
#define TX_CTL0         0x10u
#define TX_CTL1         0x14u
#define TX_DESC_LIST    0x20u
#define RX_CTL0         0x24u
#define RX_CTL1         0x28u
#define RX_DESC_LIST    0x34u
#define FRM_FLT         0x38u
#define MII_CMD         0x48u
#define MII_DATA        0x4cu
#define ADDR_HIGH0      0x50u
#define ADDR_LOW0       0x54u

#define CTL0_FULL_DUPLEX (1u << 0)
#define CTL0_SPEED_100   (3u << 2)
#define CTL1_SOFT_RESET  (1u << 0)
#define CTL1_BURST_8     (8u << 24)
#define INT_RX           (1u << 8)
#define INT_RX_BUF_UA    (1u << 9)
#define TX_CTL0_EN       (1u << 31)
#define TX_CTL1_MD       (1u << 1)   /* store and forward */
#define TX_CTL1_DMA_EN   (1u << 30)
#define TX_CTL1_DMA_START (1u << 31)
#define RX_CTL0_EN       (1u << 31)
#define RX_CTL1_MD       (1u << 1)
#define RX_CTL1_DMA_EN   (1u << 30)
#define RX_CTL1_DMA_START (1u << 31)
#define MII_BUSY         (1u << 0)
#define MII_WRITE        (1u << 1)
#define MII_REG_SHIFT    4u
#define MII_ADDR_SHIFT   12u
#define MII_MDC_DIV_128  (3u << 20)

/* Descriptor words (four per descriptor, chained in a ring). */
#define DESC_OWN         (1u << 31)  /* status: the DMA engine owns it */
#define DESC_SIZE_MASK   0x7ffu      /* ctl: buffer size */
#define TX_DESC_FIRST    (1u << 29)
#define TX_DESC_LAST     (1u << 30)
#define RX_DESC_FIRST    (1u << 9)
#define RX_DESC_LAST     (1u << 8)
#define RX_DESC_ERRORS   ((1u << 14) | (1u << 7) | (1u << 4) | (1u << 1) | (1u << 0))
#define RX_DESC_LEN_SHIFT 16u
#define RX_DESC_LEN_MASK 0x3fffu
#define FCS_LEN          4u

/* Clock control and system control (the internal-PHY selection). */
#define CCU_BUS_GATE0    0x060u
#define CCU_BUS_GATE4    0x070u
#define CCU_BUS_RST0     0x2c0u
#define CCU_BUS_RST2     0x2c8u
#define CCU_EMAC_BIT     (1u << 17)
#define CCU_EPHY_BIT     (1u << 0)
#define CCU_EPHY_RST_BIT (1u << 2)
/* The system-control register that decides whether there is a PHY at all.
 *
 * Every field here is **corroborated** against an implementation that runs
 * on this hardware (2026-10-22, CLAUDE.md §3): the PHY address at bit 20,
 * the clock selection at bit 18 (1 = 24 MHz, which is what the internal PHY
 * wants), the internal-PHY select at bit 15, and bit 16 as *shutdown* —
 * active high, so leaving it clear is what powers the PHY up. The register
 * is written whole rather than read-modify-written, which is what that
 * implementation does on this path too; the MII/RGMII selection bits in the
 * low end are then 0, which is the MII setting the internal PHY needs.
 *
 * What remains unverified is not the value but its **effect**: QEMU stubs
 * this block, so the write is dropped and a readback gives the reset value
 * (`0x58000` — which has shutdown *set*, so writing this register is the
 * difference between a PHY and nothing). Only a board can show that it
 * takes. */
#define SYSCON_EPHY_CLK  0x30u
#define EPHY_ADDR_SHIFT  20u
#define EPHY_CLK_24M     (1u << 18)   /* 1: 24 MHz, 0: 25 MHz */
#define EPHY_SELECT_INT  (1u << 15)   /* 1: internal PHY, 0: external */
#define EPHY_SHUTDOWN    (1u << 16)   /* 1: shutdown — left clear on purpose */

/* PHY registers (IEEE 802.3 clause 22). */
#define MII_BMCR         0u
#define MII_BMSR         1u
#define MII_PHYSID1      2u
#define MII_PHYSID2      3u
#define MII_LPA          5u
#define BMCR_RESET       (1u << 15)
#define BMCR_ANENABLE    (1u << 12)
#define BMCR_ANRESTART   (1u << 9)
#define BMSR_LINK        (1u << 2)
#define LPA_100FULL      (1u << 8)
#define LPA_100HALF      (1u << 7)
#define LPA_10FULL       (1u << 6)

#define INTERNAL_PHY_ADDR 1u
#define RX_RING          64u   /* 64 frames of slack: a netboot window arrives as one burst */
#define TX_RING          8u
#define BUF_SIZE         1600u       /* a full frame + FCS, under DESC_SIZE_MASK */
#define RESET_SPINS      1000000u
#define LINK_WAIT_MS     4000u

struct desc {
    volatile uint32_t status;
    volatile uint32_t ctl;
    volatile uint32_t buf;
    volatile uint32_t next;
};

struct emac {
    uintptr_t base;
    struct desc *rx;
    struct desc *tx;
    uint8_t *rx_buf;
    uint8_t *tx_buf;
    uint32_t rx_buf_phys;
    uint32_t tx_buf_phys;
    uint32_t rx_next;
    uint32_t tx_next;
    struct frame_sink sink;
};

static struct emac emac;

static uint32_t rd(uint32_t off) {
    return *(volatile uint32_t *)(emac.base + off);
}

static void wr(uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(emac.base + off) = v;
}

static void io_set(uint32_t phys, uint32_t bits) {
    volatile uint32_t *r = (volatile uint32_t *)io_virt(phys);
    *r |= bits;
}

/* Gates and resets for the MAC and the internal PHY, and the PHY select. */
static void power_on(void) {
    io_set(H3_CCU_BASE + CCU_BUS_GATE0, CCU_EMAC_BIT);
    io_set(H3_CCU_BASE + CCU_BUS_GATE4, CCU_EPHY_BIT);
    io_set(H3_CCU_BASE + CCU_BUS_RST0, CCU_EMAC_BIT);
    io_set(H3_CCU_BASE + CCU_BUS_RST2, CCU_EPHY_RST_BIT);
    /* EPHY_SHUTDOWN is deliberately absent: the reset value has it set, and
     * clearing it is what powers the internal PHY up. */
    *(volatile uint32_t *)io_virt(H3_SYSCON_BASE + SYSCON_EPHY_CLK) =
        (INTERNAL_PHY_ADDR << EPHY_ADDR_SHIFT) | EPHY_CLK_24M | EPHY_SELECT_INT;
}

static int soft_reset(void) {
    wr(BASIC_CTL1, rd(BASIC_CTL1) | CTL1_SOFT_RESET);
    for (uint32_t i = 0; i < RESET_SPINS; i++) {
        if ((rd(BASIC_CTL1) & CTL1_SOFT_RESET) == 0) {
            wr(BASIC_CTL1, CTL1_BURST_8);
            return KERR_OK;
        }
    }
    return KERR_IO;
}

static int mdio_wait(void) {
    for (uint32_t i = 0; i < RESET_SPINS; i++) {
        if ((rd(MII_CMD) & MII_BUSY) == 0) {
            return KERR_OK;
        }
    }
    return KERR_IO;
}

static int mdio_read(uint32_t addr, uint32_t reg, uint32_t *v) {
    wr(MII_CMD, MII_MDC_DIV_128 | (addr << MII_ADDR_SHIFT) | (reg << MII_REG_SHIFT) | MII_BUSY);
    if (mdio_wait() != KERR_OK) {
        return KERR_IO;
    }
    *v = rd(MII_DATA) & 0xffffu;
    return KERR_OK;
}

static int mdio_write(uint32_t addr, uint32_t reg, uint32_t v) {
    wr(MII_DATA, v);
    wr(MII_CMD, MII_MDC_DIV_128 | (addr << MII_ADDR_SHIFT) | (reg << MII_REG_SHIFT) | MII_WRITE | MII_BUSY);
    return mdio_wait();
}

/* Builds both rings: every RX descriptor owned by the DMA with a buffer,
 * every TX descriptor ours. */
static int build_rings(void) {
    uint32_t rx_phys, tx_phys;
    emac.rx = dma_alloc(RX_RING * sizeof(struct desc), 64, &rx_phys);
    emac.tx = dma_alloc(TX_RING * sizeof(struct desc), 64, &tx_phys);
    emac.rx_buf = dma_alloc(RX_RING * BUF_SIZE, 64, &emac.rx_buf_phys);
    emac.tx_buf = dma_alloc(TX_RING * BUF_SIZE, 64, &emac.tx_buf_phys);
    if (emac.rx == NULL || emac.tx == NULL || emac.rx_buf == NULL || emac.tx_buf == NULL) {
        return KERR_NOMEM;
    }
    for (uint32_t i = 0; i < RX_RING; i++) {
        emac.rx[i] = (struct desc){.status = DESC_OWN, .ctl = BUF_SIZE, .buf = emac.rx_buf_phys + i * BUF_SIZE,
                                   .next = rx_phys + ((i + 1u) % RX_RING) * (uint32_t)sizeof(struct desc)};
    }
    for (uint32_t i = 0; i < TX_RING; i++) {
        emac.tx[i] = (struct desc){.buf = emac.tx_buf_phys + i * BUF_SIZE,
                                   .next = tx_phys + ((i + 1u) % TX_RING) * (uint32_t)sizeof(struct desc)};
    }
    wr(RX_DESC_LIST, rx_phys);
    wr(TX_DESC_LIST, tx_phys);
    return KERR_OK;
}

static void set_mac(const uint8_t mac[6]) {
    wr(ADDR_HIGH0, (uint32_t)mac[4] | ((uint32_t)mac[5] << 8));
    wr(ADDR_LOW0, (uint32_t)mac[0] | ((uint32_t)mac[1] << 8) | ((uint32_t)mac[2] << 16) | ((uint32_t)mac[3] << 24));
}

/* PHY reset and autonegotiation; waits for the link (in ms of the tick). */
static int bring_up_phy(struct emac_link *link) {
    uint32_t id1, id2, bmsr = 0, lpa = 0;
    uint64_t deadline = time_monotonic_ns() + (uint64_t)LINK_WAIT_MS * 1000000u;
    if (mdio_read(INTERNAL_PHY_ADDR, MII_PHYSID1, &id1) != KERR_OK ||
        mdio_read(INTERNAL_PHY_ADDR, MII_PHYSID2, &id2) != KERR_OK || id1 == 0xffffu) {
        return KERR_IO;
    }
    (void)mdio_write(INTERNAL_PHY_ADDR, MII_BMCR, BMCR_RESET | BMCR_ANENABLE | BMCR_ANRESTART);
    while ((bmsr & BMSR_LINK) == 0 && time_monotonic_ns() < deadline) {
        (void)mdio_read(INTERNAL_PHY_ADDR, MII_BMSR, &bmsr);
    }
    (void)mdio_read(INTERNAL_PHY_ADDR, MII_LPA, &lpa);
    *link = (struct emac_link){.phy_id = (id1 << 16) | id2, .phy_addr = INTERNAL_PHY_ADDR,
                               .up = (bmsr & BMSR_LINK) != 0, .speed_100 = (lpa & (LPA_100FULL | LPA_100HALF)) != 0,
                               .full_duplex = (lpa & (LPA_100FULL | LPA_10FULL)) != 0};
    return link->up ? KERR_OK : KERR_TIMEDOUT;
}

/* Hands every completed RX descriptor's frame to the sink and gives the
 * descriptor back to the DMA. */
static void receive_all(void) {
    for (uint32_t n = 0; n < RX_RING; n++) {
        struct desc *d = &emac.rx[emac.rx_next];
        uint32_t st = d->status, len;
        if (st & DESC_OWN) {
            break;
        }
        len = (st >> RX_DESC_LEN_SHIFT) & RX_DESC_LEN_MASK;
        if ((st & (RX_DESC_FIRST | RX_DESC_LAST)) == (RX_DESC_FIRST | RX_DESC_LAST) && (st & RX_DESC_ERRORS) == 0 &&
            len > FCS_LEN && len <= BUF_SIZE && emac.sink.frame != NULL) {
            emac.sink.frame(emac.sink.self, emac.rx_buf + emac.rx_next * BUF_SIZE, len - FCS_LEN);
        }
        d->ctl = BUF_SIZE;
        d->status = DESC_OWN;
        emac.rx_next = (emac.rx_next + 1u) % RX_RING;
    }
    wr(RX_CTL1, RX_CTL1_DMA_EN | RX_CTL1_DMA_START | RX_CTL1_MD);
}

static void emac_irq(void *self) {
    uint32_t st = rd(INT_STA);
    (void)self;
    wr(INT_STA, st);
    if (st & (INT_RX | INT_RX_BUF_UA)) {
        receive_all();
    }
}

static int emac_send(void *self, const uint8_t *frame, uint32_t len) {
    struct desc *d = &emac.tx[emac.tx_next];
    uint8_t *buf = emac.tx_buf + emac.tx_next * BUF_SIZE;
    (void)self;
    if (len > BUF_SIZE - FCS_LEN) {
        return KERR_INVAL;
    }
    if (d->status & DESC_OWN) {
        return KERR_AGAIN;
    }
    for (uint32_t i = 0; i < len; i++) {
        buf[i] = frame[i];
    }
    d->ctl = TX_DESC_FIRST | TX_DESC_LAST | len;
    __asm__ volatile("dsb" ::: "memory");
    d->status = DESC_OWN;
    __asm__ volatile("dsb" ::: "memory");
    wr(TX_CTL1, TX_CTL1_MD | TX_CTL1_DMA_EN | TX_CTL1_DMA_START);
    emac.tx_next = (emac.tx_next + 1u) % TX_RING;
    return KERR_OK;
}

struct netdev_port h3_emac_netdev(void) {
    return (struct netdev_port){.send = emac_send, .self = &emac};
}

/* Receive and transmit on, at the link's speed and duplex. */
static void start_dma(const struct emac_link *link) {
    wr(BASIC_CTL0, (link->full_duplex ? CTL0_FULL_DUPLEX : 0u) | (link->speed_100 ? CTL0_SPEED_100 : 0u));
    wr(FRM_FLT, 0);
    wr(RX_CTL1, RX_CTL1_DMA_EN | RX_CTL1_DMA_START | RX_CTL1_MD);
    wr(RX_CTL0, RX_CTL0_EN);
    wr(TX_CTL1, TX_CTL1_MD | TX_CTL1_DMA_EN);
    wr(TX_CTL0, TX_CTL0_EN);
    wr(INT_STA, ~0u);
    wr(INT_EN, INT_RX | INT_RX_BUF_UA);
}

int h3_emac_init(const uint8_t mac[6], const struct frame_sink *sink, struct emac_link *link) {
    static const struct irq_handler handler = {.handle = emac_irq};
    int rc;
    emac = (struct emac){.base = io_virt(H3_EMAC_BASE), .sink = *sink};
    power_on();
    rc = soft_reset();
    if (rc == KERR_OK) {
        rc = build_rings();
    }
    if (rc == KERR_OK) {
        set_mac(mac);
        rc = bring_up_phy(link);
    }
    if (rc != KERR_OK) {
        return rc;
    }
    start_dma(link);
    return irq_register(H3_IRQ_EMAC, &handler);
}

void h3_emac_mac_from_id(const uint32_t id[4], uint8_t mac[6]) {
    uint32_t a = id[0] ^ id[2], b = id[1] ^ id[3];
    mac[0] = 0x02;  /* locally administered, unicast */
    mac[1] = (uint8_t)(a >> 24) ^ (uint8_t)b;
    mac[2] = (uint8_t)(a >> 16) ^ (uint8_t)(b >> 8);
    mac[3] = (uint8_t)(a >> 8);
    mac[4] = (uint8_t)a;
    mac[5] = (uint8_t)(b >> 16) ^ (uint8_t)(b >> 24);
}
