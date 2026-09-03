/* ── ecat_regdump — READ-ONLY EtherCAT slave register dump ──────────────────
 * Answers Task 1: which ESC is in the DUT, and is its MII management
 * interface present and available to the master (ECAT) or held by the drive
 * firmware (PDI)?
 *
 * READ-ONLY BY CONSTRUCTION: only APRD datagrams are implemented anywhere in
 * this binary (see escreg.c). No argument combination can write to the slave.
 *
 * DECODE CAVEAT: every field is printed as a RAW value first; the decoded
 * interpretation is a convenience and is marked where it needs confirming
 * against ETG.1000.6 / the specific ESC datasheet. Never act on a decode
 * alone — the raw value is authoritative. */
#include "ecat_common.h"
#include "escreg.h"

static int g_json = 0;

/* ── Register tier tables ───────────────────────────────────────────────── */
/* optional=1: the register exists only on some ESC implementations. A WKC=0
 * on one of these means "this silicon does not implement it", which is a
 * finding, not a failure — an ESC does not process a datagram addressed
 * wholly to registers it lacks. */
typedef struct { uint16_t addr; uint16_t len; const char *name; int optional; } RegDef;

/* Tier 1 — identity. The Type byte at 0x0000 is what identifies the silicon. */
static const RegDef tier1[] = {
    { 0x0000, 1, "Type", 0 },
    { 0x0001, 1, "Revision", 0 },
    { 0x0002, 2, "Build", 0 },
    { 0x0004, 1, "FMMUs supported", 0 },
    { 0x0005, 1, "SyncManagers supported", 0 },
    { 0x0006, 1, "RAM size (KB)", 0 },
    { 0x0007, 1, "Port descriptor", 0 },
    { 0x0008, 2, "ESC features supported", 0 },
};
/* Tier 2 — addressing, link state, and (the point of the exercise) the
 * SII/EEPROM and MII management ownership registers. */
static const RegDef tier2[] = {
    { 0x0010, 2, "Configured station address", 0 },
    { 0x0012, 2, "Configured station alias", 0 },
    { 0x0100, 4, "ESC DL control", 0 },
    { 0x0110, 2, "ESC DL status", 0 },
    { 0x0120, 2, "AL control", 0 },
    { 0x0130, 2, "AL status", 0 },
    { 0x0134, 2, "AL status code", 0 },
    { 0x0500, 1, "EEPROM configuration (PDI/ECAT owner)", 0 },
    { 0x0501, 1, "EEPROM PDI access state", 0 },
    { 0x0502, 2, "EEPROM control/status", 0 },
    { 0x0510, 2, "MII management control/status", 0 },
    { 0x0512, 1, "MII PHY address", 0 },
    { 0x0513, 1, "MII PHY register address", 0 },
    { 0x0514, 2, "MII PHY data", 0 },
    { 0x0516, 1, "MII ECAT access state", 0 },
    { 0x0517, 1, "MII PDI access state", 0 },
};
/* Tier 3 — per-port error counters (static snapshot of what the BER tool
 * polls dynamically, for direct comparison). */
static const RegDef tier3[] = {
    { 0x0300, 8, "RX error counters (ports 0-3: invalid frame + RX error)", 0 },
    { 0x0308, 4, "Forwarded RX error counters (ports 0-3)", 0 },
    { 0x030C, 1, "ECAT processing unit error counter", 0 },
    { 0x030D, 1, "PDI error counter", 0 },
    { 0x0310, 4, "Lost link counters (ports 0-3)", 0 },
    /* Counts even when the port is CLOSED, unlike 0x0300-0x030B which only
     * count while the loop is open — so this still records errors on a port
     * that has already dropped. (ESC Sec II v3.3 §2.9.7.) */
    { 0x0314, 4, "Extended RX error counters (ports 0-3)", 1 },
};

static uint64_t le_val(const uint8_t *d, int len) {
    uint64_t v = 0;
    for (int i = len - 1; i >= 0; i--) v = (v << 8) | d[i];
    return v;
}

static void hexdump(const uint8_t *d, int len) {
    for (int i = 0; i < len; i++) printf("%02x%s", d[i], i + 1 < len ? " " : "");
}

/* ── Decode helpers (convenience only; raw value is authoritative) ──────── */
static const char *esc_type_name(uint8_t t) {
    /* CONFIRM against ETG.1000.6 Table "Type". These are the commonly cited
     * encodings; the raw byte is printed regardless and must be trusted over
     * this table. */
    switch (t) {
        case 0x01: return "ESC10/20 (early)?";
        case 0x02: return "IP Core?";
        case 0x04: return "IP Core (Altera/Xilinx)?";
        case 0x11: return "ET1100?";
        case 0x12: return "ET1200?";
        case 0x92: return "LAN9252?";
        default:   return "unknown";
    }
}

