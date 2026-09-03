/* ── ecat_phy — DP83822 PHY register access over the ESC MII interface ──────
 *
 * *** THIS TOOL WRITES TO THE SLAVE. It is NOT ecat_regdump. ***
 *
 * Even a PHY *read* writes ESC registers: MII management is indirect, so the
 * PHY address/register selector (0x0512) and a command (0x0510) must be
 * written before the result appears in the data register (0x0514). Those
 * writes target the ESC's MII block, not the drive application.
 *
 * Reading EXTENDED registers additionally writes the PHY itself (REGCR/ADDAR
 * are a shared indirect pointer), and so requires --allow-phy-write.
 *
 * All register addresses, bit positions and encodings come from SNLS505H
 * Rev H section 8 and SNLA265. See phy_regs.h. */
#include "ecat_common.h"
#include "escmii.h"
#include "phy_regs.h"

static FILE *g_csv = NULL;

static void csv_row(int phy, const char *space, unsigned addr,
                    const char *name, int ok, uint16_t v)
{
    if (!g_csv) return;
    char clean[64]; size_t j = 0;
    for (const char *p = name; *p && j < sizeof(clean) - 1; p++)
        if (*p != ',') clean[j++] = (*p == ' ' && j && clean[j-1] == ' ') ? *p : *p;
    clean[j] = '\0';
    if (ok) fprintf(g_csv, "%d,%s,0x%04X,%s,0x%04X,%u\n", phy, space, addr, clean, v, v);
    else    fprintf(g_csv, "%d,%s,0x%04X,%s,,\n", phy, space, addr, clean);
}

static void bits16(uint16_t v) {
    for (int b = 15; b >= 0; b--) { putchar((v >> b) & 1 ? '1' : '0');
                                    if (b % 4 == 0 && b) putchar(' '); }
}

/* ── PHY identity gate ──────────────────────────────────────────────────────
 * Everything this tool decodes, and every register it writes, comes from
 * SNLS505H — the DP83822 datasheet. On a MIXED chain some slaves carry other
 * vendors' PHYs, where the same register numbers mean entirely different
 * things. Decoding those against the TI map would report confident nonsense,
 * and WRITING them (the canary targets 0x001B and MMD 0x1F 0x04A5, verified
 * inert only on a DP83822) could hit a control register on unknown silicon.
 *
 * So: confirm the part from the standard clause-22 PHY Identifier before
 * decoding or writing. SNLS505H Tables 8-3/8-4 give OUI 0x080028 and vendor
 * model 0x24; revision is allowed to be anything. */
#define DP83822_OUI    0x080028u
#define DP83822_MODEL  0x24u

static int phy_is_dp83822(EscCtx *ctx, int phy, uint32_t *oui_out,
                          unsigned *model_out, unsigned *rev_out) {
    uint16_t id1 = 0, id2 = 0;
    if (mii_read_phy(ctx, (uint8_t)phy, 0x02, &id1, NULL) != 0) return -1;
    if (mii_read_phy(ctx, (uint8_t)phy, 0x03, &id2, NULL) != 0) return -1;
    uint32_t oui   = ((uint32_t)id1 << 6) | ((id2 >> 10) & 0x3F);
    unsigned model = (id2 >> 4) & 0x3F;
    unsigned rev   = id2 & 0xF;
    if (oui_out) *oui_out = oui;
    if (model_out) *model_out = model;
    if (rev_out) *rev_out = rev;
    if (id1 == 0xFFFF || (id1 == 0 && id2 == 0)) return 0;   /* nothing there */
    return (oui == DP83822_OUI && model == DP83822_MODEL) ? 1 : 0;
}

