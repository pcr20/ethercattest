/* ── ecat_phy — DP83822 PHY register access over the ESC MII interface ──────
 *
 * *** THIS TOOL WRITES TO THE SLAVE. It is NOT ecat_regdump. ***
 *
 * Even a PHY *read* writes ESC registers: MII management is an indirect
 * interface, so the PHY address/register selector (0x0512) and a command
 * (0x0510) must be written before the result appears in the data register
 * (0x0514). Those writes target the ESC's MII block, not the drive
 * application — but if the drive firmware holds MII via PDI they contend.
 *
 * Writing a PHY REGISTER (which changes DUT behaviour) additionally requires
 * the explicit --allow-phy-write flag. There is no way to write a PHY register
 * by accident.
 *
 * DECODE CAVEAT: every value is printed RAW first. Decodes are a convenience
 * and are marked UNVERIFIED where they are not confirmed from the DP83822
 * datasheet (SNLS505H) or ETG.1000.6. The raw value is authoritative. */
#include "ecat_common.h"
#include "escmii.h"

/* ── DP83822 register map (confirmed from SNLS505H, see README §6.3) ────── */
typedef struct { uint8_t reg; const char *name; int clear_on_read; } PhyReg;

static const PhyReg phy_direct[] = {
    { 0x00, "BMCR   basic mode control",              0 },
    { 0x01, "BMSR   basic mode status (latched low)", 1 },
    { 0x06, "ANER   autoneg expansion (latched)",     1 },
    { 0x09, "CR1    control 1 (TDR auto-run bit 8)",  0 },
    { 0x0B, "CR3    control 3 (FLD criteria enable)", 0 },
    { 0x0F, "FLDS   fast link drop status",           0 },
    { 0x10, "PHYSTS phy status",                      0 },
    { 0x12, "MISR1  interrupt status 1",              1 },
    { 0x13, "MISR2  interrupt status 2",              1 },
    { 0x19, "PHYCR  phy control (auto-MDIX)",         0 },
};

static void print_bits(uint16_t v) {
    for (int b = 15; b >= 0; b--) {
        putchar((v >> b) & 1 ? '1' : '0');
        if (b % 4 == 0 && b) putchar(' ');
    }
}

static void decode_bmcr(uint16_t v) {
    printf("      speed=%s duplex=%s autoneg=%s power-down=%s isolate=%s loopback=%s\n",
           (v & 0x2000) ? "100M" : "10M", (v & 0x0100) ? "full" : "half",
           (v & 0x1000) ? "on" : "off",   (v & 0x0800) ? "YES" : "no",
           (v & 0x0400) ? "YES" : "no",   (v & 0x4000) ? "YES" : "no");
    if (v & 0x0800)
        printf("      NOTE bit11 set: reads the INT/PWDN_N pin state "
               "(mechanism B indicator) as well as commanded power-down\n");
}

static void decode_cr3(uint16_t v) {
    printf("      FLD enable (bit10)=%d  criteria bits[3:0]=0x%X\n",
           (v >> 10) & 1, v & 0xF);
    printf("        bit0 energy/signal loss =%d  bit1 low SNR      =%d\n",
           v & 1, (v >> 1) & 1);
    printf("        bit2 MLT3 error count   =%d  bit3 RX error cnt =%d\n",
           (v >> 2) & 1, (v >> 3) & 1);
    printf("      [bit->criterion mapping UNVERIFIED: SNLS505H confirms "
           "bits[3:0]+bit10 enable FLD, not the per-bit order]\n");
    uint16_t other = v & (uint16_t)~0x043Fu;
    if (other)
        printf("      [bits outside {3:0,10} set: 0x%04X — NOT explained by "
               "the confirmed datasheet facts]\n", other);
}

static void decode_flds(uint16_t v) {
    printf("      byte 0x0514(lo)=0x%02X  byte 0x0515(hi)=0x%02X\n",
           v & 0xFF, (v >> 8) & 0xFF);
    printf("      candidate status field bits[8:4] = 0x%02X  "
           "[UNVERIFIED — see README §6.4]\n", (v >> 4) & 0x1F);
    printf("      NOTE the vendor procedure reads only 0x0515 (the high byte);\n"
           "      if the status really is bits[8:4] that read LOSES bits 7:4.\n"
           "      This tool reads the full 16-bit register at 0x0514.\n");
}

static void decode_mii_status(uint16_t st) {
    printf("      0x0510 raw=0x%04X  busy(15)=%d cmd-err(14)=%d read-err(13)=%d "
           "pdi-may-control(1)=%d  [bits 14/13/1 UNVERIFIED]\n",
           st, (st & MII_STAT_BUSY) ? 1 : 0, (st & MII_STAT_CMD_ERR) ? 1 : 0,
           (st & MII_STAT_READ_ERR) ? 1 : 0, (st & MII_CTRL_PDI_CTRL) ? 1 : 0);
}

