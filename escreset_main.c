/* ── ecat_escreset — clear EtherCAT slave error counters ────────────────────
 *
 * *** THIS TOOL WRITES TO THE SLAVE (APWR). ***
 *
 * Kept as a separate binary so ecat_regdump remains read-only by construction
 * (it links escreg.o only, which implements APRD and nothing else).
 *
 * Semantics are from the Beckhoff EtherCAT Slave Controller documentation,
 * "Section II - Register Description" v3.3, section 2.9. These registers are
 * standard across ESC20 / ET1100 / ET1150 / ET1200 / IP core. Every counter is
 * r/w(clr) from ECAT and r/- from PDI — the MASTER can clear them and the
 * slave application cannot, so unlike the MII interface there is no
 * arbitration to contend with.
 *
 * There are FOUR independent clear groups. Writing one register in a group
 * clears the whole group; the write VALUE IS IGNORED (the datasheet says
 * write 0):
 *
 *   write 0x0300  -> clears 0x0300-0x030B (RX error + forwarded RX error),
 *                    0x0314-0x0317 (extended RX error),
 *                    0x0320-0x0327 (RX error code)
 *   write 0x030C  -> clears 0x030C only (ECAT processing unit error counter)
 *   write 0x030D  -> clears 0x030D and 0x030E:0x030F (PDI error code)
 *   write 0x0310  -> clears 0x0310-0x0313 (lost link counters)
 *
 * All counters are 8-bit and STOP at 0xFF rather than wrapping, so a saturated
 * counter has stopped recording and its value is only a floor. That is the
 * usual reason to clear them.
 *
 * Reads before and after so the effect is demonstrated, not assumed. */
#include "ecat_common.h"
#include "escmii.h"          /* for esc_write()/esc_read* — ESC write path */

typedef struct { uint16_t reg; const char *name; const char *clears; } ClearGroup;

static const ClearGroup groups[] = {
    { 0x0300, "rx",       "0x0300-0x030B RX + forwarded RX error, "
                          "0x0314-0x0317 extended, 0x0320-0x0327 error code" },
    { 0x030C, "pu",       "0x030C ECAT processing unit error counter"        },
    { 0x030D, "pdi",      "0x030D PDI0 error counter + 0x030E:0x030F code"   },
    { 0x0310, "lostlink", "0x0310-0x0313 lost link counters"                 },
};
#define NGROUPS ((int)(sizeof(groups)/sizeof(groups[0])))

/* The full error-counter block, for before/after display. */
static void dump_counters(EscCtx *ctx, const char *tag) {
    uint8_t b[20];
    printf("  %s:\n", tag);
    if (esc_read_range(ctx, 0x0300, 16, b) < 1) {
        printf("    read of 0x0300-0x030F FAILED\n"); return; }
    for (int p = 0; p < 4; p++)
        printf("    port%d  invalid=%-3u rxerr=%-3u fwderr=%-3u%s\n", p,
               b[p*2], b[p*2+1], b[8+p],
               (b[p*2]==0xFF||b[p*2+1]==0xFF||b[8+p]==0xFF) ? "  <- SATURATED" : "");
    printf("    0x030C proc-unit=%-3u%s   0x030D pdi=%-3u%s\n",
           b[12], b[12]==0xFF ? " <- SATURATED" : "",
           b[13], b[13]==0xFF ? " <- SATURATED" : "");
    uint8_t l[4];
    if (esc_read_range(ctx, 0x0310, 4, l) >= 1)
        printf("    0x0310 lost-link  P0=%u P1=%u P2=%u P3=%u\n",
               l[0], l[1], l[2], l[3]);
}

static void usage(const char *p) {
    printf("Usage: %s -i <iface> [-p pos] [-t ms] <groups> --yes-write-to-slave\n\n"
           "  Groups (repeatable, or --all):\n", p);
    for (int i = 0; i < NGROUPS; i++)
        printf("    --%-9s write 0x%04X -> clears %s\n",
               groups[i].name, groups[i].reg, groups[i].clears);
    printf("    --all       all four groups\n\n"
           "  --yes-write-to-slave   required; without it nothing is written\n\n"
           "Counters are r/w(clr) from ECAT (Beckhoff ESC Section II v3.3 "
           "§2.9).\nThe write value is ignored; this tool writes 0.\n");
}

int main(int argc, char *argv[]) {
    const char *iface = NULL;
    int position = 0, timeout_ms = 10, confirm = 0, want[NGROUPS];
    memset(want, 0, sizeof(want));

    static struct option lo[] = {
        { "rx",       no_argument, 0, 2000 }, { "pu",   no_argument, 0, 2001 },
        { "pdi",      no_argument, 0, 2002 }, { "lostlink", no_argument, 0, 2003 },
        { "all",      no_argument, 0, 2004 },
        { "yes-write-to-slave", no_argument, 0, 2005 },
        { 0, 0, 0, 0 }
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "i:p:t:h", lo, NULL)) != -1) {
        switch (opt) {
        case 'i': iface = optarg; break;
        case 'p': position = atoi(optarg); break;
        case 't': timeout_ms = atoi(optarg); break;
        case 2000: case 2001: case 2002: case 2003:
            want[opt - 2000] = 1; break;
        case 2004: for (int i = 0; i < NGROUPS; i++) want[i] = 1; break;
        case 2005: confirm = 1; break;
        default: usage(argv[0]); return opt == 'h' ? 0 : 1;
        }
    }
    if (!iface) { usage(argv[0]); return 1; }

    int any = 0;
    for (int i = 0; i < NGROUPS; i++) any |= want[i];
    if (!any) { fprintf(stderr, "No group selected. Use --all or a specific "
                                "group; see -h.\n"); return 1; }
    if (!confirm) {
        fprintf(stderr,
            "REFUSED: clearing error counters WRITES to the slave (APWR) and\n"
            "DESTROYS the accumulated history, which cannot be recovered.\n"
            "Take a regdump first if you need the current values.\n"
            "Re-run with --yes-write-to-slave if that is intended.\n");
        return 1;
    }

    EscCtx ctx;
    if (esc_open(&ctx, iface, (uint16_t)position, timeout_ms) != 0) return 1;

    printf("EtherCAT ESC error-counter reset  (THIS TOOL WRITES)\n");
    printf("Interface:  %s\nPosition:   %d\n\n", iface, position);
    printf("Semantics: Beckhoff ESC Section II Register Description v3.3 "
           "§2.9\n           counters are r/w(clr) from ECAT; write value "
           "ignored (writes 0)\n\n");

    dump_counters(&ctx, "BEFORE");

    printf("\n  clearing:\n");
    int rc = 0;
    for (int i = 0; i < NGROUPS; i++) {
        if (!want[i]) continue;
        int wkc = esc_write16(&ctx, groups[i].reg, 0);
        if (wkc < 1) {
            printf("    0x%04X  WRITE FAILED (wkc=%d)\n", groups[i].reg, wkc);
            rc = 1;
        } else {
            printf("    0x%04X  written (wkc=%d) -> %s\n",
                   groups[i].reg, wkc, groups[i].clears);
        }
    }

    printf("\n");
    dump_counters(&ctx, "AFTER");

    printf("\nTransactions: %lu sent, %lu matched, %lu retries, %lu timeouts\n",
           ctx.frames_sent, ctx.frames_matched, ctx.retries, ctx.timeouts);
    esc_close(&ctx);
    return rc;
}