/* ── Decoders. Every claim below cites SNLS505H Rev H section 8. ─────────── */
static void d_bmcr(uint16_t v) {
    printf("        reset=%d loopback=%d speed=%s autoneg=%s pwrdn=%d isolate=%d "
           "restart_an=%d duplex=%s\n",
           (v>>15)&1, (v>>14)&1, (v&0x2000)?"100M":"10M", (v&0x1000)?"on":"off",
           (v>>11)&1, (v>>10)&1, (v>>9)&1, (v&0x0100)?"full":"half");
    if (v & 0x0800)
        printf("        bit11 set: IEEE power-down — this bit READS THE "
               "INT/PWDN_N PIN (Table 8-1)\n");
}
static void d_bmsr(uint16_t v) {
    printf("        autoneg_complete=%d remote_fault=%d autoneg_able=%d "
           "LINK=%d jabber=%d\n",
           (v>>5)&1, (v>>4)&1, (v>>3)&1, (v>>2)&1, (v>>1)&1);
    printf("        [bit2 link status is LATCH-LOW: reads 0 if the link "
           "dropped since the previous read]\n");
}
static void d_aner(uint16_t v) {
    printf("        LP_autoneg_able=%d page_received=%d local_next_page=%d "
           "LP_next_page=%d parallel_detect_FAULT=%d\n",
           v&1, (v>>1)&1, (v>>2)&1, (v>>3)&1, (v>>4)&1);
    if (!(v & 1))
        printf("        *** bit0=0: link partner is NOT autonegotiating — the "
               "link came up by PARALLEL DETECTION,\n"
               "        *** which cannot resolve duplex (IEEE 802.3 defaults "
               "to HALF). Check PHYSTS bit2.\n");
}
static void d_anar(const char *tag, uint16_t v) {
    printf("        %s: 100FD=%d 100HD=%d 10FD=%d 10HD=%d selector=0x%02X\n",
           tag, (v>>8)&1, (v>>7)&1, (v>>6)&1, (v>>5)&1, v & 0x1F);
}
static void d_cr1(uint16_t v) {
    printf("        TDR_auto_run(bit8)=%d  (0 = TDR does NOT run on link drop)\n",
           (v>>8)&1);
}
static void d_cr3(uint16_t v) {
    printf("        bypass_deq_error(bit12)=%d [DEFAULT 1, Table 8-12]  "
           "descrambler_FLD(bit10)=%d\n", (v>>12)&1, (v>>10)&1);
    printf("        FLD criteria bits[3:0]: RX_err(b3)=%d MLT3(b2)=%d "
           "low_SNR(b1)=%d signal_loss(b0)=%d\n",
           (v>>3)&1, (v>>2)&1, (v>>1)&1, v&1);
    int any = ((v>>10)&1) || (v & 0x0F);
    printf("        Fast Link Down is the OR of bit10 and bits[3:0] -> %s\n",
           any ? "ENABLED" : "DISABLED (no FLD criterion active)");
}
static void d_flds(uint16_t v) {
    unsigned f = (v >> 4) & 0x1F;
    printf("        status field bits[8:4] = 0x%02X   [R, RC — CLEARED BY "
           "THIS READ, Table 8-15]\n", f);
    if (!f) { printf("        no fast-link-down event latched since last read\n");
              return; }
    if (f & 0x10) printf("        - Descrambler Loss Sync\n");
    if (f & 0x08) printf("        - RX Errors (32 RX_ER in 10us)\n");
    if (f & 0x04) printf("        - MLT3 Errors (20 in 10us)\n");
    if (f & 0x02) printf("        - SNR Level (20 threshold crossings in 10us)\n");
    if (f & 0x01) printf("        - Signal/Energy Lost\n");
}
static void d_physts(uint16_t v) {
    printf("        LINK=%d speed=%s DUPLEX=%s autoneg_complete=%d "
           "mdix=%d loopback=%d\n",
           v&1, (v&0x0002)?"10M":"100M", (v&0x0004)?"FULL":"HALF",
           (v>>4)&1, (v>>14)&1, (v>>3)&1);
    printf("        latches: rx_err(b13)=%d polarity_inv(b12)=%d "
           "false_carrier(b11)=%d sig_detect(b10)=%d descrambler_lock(b9)=%d\n",
           (v>>13)&1, (v>>12)&1, (v>>11)&1, (v>>10)&1, (v>>9)&1);
    printf("        [bit1 speed is INVERTED: 1=10Mbps, 0=100Mbps]\n");
}
static const char *misr1_names[8] = {
    "rx_error_HF", "false_carrier_HF", "autoneg_complete", "duplex_changed",
    "speed_changed", "link_status_changed", "energy_detect", "link_quality" };
static const char *misr2_names[8] = {
    "jabber_detect", "wol/polarity", "sleep_mode", "mdi_crossover_changed",
    "fifo_over_underflow", "page_received", "autoneg_error", "eee_error" };