/* ── MII ownership check, run before anything else ──────────────────────── */
static int check_mii_owner(EscCtx *ctx) {
    uint8_t ecat_acc = 0, pdi_acc = 0;
    uint16_t ctrl = 0;
    if (esc_read8(ctx, ESC_MII_ECAT_ACC, &ecat_acc) != 0 ||
        esc_read8(ctx, ESC_MII_PDI_ACC,  &pdi_acc)  != 0 ||
        esc_read16(ctx, ESC_MII_CTRL,    &ctrl)     != 0) {
        fprintf(stderr, "ERROR: cannot read the MII block (0x0510-0x0517).\n");
        return -1;
    }
    printf("── MII management arbitration ─────────────────────────────\n");
    printf("  0x0516 ECAT access state = 0x%02X\n", ecat_acc);
    printf("  0x0517 PDI  access state = 0x%02X\n", pdi_acc);
    decode_mii_status(ctrl);
    if (ctrl & MII_CTRL_PDI_CTRL)
        printf("  WARNING: 0x0510 bit1 set — the PDI (drive firmware) is\n"
               "  permitted to control MII management. Master access may\n"
               "  contend with the drive. [decode UNVERIFIED]\n");
    printf("  [polarity of 0x0516/0x0517 is UNVERIFIED — confirm against "
           "ETG.1000.6]\n\n");
    return 0;
}

static void usage(const char *p) {
    printf("Usage: %s -i <iface> [options]\n"
           "  -i <iface>        interface (required)\n"
           "  -p <position>     chain position of the slave (default 0)\n"
           "  -a <phy_addr>     PHY address (default 0; the vendor FLD\n"
           "                    procedure uses PHY 0)\n"
           "  -t <ms>           transaction timeout, default 10\n"
           "  -d                dump the diagnostic PHY register set\n"
           "  -r <reg>          read one PHY register, e.g. -r 0x0F\n"
           "  -w <reg>=<val>    WRITE one PHY register, e.g. -w 0x0B=0x140F\n"
           "  --allow-phy-write required alongside -w; without it -w refuses\n"
           "  --fld-enable      write CR3 = 0x140F (the vendor FLD-enable\n"
           "                    value); needs --allow-phy-write\n"
           "  --fld-status      read FLDS (0x0F) and decode\n"
           "  -h                help\n\n"
           "This tool WRITES ESC registers even to READ a PHY register.\n"
           "Writing a PHY register needs --allow-phy-write.\n", p);
}

