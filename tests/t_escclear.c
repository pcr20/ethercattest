/* ESC error-counter clear groups, per Beckhoff ESC Section II Register
 * Description v3.3 section 2.9. Also covers the widened parse of the 16-byte
 * 0x0300 datagram that ecat_ber already fetches every frame. */
#include "crc.c"
#include "stats.c"
#include "frame.c"

static int fails = 0;
#define CHECK(c, ...) do { if(!(c)) { printf("FAIL: "); printf(__VA_ARGS__); \
                                      printf("\n"); fails++; } } while (0)

/* Build a synthetic APRD reply carrying a known 0x0300-0x030F block. */
static int mk(uint8_t *b, const uint8_t blk[ESC_DIAG_LEN], uint64_t seq, int fcs_ok)
{
    int len = build_frame(b, MAX_FRAME, (const uint8_t[]){1,2,3,4,5,6}, 1, seq, 0);
    /* Locate the first APRD datagram's data (after Eth+ECAT hdr, NOP, BRD). */
    int pos = ETH_HDR_LEN + ECAT_HDR_LEN;
    for (int guard = 0; guard < 8; guard++) {
        uint16_t lf = le16get(b + pos + 6);
        uint16_t dl = lf & 0x07FF;
        if (b[pos] == ECAT_CMD_APRD && dl == ESC_DIAG_LEN) {
            memcpy(b + pos + ECAT_DG_HDR_LEN, blk, ESC_DIAG_LEN);
            le16put(b + pos + ECAT_DG_HDR_LEN + ESC_DIAG_LEN, 1);  /* WKC = 1 */
            return fcs_ok ? len : len;
        }
        pos += ECAT_DG_HDR_LEN + dl + ECAT_DG_WKC_LEN;
        if (!((lf >> 15) & 1)) break;
    }
    return -1;
}