static void decode_port_descriptor(uint8_t pd) {
    /* 2 bits per port: 00 not implemented, 01 not configured,
     * 10 EBUS, 11 MII. CONFIRM against ETG.1000.6. */
    static const char *k[4] = { "not impl", "not cfg", "EBUS", "MII" };
    printf("      ports:");
    for (int p = 0; p < 4; p++) printf(" P%d=%s", p, k[(pd >> (2 * p)) & 3]);
    printf("   [decode unverified — raw byte above is authoritative]\n");
}

static void decode_dl_status(uint16_t s) {
    /* CONFIRM bit assignments against ETG.1000.6. Printed as an aid only. */
    printf("      PDI operational=%u  watchdog=%u\n", (s >> 0) & 1, (s >> 1) & 1);
    for (int p = 0; p < 4; p++) {
        printf("      P%d: link=%u  loop=%u  comm=%u\n", p,
               (s >> (4 + p)) & 1, (s >> (8 + p * 2)) & 1, (s >> (9 + p * 2)) & 1);
    }
    printf("      [decode unverified — raw value above is authoritative]\n");
}

/* ── Dump one tier ──────────────────────────────────────────────────────── */
static int dump_tier(EscCtx *ctx, const char *title,
                     const RegDef *defs, int ndefs, int decode) {
    printf("\n── %s ──────────────────────────────────────\n", title);
    int failures = 0;
    for (int i = 0; i < ndefs; i++) {
        EscRead r;
        memset(&r, 0, sizeof(r));
        r.addr = defs[i].addr;
        r.len  = defs[i].len;
        if (esc_read_multi(ctx, &r, 1) != 0 || !r.ok) {
            printf("  0x%04X %-42s  NO RESPONSE\n", defs[i].addr, defs[i].name);
            failures++;
            continue;
        }
        printf("  0x%04X %-42s  ", defs[i].addr, defs[i].name);
        hexdump(r.data, r.len);
        printf("   (wkc=%u)\n", r.wkc);
        if (r.wkc == 0 && defs[i].optional) {
            /* Expected on most silicon. 0x0314-0x0317 and 0x0320-0x0327 are
             * annotated "IP core V4.0.0" only in Beckhoff ESC Section II v3.3
             * §2.9.7/§2.9.8 — the ESC20/ET1100/ET1150/ET1200 columns are
             * blank. An ESC will not process a datagram addressed wholly to
             * registers it does not implement, so WKC=0 IS the answer here.
             * Not a failure, and the all-zero data means nothing: suppress the
             * decode rather than print four reassuring zeros. */
            printf("      NOT IMPLEMENTED on this ESC (WKC=0). These registers\n"
                   "      exist only on EtherCAT IP core V4.0.0 and later; the\n"
                   "      data above is meaningless, not a reading of zero.\n");
            continue;
        } else if (r.wkc == 0) {
            /* WKC=0 means NO slave processed the datagram. On a loopback
             * interface our own frame reflects back unmodified and looks like
             * a "response" — treat it as the failure it is. */
            printf("      WKC=0: no slave processed this read "
                   "(nothing at position %u, or frame reflected)\n", ctx->position);
            failures++;
        } else if (r.wkc != 1) {
            printf("      NOTE: wkc=%u (expected 1 — check chain position)\n", r.wkc);
        }

        if (!decode) continue;
        if (defs[i].addr == 0x0000)
            printf("      type=0x%02X -> %s  [CONFIRM against ETG.1000.6]\n",
                   r.data[0], esc_type_name(r.data[0]));
        else if (defs[i].addr == 0x0007)
            decode_port_descriptor(r.data[0]);
        else if (defs[i].addr == 0x0110)
            decode_dl_status((uint16_t)le_val(r.data, 2));
        else if (defs[i].addr == 0x0516 || defs[i].addr == 0x0517)
            printf("      raw=0x%02X — MII access arbitration; see notes below\n",
                   r.data[0]);
        else if (defs[i].addr == 0x0300) {
            for (int p = 0; p < 4; p++)
                printf("      P%d invalid-frame=%u  rx-error=%u\n", p,
                       r.data[p * 2], r.data[p * 2 + 1]);
        } else if (defs[i].addr == 0x0310) {
            for (int p = 0; p < 4; p++)
                printf("      P%d lost-link=%u\n", p, r.data[p]);
        } else if (defs[i].addr == 0x0314) {
            for (int p = 0; p < 4; p++)
                printf("      P%d extended-rx-error=%u%s\n", p, r.data[p],
                       r.data[p] == 0xFF ? "  <- SATURATED" : "");
        }
    }
    return failures;
}