int main(int argc, char *argv[]) {
    const char *iface = NULL;
    int position = 0, phy_addr = 0, timeout_ms = 10;
    int do_dump = 0, do_fld_status = 0, do_fld_enable = 0, allow_write = 0;
    int rd_reg = -1, wr_reg = -1; long wr_val = -1;

    static struct option lo[] = {
        { "allow-phy-write", no_argument, 0, 1000 },
        { "fld-enable",      no_argument, 0, 1001 },
        { "fld-status",      no_argument, 0, 1002 },
        { 0, 0, 0, 0 }
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "i:p:a:t:r:w:dh", lo, NULL)) != -1) {
        switch (opt) {
        case 'i': iface = optarg; break;
        case 'p': position = atoi(optarg); break;
        case 'a': phy_addr = (int)strtol(optarg, NULL, 0); break;
        case 't': timeout_ms = atoi(optarg); break;
        case 'd': do_dump = 1; break;
        case 'r': rd_reg = (int)strtol(optarg, NULL, 0); break;
        case 'w': {
            char *eq = strchr(optarg, '=');
            if (!eq) { fprintf(stderr, "-w needs <reg>=<val>\n"); return 1; }
            *eq = '\0';
            wr_reg = (int)strtol(optarg, NULL, 0);
            wr_val = strtol(eq + 1, NULL, 0);
            break;
        }
        case 1000: allow_write = 1; break;
        case 1001: do_fld_enable = 1; break;
        case 1002: do_fld_status = 1; break;
        default: usage(argv[0]); return opt == 'h' ? 0 : 1;
        }
    }
    if (!iface) { usage(argv[0]); return 1; }
    if (!do_dump && !do_fld_status && !do_fld_enable && rd_reg < 0 && wr_reg < 0)
        do_dump = 1;

    if ((wr_reg >= 0 || do_fld_enable) && !allow_write) {
        fprintf(stderr,
            "REFUSED: writing a PHY register changes DUT behaviour.\n"
            "Re-run with --allow-phy-write if that is what you intend.\n");
        return 1;
    }
    if (phy_addr < 0 || phy_addr > 31) {
        fprintf(stderr, "PHY address must be 0..31\n"); return 1; }

    EscCtx ctx;
    if (esc_open(&ctx, iface, (uint16_t)position, timeout_ms) != 0) return 1;

    printf("EtherCAT PHY register access over ESC MII  (THIS TOOL WRITES)\n");
    printf("Interface:  %s\nPosition:   %d\nPHY addr:   %d\nTimeout:    %d ms\n",
           iface, position, phy_addr, timeout_ms);
    printf("MAC:        %02x:%02x:%02x:%02x:%02x:%02x\n\n",
           ctx.src_mac[0], ctx.src_mac[1], ctx.src_mac[2],
           ctx.src_mac[3], ctx.src_mac[4], ctx.src_mac[5]);

    if (check_mii_owner(&ctx) != 0) { esc_close(&ctx); return 1; }

    int rc = 0;

    if (do_dump) {
        printf("── PHY %d register snapshot ────────────────────────────────\n",
               phy_addr);
        printf("  CAUTION: BMSR/ANER/MISR1/MISR2 are latching or clear-on-read.\n"
               "  Reading them CLEARS the latched state. They are read FIRST.\n\n");
        for (size_t i = 0; i < sizeof(phy_direct)/sizeof(phy_direct[0]); i++) {
            uint16_t v = 0, st = 0;
            int r = mii_read_phy(&ctx, (uint8_t)phy_addr, phy_direct[i].reg, &v, &st);
            if (r != 0) {
                printf("  0x%02X %-38s READ FAILED (rc=%d)\n",
                       phy_direct[i].reg, phy_direct[i].name, r);
                decode_mii_status(st);
                rc = 1; continue;
            }
            printf("  0x%02X %-38s 0x%04X  ", phy_direct[i].reg,
                   phy_direct[i].name, v);
            print_bits(v);
            printf("%s\n", phy_direct[i].clear_on_read ? "  [CLEARED BY THIS READ]" : "");
            if (phy_direct[i].reg == 0x00) decode_bmcr(v);
            if (phy_direct[i].reg == 0x0B) decode_cr3(v);
            if (phy_direct[i].reg == 0x0F) decode_flds(v);
        }
        printf("\n");
    }

    if (rd_reg >= 0) {
        uint16_t v = 0, st = 0;
        int r = mii_read_phy(&ctx, (uint8_t)phy_addr, (uint8_t)rd_reg, &v, &st);
        if (r != 0) { printf("READ FAILED (rc=%d)\n", r); decode_mii_status(st); rc = 1; }
        else {
            printf("PHY %d reg 0x%02X = 0x%04X  ", phy_addr, rd_reg, v); print_bits(v);
            printf("\n");
            printf("  bytes: 0x0514(lo)=0x%02X 0x0515(hi)=0x%02X\n", v & 0xFF, v >> 8);
            if (rd_reg == 0x00) decode_bmcr(v);
            if (rd_reg == 0x0B) decode_cr3(v);
            if (rd_reg == 0x0F) decode_flds(v);
        }
    }

    if (do_fld_status) {
        uint16_t v = 0, st = 0;
        int r = mii_read_phy(&ctx, (uint8_t)phy_addr, 0x0F, &v, &st);
        if (r != 0) { printf("FLDS READ FAILED (rc=%d)\n", r); decode_mii_status(st); rc = 1; }
        else { printf("FLDS (PHY %d reg 0x0F) = 0x%04X  ", phy_addr, v);
               print_bits(v); printf("\n"); decode_flds(v); }
    }

    if (do_fld_enable) { wr_reg = 0x0B; wr_val = 0x140F; }

    if (wr_reg >= 0) {
        uint16_t before = 0, rb = 0, st = 0;
        printf("── PHY WRITE ──────────────────────────────────────────────\n");
        if (mii_read_phy(&ctx, (uint8_t)phy_addr, (uint8_t)wr_reg, &before, &st) == 0)
            printf("  before: PHY %d reg 0x%02X = 0x%04X\n", phy_addr, wr_reg, before);
        else
            printf("  before: read failed — proceeding anyway\n");
        printf("  writing PHY %d reg 0x%02X <- 0x%04X\n",
               phy_addr, wr_reg, (uint16_t)wr_val);
        if (wr_reg == 0x0B) decode_cr3((uint16_t)wr_val);

        int r = mii_write_phy(&ctx, (uint8_t)phy_addr, (uint8_t)wr_reg,
                              (uint16_t)wr_val, 1, &rb);
        if (r == 0)      printf("  OK: read-back = 0x%04X (matches)\n", rb);
        else if (r == -3){ printf("  MISMATCH: wrote 0x%04X, read back 0x%04X\n",
                                  (uint16_t)wr_val, rb); rc = 1; }
        else             { printf("  WRITE FAILED (rc=%d)\n", r); rc = 1; }
    }

    printf("\nTransactions: %lu sent, %lu matched, %lu retries, %lu timeouts\n",
           ctx.frames_sent, ctx.frames_matched, ctx.retries, ctx.timeouts);
    esc_close(&ctx);
    return rc;
}
