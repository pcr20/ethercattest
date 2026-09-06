#ifndef ECAT_PHY_REGS_H
#define ECAT_PHY_REGS_H
/* DP83822 register tables. Every address, bit position and encoding here is
 * taken from SNLS505H Rev H (datasheet, section 8 "Register Maps") and
 * SNLA265 (EEE application report). Anything not in those documents is not
 * in this file. */
#include <stdint.h>

/* ── Direct clause-22 space: 0x00-0x1F ────────────────────────────────────── */
typedef struct {
    uint8_t     reg;
    const char *name;
    int         perishable;   /* has clear-on-read or latched bits           */
} PhyDirect;

/* Indexed by address. NULL name = not documented in SNLS505H. */
static const PhyDirect phy_direct[32] = {
    [0x00] = { 0x00, "BMCR    basic mode control",              0 },
    [0x01] = { 0x01, "BMSR    basic mode status",               1 },
    [0x02] = { 0x02, "PHYIDR1 PHY identifier 1",                0 },
    [0x03] = { 0x03, "PHYIDR2 PHY identifier 2",                0 },
    [0x04] = { 0x04, "ANAR    autoneg advertisement",           0 },
    [0x05] = { 0x05, "ANLPAR  autoneg link partner ability",    0 },
    [0x06] = { 0x06, "ANER    autoneg expansion",               1 },
    [0x07] = { 0x07, "ANNPTR  autoneg next page",               0 },
    [0x08] = { 0x08, "ANLNPTR autoneg LP ability next page",    0 },
    [0x09] = { 0x09, "CR1     control 1",                       0 },
    [0x0A] = { 0x0A, "CR2     control 2",                       0 },
    [0x0B] = { 0x0B, "CR3     control 3 (FLD criteria)",        0 },
    [0x0C] = { 0x0C, "(not documented in SNLS505H)",            0 },
    [0x0D] = { 0x0D, "REGCR   extended register control",       0 },
    [0x0E] = { 0x0E, "ADDAR   extended register data",          0 },
    [0x0F] = { 0x0F, "FLDS    fast link down status",           1 },
    [0x10] = { 0x10, "PHYSTS  PHY status",                      1 },
    [0x11] = { 0x11, "PHYSCR  PHY specific control",            0 },
    [0x12] = { 0x12, "MISR1   MII interrupt status 1",          1 },
    [0x13] = { 0x13, "MISR2   MII interrupt status 2",          1 },
    [0x14] = { 0x14, "FCSCR   false carrier sense counter",     1 },
    [0x15] = { 0x15, "RECR    receive error counter",           1 },
    [0x16] = { 0x16, "BISCR   BIST control",                    0 },
    [0x17] = { 0x17, "RCSR    RMII and status",                 0 },
    [0x18] = { 0x18, "LEDCR   LED control",                     0 },
    [0x19] = { 0x19, "PHYCR   PHY control",                     0 },
    [0x1A] = { 0x1A, "10BTSCR 10Base-Te status/control",        1 },
    [0x1B] = { 0x1B, "BICSR1  BIST control and status 1",       0 },
    [0x1C] = { 0x1C, "BICSR2  BIST control and status 2",       0 },
    [0x1D] = { 0x1D, "(not documented in SNLS505H)",            0 },
    [0x1E] = { 0x1E, "CDCR    cable diagnostic control",        0 },
    [0x1F] = { 0x1F, "PHYRCR  PHY reset control",               0 },
};

/* READ ORDER, not address order. SNLS505H Table 8-16 documents that PHYSTS
 * latch bits are cleared by reading OTHER registers:
 *     PHYSTS bit13 <- cleared by reading RECR   (0x15)
 *     PHYSTS bit12 <- cleared by reading 10BTSCR(0x1A)
 *     PHYSTS bit11 <- cleared by reading FCSCR  (0x14)
 *     PHYSTS bit8  <- cleared by reading ANER   (0x06)
 *     PHYSTS bit7  <- cleared by reading MISR1  (0x12)
 *     PHYSTS bit6  <- cleared by reading BMSR   (0x01)
 * Sweeping in address order would read BMSR and ANER before PHYSTS, so
 * PHYSTS bits 6 and 8 would always read 0 regardless of the true state.
 * PHYSTS must therefore be read FIRST, then the other perishables, then the
 * static configuration registers. Values are stored by address and displayed
 * in address order; only the ACQUISITION order differs. */
static const uint8_t phy_read_order[32] = {
    0x10,                                     /* PHYSTS first — see above    */
    0x0F, 0x12, 0x13,                         /* clear-on-read latches       */
    0x01, 0x06,                               /* latched status              */
    0x14, 0x15, 0x1A,                         /* clear-on-read counters      */
    0x00, 0x02, 0x03, 0x04, 0x05, 0x07, 0x08, /* static / config             */
    0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x11,
    0x16, 0x17, 0x18, 0x19, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
};

/* ── Extended space (via REGCR/ADDAR) ─────────────────────────────────────── */
typedef struct {
    uint8_t     devad;
    uint16_t    reg;
    uint16_t    count;        /* >1 = consecutive block, post-increment      */
    const char *name;
} PhyExt;

static const PhyExt phy_ext[] = {
    { 0x1F, 0x0467, 1,  "SOR1    strap latch-in 1 (RX_D1 = PHYAD_2/EEE_EN)" },
    { 0x1F, 0x0468, 1,  "SOR2    strap latch-in 2"                          },
    { 0x1F, 0x0421, 1,  "        analog power detect status"                },
    { 0x1F, 0x0428, 1,  "        deep power down control"                   },
    { 0x1F, 0x04D0, 1,  "        EEE configuration 1"                       },
    { 0x1F, 0x04D1, 1,  "        EEE configuration 2"                       },
    { 0x1F, 0x0155, 1,  "ALCD    active link cable diag 1"                  },
    { 0x1F, 0x0215, 1,  "ALCD    active link cable diag 2"                  },
    { 0x1F, 0x021D, 1,  "ALCD    active link cable diag 3"                  },
    { 0x1F, 0x0180, 11, "TDR     cable diag results 0x0180-0x018A"          },
    { 0x03, 0x0014, 1,  "MMD3    EEE capability"                            },
    { 0x03, 0x0016, 1,  "MMD3    wake error counter"                        },
    { 0x07, 0x003C, 1,  "MMD7    EEE advertisement"                         },
    { 0x07, 0x003D, 1,  "MMD7    EEE link partner ability"                  },
};
#define PHY_EXT_COUNT ((int)(sizeof(phy_ext)/sizeof(phy_ext[0])))

/* Is there a PHY at this MDIO address? An address with no device floats to
 * all-ones via the bus pull-up; some implementations read back all-zeros.
 * Neither is a PHY identifier. In the header so the address scan and its
 * unit test share one definition — the EVE-NET reads 0xFFFF at address 0,
 * which is exactly the case that must not be decoded as a PHY. */
static inline int phy_id_present(uint16_t id1, uint16_t id2) {
    if (id1 == 0xFFFF && id2 == 0xFFFF) return 0;
    if (id1 == 0x0000 && id2 == 0x0000) return 0;
    return 1;
}

#endif /* ECAT_PHY_REGS_H */