static void d_misr(uint16_t v, const char **names) {
    unsigned st = (v >> 8) & 0xFF;
    printf("        status bits[15:8]=0x%02X  enables bits[7:0]=0x%02X   "
           "[R, RC — CLEARED BY THIS READ]\n", st, v & 0xFF);
    if (!st) { printf("        no interrupt events latched\n"); return; }
    for (int b = 0; b < 8; b++)
        if (st & (1u << b)) printf("        - %s\n", names[b]);
}
static void d_phyidr(uint16_t id1, uint16_t id2) {
    /* SNLS505H Tables 8-3/8-4: OUI[21:6] in ID1, OUI[5:0] in ID2[15:10],
     * model in ID2[9:4], revision in ID2[3:0]. */
    uint32_t oui = ((uint32_t)id1 << 6) | ((id2 >> 10) & 0x3F);
    printf("        OUI=0x%06X model=0x%02X revision=0x%X\n",
           oui, (id2 >> 4) & 0x3F, id2 & 0xF);
}
static void d_fcscr(uint16_t v) {
    printf("        false carrier events = %u  (8-bit, saturates at 255, "
           "CLEARED BY THIS READ)\n", v & 0xFF);
}
static void d_recr(uint16_t v) {
    printf("        RX_ER count = %u  (16-bit, saturates at 65535, "
           "CLEARED BY THIS READ)\n", v);
}
static void d_sor1(uint16_t v) {
    static const char *mode[4] = { "Mode 1", "Mode 2", "Mode 3", "Mode 4" };
    unsigned rxd1 = (v >> 14) & 3;
    printf("        RX_D1 strap = %s   (RX_D1 carries PHYAD_2 and EEE_EN)\n",
           mode[rxd1]);
    /* SNLA265 Tables 6/7/8: Mode1 = PHYAD_2 0, EEE disabled;
     * Mode2 = PHYAD_2 0, EEE enabled; Mode3 = PHYAD_2 1, EEE enabled. */
    if (rxd1 == 0) printf("        -> PHYAD_2=0, EEE DISABLED (SNLA265 Table 6)\n");
    else if (rxd1 == 1) printf("        -> PHYAD_2=0, EEE ENABLED (SNLA265 Table 7)\n");
    else if (rxd1 == 2) printf("        -> PHYAD_2=1, EEE ENABLED (SNLA265 Table 8)\n");
    else printf("        -> Mode 4: not documented in SNLA265\n");
    printf("        RX_D0=%u COL=%u RX_ER=%u strap modes\n",
           (v >> 12) & 3, (v >> 10) & 3, (v >> 8) & 3);
}

static void decode_direct(uint8_t reg, const uint16_t *val, const int *ok) {
    uint16_t v = val[reg];
    switch (reg) {
    case 0x00: d_bmcr(v); break;
    case 0x01: d_bmsr(v); break;
    case 0x03: if (ok[0x02]) d_phyidr(val[0x02], v); break;
    case 0x04: d_anar("advertised", v); break;
    case 0x05: d_anar("link partner", v); break;
    case 0x06: d_aner(v); break;
    case 0x09: d_cr1(v); break;
    case 0x0B: d_cr3(v); break;
    case 0x0F: d_flds(v); break;
    case 0x10: d_physts(v); break;
    case 0x12: d_misr(v, misr1_names); break;
    case 0x13: d_misr(v, misr2_names); break;
    case 0x14: d_fcscr(v); break;
    case 0x15: d_recr(v); break;
    default: break;
    }
}

/* Bit layout verified against Beckhoff ESC Section II Register Description
 * v3.3 §2.12.1: [0] write enable (self-clearing at SOF of the next frame),
 * [1] MI controllable by PDI, [2] MI link detection, [7:3] PHY address of
 * port 0, [10:8] command (001 read, 010 write), [12] clause-45 available,
 * [13] read error, [14] command error, [15] busy. */
static void decode_mii_status(uint16_t st) {
    static const char *cmd[8] = { "idle", "READ", "WRITE", "reserved",
                                  "c45 set-addr", "c45 read", "c45 write",
                                  "c45 read++" };
    printf("      0x0510 raw=0x%04X  busy(15)=%d cmd-err(14)=%d read-err(13)=%d\n",
           st, !!(st & MII_STAT_BUSY), !!(st & MII_STAT_CMD_ERR),
           !!(st & MII_STAT_READ_ERR));
    printf("      command[10:8]=%s  pdi-may-control(1)=%d  "
           "port0 PHY address[7:3]=%u  clause45(12)=%d\n",
           cmd[(st >> 8) & 7], !!(st & MII_CTRL_PDI_CTRL),
           (st >> 3) & 0x1F, !!(st & 0x1000));
}

