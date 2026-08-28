#ifndef ECAT_ESCMII_H
#define ECAT_ESCMII_H
/* ── PHY register access over the ESC MII management interface ──────────────
 *
 * *** THIS MODULE WRITES TO THE SLAVE. ***
 *
 * It is deliberately kept OUT of ecat_regdump: that tool links escreg.o only,
 * which implements APRD and nothing else, so it remains read-only by
 * construction (tests/t_escframe.c T7). Only ecat_phy links this module.
 *
 * IMPORTANT — even READING a PHY register writes to the ESC. The MII
 * management interface is indirect: you must write the PHY address and PHY
 * register selector (0x0512/0x0513) and a command (0x0510) before the result
 * can be read back from the data register (0x0514). Those writes target the
 * ESC's own MII block, not the drive application, but if the drive firmware
 * holds MII via PDI they contend. See the arbitration note below.
 *
 * ESC MII register block (ETG.1000.6), all little-endian:
 *   0x0510  MII management control/status (16-bit)
 *   0x0512  MII PHY address        (8-bit)   \ written together as one
 *   0x0513  MII PHY register addr  (8-bit)   / 16-bit LE word at 0x0512
 *   0x0514  MII PHY data           (16-bit)
 *   0x0516  MII ECAT access state  (8-bit)
 *   0x0517  MII PDI  access state  (8-bit)
 *
 * Because 0x0512/0x0513 are consecutive bytes, a 16-bit LE write of value V to
 * 0x0512 puts (V & 0xFF) at 0x0512 = PHY ADDRESS and (V >> 8) at 0x0513 = PHY
 * REGISTER. So V = (phy_reg << 8) | phy_addr. This matches the vendor
 * procedure exactly: 0x0B00 selects PHY 0 register 0x0B, 0x0F00 selects
 * PHY 0 register 0x0F (FLDS).
 *
 * Command word written to 0x0510. Verified against Beckhoff ESC Section II
 * Register Description v3.3 §2.12.1: bits[10:8] are a 3-bit command field
 * (001=read, 010=write) and bit 0 is write-enable, which SELF-CLEARS at the
 * SOF of the next frame — so write-enable and the command must go in the SAME
 * write, which is what these values do:
 *   0x0100  read  = command field bit 8
 *   0x0201  write = write-enable bit 0 + command field bit 9
 * Busy is bit 15 of 0x0510; poll until it clears.
 *
 * Bit assignments for 0x0510 beyond busy/read/write are decoded for display
 * only and are labelled UNVERIFIED — confirm against ETG.1000.6 and the ESC
 * datasheet before relying on them. */
#include "escreg.h"

/* APWR (auto-increment physical write). Deliberately defined HERE and not in
 * ecat_common.h, so that no translation unit which does not include this
 * header can emit a write command. */
#define ECAT_CMD_APWR        0x02

/* 0x0510 command words, exactly as in the vendor procedure. */
#define MII_CMD_READ         0x0100
#define MII_CMD_WRITE        0x0201
#define MII_STAT_BUSY        0x8000   /* bit 15 */
#define MII_STAT_CMD_ERR     0x4000   /* bit 14 — command error              */
#define MII_STAT_READ_ERR    0x2000   /* bit 13 — read error                  */
#define MII_CTRL_PDI_CTRL    0x0002   /* bit  1 — MI controllable by PDI      */

#define ESC_MII_CTRL         0x0510
#define ESC_MII_PHYADR       0x0512
#define ESC_MII_PHYREG       0x0513
#define ESC_MII_DATA         0x0514
#define ESC_MII_DATA_HI      0x0515
#define ESC_MII_ECAT_ACC     0x0516
#define ESC_MII_PDI_ACC      0x0517

/* Compose the 16-bit word written to 0x0512 to select a PHY register. */
static inline uint16_t mii_sel(uint8_t phy_addr, uint8_t phy_reg) {
    return (uint16_t)(((uint16_t)phy_reg << 8) | phy_addr);
}