int main(void)
{
    crc32c_init();                    /* §4.3 invariant: always first */

    /* ── T1: the four clear groups, from the datasheet ──────────────────── */
    {
        int f0 = fails;
        /* Writing ANY of 0x0300-0x030B clears that whole group; 0x030C,
         * 0x030D and 0x0310 are independent groups. */
        struct { uint16_t reg; uint16_t lo, hi; } g[] = {
            { 0x0300, 0x0300, 0x030B },
            { 0x030C, 0x030C, 0x030C },
            { 0x030D, 0x030D, 0x030D },
            { 0x0310, 0x0310, 0x0313 },
        };
        for (size_t i = 0; i < sizeof(g)/sizeof(g[0]); i++) {
            CHECK(g[i].reg >= g[i].lo && g[i].reg <= g[i].hi,
                  "group %zu: write reg 0x%04X outside its own range", i, g[i].reg);
            for (size_t j = 0; j < i; j++)
                CHECK(!(g[i].lo <= g[j].hi && g[j].lo <= g[i].hi),
                      "clear groups %zu and %zu overlap — they are independent "
                      "per ESC Sec II v3.3 §2.9", i, j);
        }
        CHECK(g[0].hi == 0x030B,
              "0x030C must NOT be in the 0x0300 group: clearing RX errors must "
              "not silently clear the processing-unit counter");
        if (fails == f0)
            printf("T1 PASS: four independent clear groups, 0x030C separate "
                   "from 0x0300-0x030B\n");
    }

    /* ── T2: the widened parse reads all 16 bytes, not just 4 ───────────── */
    {
        int f0 = fails;
        memset(&g_stats, 0, sizeof(g_stats));
        g_num_slaves = 1;
        uint8_t blk[ESC_DIAG_LEN];
        memset(blk, 0, sizeof(blk));
        for (int p = 0; p < 4; p++) {
            blk[p*2]     = (uint8_t)(1 + p*2);   /* 0x0300+2p invalid  */
            blk[p*2 + 1] = (uint8_t)(2 + p*2);   /* 0x0301+2p rxerr    */
            blk[8 + p]   = (uint8_t)(9 + p);     /* 0x0308+p forwarded */
            blk[16 + p]  = (uint8_t)(20 + p);    /* 0x0310+p lost link */
            blk[20 + p]  = (uint8_t)(30 + p);    /* 0x0314+p ext RX    */
        }
        blk[12] = 13;                             /* 0x030C proc unit   */
        blk[13] = 14;                             /* 0x030D PDI0        */
        le16put(blk + 32 + 1*2, 0x0058);          /* 0x0322 port1 code  */
        uint8_t f[MAX_FRAME];
        int len = mk(f, blk, 0, 1);
        CHECK(len > 0, "could not build a synthetic APRD reply");
        parse_return_frame(f, len, 1, 0, /*fcs_ok=*/1, NULL);

        CHECK(g_stats.esc_crc[0][0] == 1 && g_stats.esc_crc[0][3] == 7,
              "invalid-frame counters wrong: %lu %lu",
              g_stats.esc_crc[0][0], g_stats.esc_crc[0][3]);
        CHECK(g_stats.esc_rxerr[0][0] == 2 && g_stats.esc_rxerr[0][3] == 8,
              "RX error counters (odd bytes) not parsed: %lu %lu",
              g_stats.esc_rxerr[0][0], g_stats.esc_rxerr[0][3]);
        CHECK(g_stats.esc_fwderr[0][0] == 9 && g_stats.esc_fwderr[0][3] == 12,
              "forwarded RX error counters not parsed: %lu %lu",
              g_stats.esc_fwderr[0][0], g_stats.esc_fwderr[0][3]);
        CHECK(g_stats.esc_puerr[0] == 13, "0x030C not parsed: %lu",
              g_stats.esc_puerr[0]);
        CHECK(g_stats.esc_pdierr[0] == 14, "0x030D not parsed: %lu",
              g_stats.esc_pdierr[0]);
        CHECK(g_stats.esc_lostlnk[0][0] == 20 && g_stats.esc_lostlnk[0][3] == 23,
              "lost-link now in the SAME datagram: %lu %lu",
              g_stats.esc_lostlnk[0][0], g_stats.esc_lostlnk[0][3]);
        CHECK(g_stats.esc_extrx[0][0] == 30 && g_stats.esc_extrx[0][3] == 33,
              "0x0314+p extended RX error not parsed: %lu %lu",
              g_stats.esc_extrx[0][0], g_stats.esc_extrx[0][3]);
        CHECK(g_stats.esc_rxcode[0][1] == 0x0058,
              "0x0320+2p RX error code not parsed: 0x%04X",
              g_stats.esc_rxcode[0][1]);
        CHECK(strstr(esc_rx_error_code_name(0x58), "inter-frame gap") != NULL,
              "0x58 must decode as the IFG-too-short reason");
        CHECK(strstr(esc_rx_error_code_name(0x50), "OVERRUN") != NULL,
              "0x50 must decode as FIFO overrun");
        CHECK(strstr(esc_rx_error_code_name(0x21), "RX_CLK") != NULL,
              "0x21 must decode as RX_CLK too slow");
        if (fails == f0)
            printf("T2 PASS: the whole 0x0300-0x0327 block parsed — rxerr, fwderr, 0x030C and "
                   "0x030D no longer discarded\n");
    }

    /* ── T3: saturation is visible, not mistaken for "no errors" ────────── */
    {
        int f0 = fails;
        memset(&g_stats, 0, sizeof(g_stats));
        g_num_slaves = 1;
        uint8_t blk[ESC_DIAG_LEN];
        memset(blk, 0, sizeof(blk));
        blk[0] = 0xFF; blk[12] = 0xFF;
        uint8_t f[MAX_FRAME];
        int len = mk(f, blk, 0, 1);
        parse_return_frame(f, len, 1, 0, /*fcs_ok=*/1, NULL);
        CHECK(g_stats.esc_raw_crc[0][0] == 0xFF,
              "raw value must be retained so saturation can be reported");
        CHECK(g_stats.esc_raw_puerr[0] == 0xFF,
              "raw 0x030C must be retained: it stops at 0xFF and the delta "
              "then reads 0 forever, which looks like no errors");
        if (fails == f0)
            printf("T3 PASS: raw 0xFF retained so a saturated counter is "
                   "distinguishable from a clean one\n");
    }

    /* ── T4: corrupt frames still gated out (§3.7) ──────────────────────── */
    {
        int f0 = fails;
        memset(&g_stats, 0, sizeof(g_stats));
        g_num_slaves = 1;
        uint8_t blk[ESC_DIAG_LEN];
        memset(blk, 200, sizeof(blk));
        uint8_t f[MAX_FRAME];
        int len = mk(f, blk, 0, 1);
        parse_return_frame(f, len, 1, 0, /*fcs_ok=*/0, NULL);
        CHECK(g_stats.esc_rxerr[0][0] == 0 && g_stats.esc_puerr[0] == 0,
              "a bad-FCS frame must not poison the NEW counters either "
              "(rxerr=%lu puerr=%lu)",
              g_stats.esc_rxerr[0][0], g_stats.esc_puerr[0]);
        if (fails == f0)
            printf("T4 PASS: new counters gated on FCS-valid frames, as §3.7 "
                   "requires for the existing ones\n");
    }

    if (fails) { printf("\n%d ESC-CLEAR CHECK(S) FAILED\n", fails); return 1; }
    printf("\nALL ESC-CLEAR TESTS PASS\n");
    return 0;
}