static int check_mii_owner(EscCtx *ctx) {
    uint8_t ea = 0, pa = 0; uint16_t ctrl = 0;
    if (esc_read8(ctx, ESC_MII_ECAT_ACC, &ea) != 0 ||
        esc_read8(ctx, ESC_MII_PDI_ACC, &pa) != 0 ||
        esc_read16(ctx, ESC_MII_CTRL, &ctrl) != 0) {
        fprintf(stderr, "ERROR: cannot read the ESC MII block (0x0510-0x0517)\n");
        return -1;
    }
    printf("── MII management arbitration ─────────────────────────────\n");
    /* Polarity per Beckhoff ESC Sec II v3.3 §2.12.5/§2.12.6:
     *   0x0516[0]  0 = ECAT permits PDI takeover, 1 = ECAT claims exclusive
     *   0x0517[0]  0 = ECAT has access,           1 = PDI has access
     *   0x0517[1]  ECAT-writable: 1 resets 0x0517[0] to 0 */
    printf("  0x0516 ECAT access = 0x%02X -> %s\n", ea,
           (ea & 1) ? "ECAT claims EXCLUSIVE access"
                    : "ECAT permits PDI takeover");
    printf("  0x0517 PDI  access = 0x%02X -> %s\n", pa,
           (pa & 1) ? "*** PDI HAS ACCESS ***" : "ECAT has access");
    decode_mii_status(ctrl);
    if (pa & 1)
        printf("  *** The drive firmware currently OWNS the MII interface.\n"
               "  *** Master access will contend. 0x0517[1] can reset this.\n");
    else if ((ctrl & MII_CTRL_PDI_CTRL) && !(ea & 1))
        printf("  NOTE: ECAT has access, but 0x0510[1]=1 and 0x0516[0]=0 mean the\n"
               "  PDI may take over at any time. Writing 0x0516[0]=1 would claim\n"
               "  exclusive access (permitted while 0x0517[0]=0).\n");
    printf("\n");
    return 0;
}

/* ── The sweep ──────────────────────────────────────────────────────────── */
static int sweep_phy(EscCtx *ctx, int phy, int do_ext) {
    uint16_t val[32]; int ok[32];
    memset(val, 0, sizeof(val)); memset(ok, 0, sizeof(ok));

    printf("══ PHY %d ═════════════════════════════════════════════════\n", phy);
    uint32_t oui = 0; unsigned model = 0, rev = 0;
    int known = phy_is_dp83822(ctx, phy, &oui, &model, &rev);
    if (known == 1) {
        printf("  identity: DP83822 confirmed (OUI=0x%06X model=0x%02X rev=0x%X)\n",
               oui, model, rev);
    } else if (known == 0) {
        printf("  identity: OUI=0x%06X model=0x%02X rev=0x%X — NOT a DP83822.\n",
               oui, model, rev);
        printf("  *** Register VALUES below are raw and real, but the DECODES are\n"
               "  *** suppressed: they come from SNLS505H and do not apply to this\n"
               "  *** part. Consult that vendor's datasheet. ***\n");
    } else {
        printf("  identity: could not read the PHY ID — decodes suppressed\n");
    }
    printf("  Acquisition order is NOT address order: PHYSTS (0x10) is read\n"
           "  first because its latch bits are cleared by reading BMSR, ANER,\n"
           "  MISR1, FCSCR, RECR and 10BTSCR (SNLS505H Table 8-16). Values are\n"
           "  displayed below in address order.\n");
    printf("  This snapshot CONSUMES the clear-on-read registers: 0x01 0x06\n"
           "  0x0F 0x10 0x12 0x13 0x14 0x15 0x1A.\n\n");

    int failed = 0;
    for (int i = 0; i < 32; i++) {
        uint8_t r = phy_read_order[i];
        uint16_t v = 0, st = 0;
        if (mii_read_phy(ctx, (uint8_t)phy, r, &v, &st) == 0) {
            val[r] = v; ok[r] = 1;
        } else {
            failed++;
            if (failed == 1) { printf("  read failure at reg 0x%02X:\n", r);
                               decode_mii_status(st); }
        }
    }

    for (int r = 0; r < 32; r++) {
        const char *name = phy_direct[r].name ? phy_direct[r].name : "(unknown)";
        if (!ok[r]) { printf("  0x%02X %-42s  READ FAILED\n", r, name);
                      csv_row(phy, "direct", (unsigned)r, name, 0, 0); continue; }
        printf("  0x%02X %-42s 0x%04X  ", r, name, val[r]);
        bits16(val[r]);
        printf("%s\n", phy_direct[r].perishable ? "  [RC]" : "");
        csv_row(phy, "direct", (unsigned)r, name, 1, val[r]);
        if (known == 1) decode_direct((uint8_t)r, val, ok);
    }

    if (do_ext) {
        printf("\n  ── Extended registers (via REGCR/ADDAR — WRITES THE PHY) ──\n");
        for (int e = 0; e < PHY_EXT_COUNT; e++) {
            const PhyExt *x = &phy_ext[e];
            uint16_t buf[16];
            int n = x->count > 16 ? 16 : x->count;
            int rc = (n > 1)
                ? mii_mmd_read_block(ctx, (uint8_t)phy, x->devad, x->reg, n, buf)
                : mii_mmd_read(ctx, (uint8_t)phy, x->devad, x->reg, buf);
            if (rc != 0) {
                printf("  MMD%02X 0x%04X %-38s READ FAILED\n",
                       x->devad, x->reg, x->name);
                csv_row(phy, "ext", x->reg, x->name, 0, 0);
                continue;
            }
            for (int k = 0; k < n; k++) {
                printf("  MMD%02X 0x%04X %-38s 0x%04X  ", x->devad,
                       (unsigned)(x->reg + k), k ? "" : x->name, buf[k]);
                bits16(buf[k]); printf("\n");
                csv_row(phy, "ext", (unsigned)(x->reg + k), x->name, 1, buf[k]);
            }
            if (x->reg == 0x0467) d_sor1(buf[0]);
        }
    }
    printf("\n");
    return failed ? 1 : 0;
}