/* ── Raw ESC access ─────────────────────────────────────────────────────── */
/* Build a single-datagram APWR frame. Exposed for unit tests. */
int esc_build_write_frame(uint8_t *buf, int buflen, const uint8_t *src_mac,
                          uint16_t position, uint8_t idx,
                          uint16_t addr, const uint8_t *data, uint16_t len);

/* APWR len bytes to ESC register addr. Returns WKC (>=1 ok), or -1. */
int esc_write(EscCtx *ctx, uint16_t addr, const uint8_t *data, uint16_t len);
int esc_write16(EscCtx *ctx, uint16_t addr, uint16_t value);
int esc_read16(EscCtx *ctx, uint16_t addr, uint16_t *value);
int esc_read8(EscCtx *ctx, uint16_t addr, uint8_t *value);

/* ── MII management ─────────────────────────────────────────────────────── */
/* Poll 0x0510 until busy clears. Returns the final status word, or -1. */
int mii_wait_idle(EscCtx *ctx, int timeout_ms, uint16_t *status_out);

/* Read one PHY register. WRITES 0x0512 and 0x0510 on the ESC. Returns 0 on
 * success and stores the 16-bit value; also returns the raw status word. */
int mii_read_phy(EscCtx *ctx, uint8_t phy_addr, uint8_t phy_reg,
                 uint16_t *value, uint16_t *status_out);

/* Write one PHY register. WRITES THE PHY ITSELF. Returns 0 on success.
 * If verify is non-zero the value is read back and compared. */
int mii_write_phy(EscCtx *ctx, uint8_t phy_addr, uint8_t phy_reg,
                  uint16_t value, int verify, uint16_t *readback);

/* ── Extended / MMD register access (REGCR 0x0D + ADDAR 0x0E) ───────────────
 * Direct clause-22 space is only 32 registers (0x00-0x1F). Everything else on
 * the DP83822 — TDR results 0x0180-0x018A, ALCD, the strap latch-in registers
 * 0x0467/0x0468, EEE config — lives in extended space reached through this
 * indirection. Confirmed from SNLS505H Table 8-13 and SNLA265 section 2.2.2.
 *
 * REGCR (0x0D): bits[15:14] command, bits[4:0] DEVAD.
 *   00 = Address, 01 = Data no post-increment,
 *   10 = Data post-increment on read AND write, 11 = post-increment on write.
 *
 * DEVAD: 0x1F = vendor space (0x0000-0x04D6), 0x03 = MMD3, 0x07 = MMD7.
 *
 * *** THESE WRITE THE PHY. *** REGCR/ADDAR are a SHARED indirect pointer: if
 * the drive firmware also performs MMD accesses over the PDI, its pointer
 * writes and ours can interleave and corrupt each other in both directions.
 * ESC register 0x0510 bit 1 indicates whether the PDI may drive MII at all. */
#define MMD_DEVAD_VENDOR     0x1F
#define MMD_DEVAD_MMD3       0x03
#define MMD_DEVAD_MMD7       0x07

#define MMD_CMD_ADDR(d)      ((uint16_t)(d))              /* 00 = address     */
#define MMD_CMD_DATA(d)      ((uint16_t)(0x4000u | (d)))  /* 01 = data        */
#define MMD_CMD_DATA_INC(d)  ((uint16_t)(0x8000u | (d)))  /* 10 = data, ++    */

#define PHY_REG_REGCR        0x0D
#define PHY_REG_ADDAR        0x0E

/* Read one extended register. 4 PHY operations. Returns 0 on success. */
int mii_mmd_read(EscCtx *ctx, uint8_t phy_addr, uint8_t devad,
                 uint16_t reg, uint16_t *value);

/* Read n consecutive extended registers using post-increment: 3 setup
 * operations then one read each, instead of 4 per register. out[] holds n
 * values. Returns 0 on success. */
int mii_mmd_read_block(EscCtx *ctx, uint8_t phy_addr, uint8_t devad,
                       uint16_t start_reg, int n, uint16_t *out);

#endif /* ECAT_ESCMII_H */