static void mii_notes(void) {
    printf("\n── MII management availability (Task 1) ─────────────────────\n");
    printf("  The registers at 0x0510-0x0517 above determine whether PHY\n");
    printf("  register access is possible from the master:\n");
    printf("    * If 0x0510-0x0517 read back as all-zero or NO RESPONSE, this\n");
    printf("      ESC likely does not implement MII management -> PHY-level\n");
    printf("      diagnostics are NOT available over EtherCAT.\n");
    printf("    * 0x0516 (ECAT access) / 0x0517 (PDI access) indicate who owns\n");
    printf("      the interface. If the drive firmware (PDI) holds it, master\n");
    printf("      access would contend with the drive.\n");
    printf("  CONFIRM the exact bit layout against ETG.1000.6 and the ESC\n");
    printf("  datasheet before interpreting these values.\n");
    printf("\n  NOTE: reading PHY registers requires WRITING ESC MII registers.\n");
    printf("  That path is deliberately NOT implemented in this binary.\n");
}

static void usage(const char *p) {
    printf("Usage: %s -i <iface> [-p <position>] [-r <start>-<end>] [-t <ms>]\n"
           "  -i  interface (e.g. enp2s0)\n"
           "  -p  chain position of target slave (default 0)\n"
           "  -r  ad-hoc raw range, hex, e.g. -r 0x0300-0x0313\n"
           "  -t  per-transaction timeout in ms (default 10)\n"
           "  READ-ONLY: only APRD is implemented; no writes are possible.\n", p);
}

int main(int argc, char *argv[]) {
    const char *iface = NULL;
    int position = 0, timeout_ms = 10;
    uint32_t r_start = 0, r_end = 0; int have_range = 0;

    int opt;
    while ((opt = getopt(argc, argv, "i:p:r:t:h")) != -1) {
        switch (opt) {
        case 'i': iface = optarg; break;
        case 'p': position = atoi(optarg); break;
        case 't': timeout_ms = atoi(optarg); break;
        case 'r':
            if (sscanf(optarg, "%x-%x", &r_start, &r_end) != 2 &&
                sscanf(optarg, "0x%x-0x%x", &r_start, &r_end) != 2) {
                fprintf(stderr, "bad range '%s' (want 0x0300-0x0313)\n", optarg);
                return 1;
            }
            if (r_end < r_start || r_end > 0xFFFF) {
                fprintf(stderr, "bad range bounds\n"); return 1; }
            have_range = 1;
            break;
        default: usage(argv[0]); return opt == 'h' ? 0 : 1;
        }
    }
    if (!iface) { usage(argv[0]); return 1; }

    printf("EtherCAT ESC Register Dump (READ-ONLY)\n");
    printf("Interface:  %s\n", iface);
    printf("Position:   %d\n", position);
    printf("Timeout:    %d ms\n", timeout_ms);

    EscCtx ctx;
    if (esc_open(&ctx, iface, (uint16_t)position, timeout_ms) != 0) return 1;
    printf("MAC:        %02x:%02x:%02x:%02x:%02x:%02x\n",
           ctx.src_mac[0], ctx.src_mac[1], ctx.src_mac[2],
           ctx.src_mac[3], ctx.src_mac[4], ctx.src_mac[5]);

    int fail = 0;
    if (have_range) {
        uint16_t len = (uint16_t)(r_end - r_start + 1);
        uint8_t *buf = calloc(1, len ? len : 1);
        if (!buf) { esc_close(&ctx); return 1; }
        printf("\n── Raw range 0x%04X-0x%04X (%u bytes) ──────────────────\n",
               r_start, r_end, len);
        int wkc = esc_read_range(&ctx, (uint16_t)r_start, len, buf);
        if (wkc < 0) { printf("  NO RESPONSE\n"); fail = 1; }
        else {
            for (uint16_t off = 0; off < len; off += 16) {
                printf("  0x%04X: ", (unsigned)(r_start + off));
                int chunk = (len - off) < 16 ? (len - off) : 16;
                hexdump(buf + off, chunk);
                printf("\n");
            }
            printf("  (last wkc=%d)\n", wkc);
        }
        free(buf);
    } else {
        fail += dump_tier(&ctx, "Tier 1: ESC identity", tier1,
                          (int)(sizeof(tier1) / sizeof(tier1[0])), 1);
        fail += dump_tier(&ctx, "Tier 2: link state, SII and MII ownership",
                          tier2, (int)(sizeof(tier2) / sizeof(tier2[0])), 1);
        fail += dump_tier(&ctx, "Tier 3: per-port error counters", tier3,
                          (int)(sizeof(tier3) / sizeof(tier3[0])), 1);
        mii_notes();
    }

    printf("\nTransactions: %lu sent, %lu matched, %lu retries, %lu timeouts\n",
           ctx.frames_sent, ctx.frames_matched, ctx.retries, ctx.timeouts);
    if (fail) printf("*** %d register read(s) did not respond ***\n", fail);
    esc_close(&ctx);
    (void)g_json;
    return fail ? 2 : 0;
}