/* ── Reset canary (Task 3) ──────────────────────────────────────────────────
 * The DP83822 has no brownout detector, no power-on-reset flag and no
 * reset-reason register (searched SNLS505H Rev H), so "was this PHY reset?"
 * can only be answered by leaving a mark and seeing whether it survives.
 *
 * Two tiers, chosen from the datasheet's reset semantics:
 *
 *   TIER 1  BICSR1 0x001B bits[7:0] "BIST IPG Length", R/W, default 0x7D.
 *           Standard space, so cleared by ANY reset. Only affects the gap
 *           between packets the BIST generator emits, and BIST is off.
 *           Bits[15:8] are READ-ONLY (BIST error count) and bit 15 locks and
 *           clears that counter — so the write must keep bit 15 CLEAR.
 *
 *   TIER 2  MMD 0x1F (vendor) 0x04A5 "Receive Secure-ON Password #1", R/W,
 *           default 0. Holds a Wake-on-LAN Secure-ON password, which an
 *           EtherCAT drive never uses, so it is inert storage. Per Table 8-1,
 *           a BMCR bit-15 soft reset does NOT clear MMD 0x1F registers, and
 *           straps are re-latched only on power-up or RESET_N — and PHYRCR
 *           0x001F bit 15 "has the same effect as Hardware reset pin".
 *
 * With the ESC lost-link counter (already read in-band by ecat_ber, free)
 * that gives four distinguishable outcomes:
 *
 *   both marks present ................ no reset
 *   tier 1 gone, tier 2 present ....... BMCR soft reset
 *   both gone, ESC counter preserved .. PHYRCR / RESET_N hard reset
 *   both gone, ESC counters zeroed .... drive power cycle
 */
#define CANARY_T1_REG   0x1B
#define CANARY_T1_VAL   0x00A5   /* bit15 CLEAR; IPG = 0xA5 (default 0x7D)  */
#define CANARY_T1_DFLT  0x7D
#define CANARY_T2_DEVAD MMD_DEVAD_VENDOR
#define CANARY_T2_REG   0x04A5
#define CANARY_T2_VAL   0xC0DE

static int canary_write(EscCtx *ctx, int phy) {
    uint16_t rb = 0;
    int rc = 0;
    printf("── Reset canary: writing PHY %d ─────────────────────────────\n", phy);
    /* REFUSE on anything that is not a confirmed DP83822. The canary target
     * registers are verified inert only for that part; on another vendor's
     * PHY 0x001B is unknown vendor space and could be a control register. */
    uint32_t oui = 0; unsigned model = 0, rev = 0;
    int known = phy_is_dp83822(ctx, phy, &oui, &model, &rev);
    if (known != 1) {
        printf("  REFUSED: PHY %d is not a confirmed DP83822 "
               "(OUI=0x%06X model=0x%02X rev=0x%X).\n", phy, oui, model, rev);
        printf("  The canary registers are verified inert only on that part;\n"
               "  writing them on unknown silicon is not safe. Skipped.\n");
        return 2;
    }
    printf("  PHY %d confirmed DP83822 (OUI=0x%06X model=0x%02X rev=0x%X)\n",
           phy, oui, model, rev);
    int r = mii_write_phy(ctx, (uint8_t)phy, CANARY_T1_REG, CANARY_T1_VAL, 1, &rb);
    if (r == 0) printf("  tier1 BICSR1 0x1B    <- 0x%04X  (read-back 0x%04X)\n",
                       CANARY_T1_VAL, rb);
    else { printf("  tier1 BICSR1 0x1B    WRITE FAILED (rc=%d)\n", r); rc = 1; }

    /* Tier 2 is in extended space: set the pointer, then write ADDAR. */
    if (mii_write_phy(ctx, (uint8_t)phy, PHY_REG_REGCR,
                      MMD_CMD_ADDR(CANARY_T2_DEVAD), 0, NULL) != 0 ||
        mii_write_phy(ctx, (uint8_t)phy, PHY_REG_ADDAR, CANARY_T2_REG, 0, NULL) != 0 ||
        mii_write_phy(ctx, (uint8_t)phy, PHY_REG_REGCR,
                      MMD_CMD_DATA(CANARY_T2_DEVAD), 0, NULL) != 0 ||
        mii_write_phy(ctx, (uint8_t)phy, PHY_REG_ADDAR, CANARY_T2_VAL, 0, NULL) != 0) {
        printf("  tier2 MMD1F 0x%04X   WRITE FAILED\n", CANARY_T2_REG); return 1;
    }
    uint16_t v = 0;
    if (mii_mmd_read(ctx, (uint8_t)phy, CANARY_T2_DEVAD, CANARY_T2_REG, &v) == 0)
        printf("  tier2 MMD1F 0x%04X   <- 0x%04X  (read-back 0x%04X)%s\n",
               CANARY_T2_REG, CANARY_T2_VAL, v,
               v == CANARY_T2_VAL ? "" : "  *** MISMATCH ***");
    else { printf("  tier2 read-back FAILED\n"); rc = 1; }
    return rc;
}

static int canary_check(EscCtx *ctx, int phy) {
    uint16_t t1 = 0, t2 = 0;
    int ok1 = mii_read_phy(ctx, (uint8_t)phy, CANARY_T1_REG, &t1, NULL) == 0;
    int ok2 = mii_mmd_read(ctx, (uint8_t)phy, CANARY_T2_DEVAD,
                           CANARY_T2_REG, &t2) == 0;
    if (!ok1 || !ok2) { printf("  PHY %d canary read FAILED\n", phy); return 1; }

    int t1_ok = (t1 & 0x00FF) == (CANARY_T1_VAL & 0x00FF);
    int t2_ok = t2 == CANARY_T2_VAL;
    printf("  PHY %d  tier1 0x1B=0x%04X %s   tier2 0x%04X=0x%04X %s\n",
           phy, t1, t1_ok ? "intact" : "GONE",
           CANARY_T2_REG, t2, t2_ok ? "intact" : "GONE");
    if (t1_ok && t2_ok)   printf("         -> no PHY reset since the canary was written\n");
    else if (!t1_ok && t2_ok)
        printf("         -> *** BMCR soft reset detected *** (standard space\n"
               "            cleared, MMD 0x1F survived)\n");
    else if (!t1_ok && !t2_ok)
        printf("         -> *** HARD reset detected *** (PHYRCR 0x1F bit15,\n"
               "            RESET_N, or power cycle). Check the ESC lost-link\n"
               "            counter: preserved = PHY reset, zeroed = drive power cycle.\n");
    else
        printf("         -> tier1 intact but tier2 GONE — unexpected; the MMD\n"
               "            write may not have taken. Treat as inconclusive.\n");
    return (t1_ok && t2_ok) ? 0 : 2;
}

static void usage(const char *p) {
    printf("Usage: %s -i <iface> [options]\n"
           "  -i <iface>         interface (required)\n"
           "  -p <position>      chain position of the slave (default 0)\n"
           "  -a <phy|both>      PHY address, or 'both' (default: both 0 and 1)\n"
           "  -t <ms>            transaction timeout (default 10)\n"
           "  -d                 sweep all 32 direct registers (default action)\n"
           "  --ext              also read extended registers (needs --allow-phy-write)\n"
           "  --csv <file>       write a machine-readable snapshot\n"
           "  -r <reg>           read one direct register, e.g. -r 0x0F\n"
           "  -w <reg>=<val>     WRITE one PHY register (needs --allow-phy-write)\n"
           "  --allow-phy-write  permit writes to the PHY\n"
           "  --fld-enable       write CR3=0x140F (all five FLD criteria)\n"
           "  --fld-status       read FLDS and decode\n"
           "  --canary-write     write the two-tier reset canary (needs\n"
           "                     --allow-phy-write)\n"
           "  --canary-check     read it back and classify any reset\n\n"
           "This tool WRITES ESC registers even to READ a PHY register.\n"
           "Extended reads and PHY writes need --allow-phy-write.\n", p);
}

int main(int argc, char *argv[]) {
    const char *iface = NULL, *csv_path = NULL;
    int position = 0, timeout_ms = 10, phy_sel = -1;   /* -1 = both */
    int do_sweep = 0, do_ext = 0, allow_write = 0;
    int do_fld_status = 0, do_fld_enable = 0, cw = 0, cc = 0;
    int rd_reg = -1, wr_reg = -1; long wr_val = -1;

    static struct option lo[] = {
        { "allow-phy-write", no_argument,       0, 1000 },
        { "fld-enable",      no_argument,       0, 1001 },
        { "fld-status",      no_argument,       0, 1002 },
        { "ext",             no_argument,       0, 1003 },
        { "csv",             required_argument, 0, 1004 },
        { "canary-write",    no_argument,       0, 1005 },
        { "canary-check",    no_argument,       0, 1006 },
        { 0, 0, 0, 0 }
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "i:p:a:t:r:w:dh", lo, NULL)) != -1) {
        switch (opt) {
        case 'i': iface = optarg; break;
        case 'p': position = atoi(optarg); break;
        case 'a': phy_sel = (strcmp(optarg, "both") == 0)
                            ? -1 : (int)strtol(optarg, NULL, 0); break;
        case 't': timeout_ms = atoi(optarg); break;
        case 'd': do_sweep = 1; break;
        case 'r': rd_reg = (int)strtol(optarg, NULL, 0); break;
        case 'w': {
            char *eq = strchr(optarg, '=');
            if (!eq) { fprintf(stderr, "-w needs <reg>=<val>\n"); return 1; }
            *eq = '\0';
            wr_reg = (int)strtol(optarg, NULL, 0);
            wr_val = strtol(eq + 1, NULL, 0);
            break; }
        case 1000: allow_write = 1; break;
        case 1001: do_fld_enable = 1; break;
        case 1002: do_fld_status = 1; break;
        case 1003: do_ext = 1; break;
        case 1004: csv_path = optarg; break;
        case 1005: cw = 1; break;
        case 1006: cc = 1; break;
        default: usage(argv[0]); return opt == 'h' ? 0 : 1;
        }
    }
    if (!iface) { usage(argv[0]); return 1; }
    if (!do_sweep && !do_ext && !do_fld_status && !do_fld_enable &&
        !cw && !cc && rd_reg < 0 && wr_reg < 0) do_sweep = 1;
    if (do_ext) do_sweep = 1;

    if ((wr_reg >= 0 || do_fld_enable || cw) && !allow_write) {
        fprintf(stderr, "REFUSED: writing a PHY register changes DUT behaviour.\n"
                        "Re-run with --allow-phy-write if that is intended.\n");
        return 1;
    }
    if (do_ext && !allow_write) {
        fprintf(stderr, "REFUSED: extended (REGCR/ADDAR) reads WRITE the PHY's\n"
                        "indirect pointer registers, which the drive firmware may\n"
                        "also be using. Re-run with --allow-phy-write if intended.\n");
        return 1;
    }
    if (phy_sel > 31) { fprintf(stderr, "PHY address must be 0..31\n"); return 1; }

    EscCtx ctx;
    if (esc_open(&ctx, iface, (uint16_t)position, timeout_ms) != 0) return 1;

    if (csv_path) {
        g_csv = fopen(csv_path, "w");
        if (!g_csv) { perror("csv"); esc_close(&ctx); return 1; }
        fprintf(g_csv, "phy,space,addr,name,value_hex,value_dec\n");
    }

    printf("EtherCAT PHY register access over ESC MII  (THIS TOOL WRITES)\n");
    printf("Interface:  %s\nPosition:   %d\nTimeout:    %d ms\n",
           iface, position, timeout_ms);
    printf("MAC:        %02x:%02x:%02x:%02x:%02x:%02x\n",
           ctx.src_mac[0], ctx.src_mac[1], ctx.src_mac[2],
           ctx.src_mac[3], ctx.src_mac[4], ctx.src_mac[5]);
    printf("Registers decoded per SNLS505H Rev H section 8 and SNLA265.\n\n");

    if (check_mii_owner(&ctx) != 0) { esc_close(&ctx); return 1; }

    int rc = 0;
    if (cw) { if (phy_sel < 0) { rc |= canary_write(&ctx, 0);
                                 rc |= canary_write(&ctx, 1); }
              else               rc |= canary_write(&ctx, phy_sel);
              printf("\n"); }
    if (cc) { printf("── Reset canary: check ────────────────────────────────────\n");
              if (phy_sel < 0) { rc |= canary_check(&ctx, 0);
                                 rc |= canary_check(&ctx, 1); }
              else               rc |= canary_check(&ctx, phy_sel);
              printf("\n"); }
    if (do_sweep) {
        if (phy_sel < 0) { rc |= sweep_phy(&ctx, 0, do_ext);
                           rc |= sweep_phy(&ctx, 1, do_ext); }
        else               rc |= sweep_phy(&ctx, phy_sel, do_ext);
    }

    int one = phy_sel < 0 ? 0 : phy_sel;
    if (rd_reg >= 0) {
        uint16_t v = 0, st = 0;
        if (mii_read_phy(&ctx, (uint8_t)one, (uint8_t)rd_reg, &v, &st) != 0) {
            printf("READ FAILED\n"); decode_mii_status(st); rc = 1;
        } else {
            const char *nm = (rd_reg < 32 && phy_direct[rd_reg].name)
                             ? phy_direct[rd_reg].name : "(unknown)";
            printf("PHY %d 0x%02X %-42s 0x%04X  ", one, rd_reg, nm, v);
            bits16(v); printf("\n");
            uint16_t val[32]; int ok[32];
            memset(val,0,sizeof val); memset(ok,0,sizeof ok);
            val[rd_reg] = v; ok[rd_reg] = 1;
            if (rd_reg < 32) decode_direct((uint8_t)rd_reg, val, ok);
        }
    }

    if (do_fld_status) {
        uint16_t v = 0, st = 0;
        if (mii_read_phy(&ctx, (uint8_t)one, 0x0F, &v, &st) != 0) {
            printf("FLDS READ FAILED\n"); decode_mii_status(st); rc = 1;
        } else { printf("PHY %d FLDS (0x0F) = 0x%04X  ", one, v);
                 bits16(v); printf("\n"); d_flds(v); }
    }

    if (do_fld_enable) { wr_reg = 0x0B; wr_val = 0x140F; }

    if (wr_reg >= 0) {
        uint16_t before = 0, rb = 0, st = 0;
        printf("── PHY WRITE ──────────────────────────────────────────────\n");
        if (mii_read_phy(&ctx, (uint8_t)one, (uint8_t)wr_reg, &before, &st) == 0)
            printf("  before: PHY %d 0x%02X = 0x%04X\n", one, wr_reg, before);
        printf("  writing PHY %d 0x%02X <- 0x%04X\n", one, wr_reg, (uint16_t)wr_val);
        if (wr_reg == 0x0B) d_cr3((uint16_t)wr_val);
        int r = mii_write_phy(&ctx, (uint8_t)one, (uint8_t)wr_reg,
                              (uint16_t)wr_val, 1, &rb);
        if (r == 0)       printf("  OK: read-back = 0x%04X (matches)\n", rb);
        else if (r == -3) { printf("  MISMATCH: wrote 0x%04X, read 0x%04X\n",
                                   (uint16_t)wr_val, rb); rc = 1; }
        else              { printf("  WRITE FAILED (rc=%d)\n", r); rc = 1; }
    }

    printf("\nTransactions: %lu sent, %lu matched, %lu retries, %lu timeouts\n",
           ctx.frames_sent, ctx.frames_matched, ctx.retries, ctx.timeouts);
    if (g_csv) { fclose(g_csv); printf("CSV written to %s\n", csv_path); }
    esc_close(&ctx);
    return rc;
}
