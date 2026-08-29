#include "frame.h"
#include "crc.h"
#include "stats.h"

/* ── xorshift64 PRNG (deterministic per-frame payload fill) ──────────────────
 * Seeded from the sequence number. Spectrally rich enough to properly exercise
 * the 100BASE-TX MLT-3 scrambler (unlike a constant fill). */
static inline uint64_t xorshift64(uint64_t *s) {
    uint64_t x = *s;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    *s = x;
    return x;
}
/* Fill buf[0..n) with pseudo-random bytes seeded from seq. Seed is offset by a
 * constant so seq=0 doesn't yield the xorshift fixed point (0 -> all zeros). */
static inline void fill_random_payload(uint8_t *buf, size_t n, uint64_t seq) {
    uint64_t s = seq ^ 0x9E3779B97F4A7C15ULL;   /* avoid zero-state */
    if (s == 0) s = 0xDEADBEEFCAFEBABEULL;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t r = xorshift64(&s);
        memcpy(buf + i, &r, 8);
    }
    if (i < n) {
        uint64_t r = xorshift64(&s);
        memcpy(buf + i, &r, n - i);
    }
}

/* ── Build EtherCAT frame ───────────────────────────────────────────────── */
/*
 * Frame layout:
 *   [Ethernet header 14B]
 *   [EtherCAT header 2B]
 *   [Datagram 0: NOP, carrying fill payload, more=1]
 *   [Datagram 1: BRD reg 0x0000, 1 byte, more=1 if slaves>0]
 *   [Datagram 2..N: APRD slave 0..N-1, reg 0x0300, 8 bytes (CRC+lostlnk)]
 *     last APRD has more=0
 *
 * In loopback mode: just NOP datagram, no BRD/APRD.
 */
int build_frame(uint8_t *buf, int buflen,
                       const uint8_t *src_mac, int num_slaves,
                       uint64_t seq, int loopback)
{
    memset(buf, 0, buflen);

    /* Ethernet header */
    memset(buf, 0xff, 6);              /* dst: broadcast */
    memcpy(buf + 6, src_mac, 6);
    buf[12] = 0x88; buf[13] = 0xA4;   /* EtherType */

    /* Calculate payload sizes. NOTE: the APRD data size here MUST match the
     * writer below (16 bytes: registers 0x0300-0x030F — port CRC/RX-error
     * counters; lost-link 0x0310+ is NOT in this read). A mismatch makes
     * nop_payload too large and the APRD loop writes past buflen — this exact
     * mismatch (budget 8 vs writer 16) caused a stack overflow. */
    /* ONE APRD per slave covering 0x0300-0x0327 (ESC_DIAG_LEN). This replaced
     * the previous two-datagram scheme (16 B at 0x0300 + 4 B at 0x0310) and
     * must stay in step with the writer below — a budget/writer mismatch of
     * exactly this kind caused the stack overflow described in README §4.2. */
    int aprd_bytes   = loopback ? 0 : (num_slaves * (ECAT_DG_OVERHEAD + ESC_DIAG_LEN));
    int aprd2_bytes  = 0;
    int brd_bytes    = loopback ? 0 : (ECAT_DG_OVERHEAD + 1);
    int overhead     = ETH_HDR_LEN + ECAT_HDR_LEN + ECAT_DG_OVERHEAD
                       + brd_bytes + aprd_bytes + aprd2_bytes;
    int nop_payload  = buflen - overhead;
    if (nop_payload < 8) nop_payload = 8;  /* minimum sensible payload */

    /* Cursor */
    int pos = ETH_HDR_LEN + ECAT_HDR_LEN;  /* skip EtherCAT header for now */

    /* Datagram 0: NOP carrying payload */
    int has_more_after_nop = (!loopback && num_slaves > 0) ? 1 : 0;
    uint16_t nop_len_flags = (uint16_t)((nop_payload & 0x07FF)
                                   | (has_more_after_nop ? 0x8000 : 0));
    buf[pos++] = ECAT_CMD_NOP;
    buf[pos++] = ECAT_IDX_NOP;
    buf[pos++] = 0; buf[pos++] = 0; buf[pos++] = 0; buf[pos++] = 0; /* addr */
    le16put(buf + pos, nop_len_flags); pos += 2;   /* LE per ETG.1000.4 */
    buf[pos++] = 0; buf[pos++] = 0;  /* IRQ */
    /* Payload: [seq(8)][crc32c(4)][random(N)].
     * Ensure the payload is at least large enough for the header. */
    if (nop_payload < PL_HDR_LEN) nop_payload = PL_HDR_LEN;
    {
        uint8_t *pl = buf + pos;
        /* seq (LE) */
        memcpy(pl + PL_SEQ_OFF, &seq, PL_SEQ_LEN);
        /* pseudo-random payload after the 12-byte header */
        int rnd_len = nop_payload - PL_DATA_OFF;
        if (rnd_len < 0) rnd_len = 0;
        fill_random_payload(pl + PL_DATA_OFF, rnd_len, seq);
        /* CRC32C over seq bytes ++ random payload bytes, as one logical
         * stream (they are separated on the wire by the 4-byte CRC hole). */
        uint32_t crc = crc32c_chain(pl + PL_SEQ_OFF, PL_SEQ_LEN,
                                    pl + PL_DATA_OFF, (size_t)rnd_len);
        memcpy(pl + PL_CRC_OFF, &crc, PL_CRC_LEN);
    }
    atomic_store_explicit(&g_payload_crc_bytes,
                          (uint64_t)(PL_SEQ_LEN + (nop_payload - PL_DATA_OFF)),
                          memory_order_relaxed);
    pos += nop_payload;
    buf[pos++] = 0; buf[pos++] = 0;  /* WKC */

    if (!loopback && num_slaves > 0) {
        /* Datagram 1: BRD reading register 0x0000 (type/revision, safe) */
        int has_more_after_brd = (num_slaves > 0) ? 1 : 0;
        uint16_t brd_lf = (uint16_t)(1 | (has_more_after_brd ? 0x8000 : 0));
        buf[pos++] = ECAT_CMD_BRD;
        buf[pos++] = ECAT_IDX_BRD;
        buf[pos++] = 0; buf[pos++] = 0; buf[pos++] = 0; buf[pos++] = 0;
        le16put(buf + pos, brd_lf); pos += 2;   /* LE per ETG.1000.4 */
        buf[pos++] = 0; buf[pos++] = 0;  /* IRQ */
        buf[pos++] = 0;                   /* 1 byte data */
        buf[pos++] = 0; buf[pos++] = 0;  /* WKC */

        /* Datagrams 2..N+1: APRD per slave, reading 16 bytes at 0x0300 —
         * registers 0x0300-0x030F (port invalid-frame + RX-error counters).
         * Lost-link counters live at 0x0310-0x0313 and are NOT covered by
         * this read; they would need a second APRD set (see parse side). */
        for (int s = 0; s < num_slaves; s++) {
            int is_last = (s == num_slaves - 1);
            uint16_t aprd_lf = (uint16_t)(ESC_DIAG_LEN | (is_last ? 0 : 0x8000));
            /* Auto-increment addressing: we write ADP = -s, each slave
             * increments it as the frame passes, so slave s sees 0. */
            uint16_t node_le = (uint16_t)(-(int16_t)s);
            buf[pos++] = ECAT_CMD_APRD;
            buf[pos++] = (uint8_t)s;   /* idx = slave number, for demux */
            buf[pos++] = (node_le) & 0xFF;
            buf[pos++] = (node_le >> 8) & 0xFF;
            buf[pos++] = ESC_DIAG_BASE & 0xFF;
            buf[pos++] = (ESC_DIAG_BASE >> 8) & 0xFF;
            le16put(buf + pos, aprd_lf); pos += 2;   /* LE per ETG.1000.4 */
            buf[pos++] = 0; buf[pos++] = 0;  /* IRQ */
            memset(buf + pos, 0, ESC_DIAG_LEN);      /* slaves fill this in */
            pos += ESC_DIAG_LEN;
            buf[pos++] = 0; buf[pos++] = 0;  /* WKC */
        }
    }

    /* EtherCAT header: total datagram length, type=1 */
    int ecat_payload_len = pos - ETH_HDR_LEN - ECAT_HDR_LEN;
    uint16_t ecat_hdr = (uint16_t)((ecat_payload_len & 0x07FF) | (0x1 << 12));
    le16put(buf + ETH_HDR_LEN, ecat_hdr);   /* LE per ETG.1000.4 */

    /* Self-check: pos must never exceed buflen. If it does, the overhead
     * budget above disagrees with what was actually written (exactly the class
     * of bug that caused a stack overflow when the APRD data size changed
     * without the budget). Fail loudly rather than corrupt memory further. */
    if (pos > buflen) {
        fprintf(stderr, "FATAL: build_frame wrote %d bytes into a %d-byte "
                "buffer (overhead accounting mismatch)\n", pos, buflen);
        abort();
    }

    return pos;  /* actual frame length */
}

/* ── Parse returned frame, update stats ─────────────────────────────────────
 * Returns the sequence number extracted from the NOP payload, or UINT64_MAX
 * if the frame could not be parsed / is not one of ours.
 * fcs_ok gates everything read from EtherCAT header fields whose integrity we
 * cannot independently verify: the BRD WKC mismatch count and the ESC
 * CRC/lost-link counter accumulation. A corrupt frame carries garbage in those
 * regions; one garbage 8-bit counter value poisons the delta accumulation
 * permanently. The ESC registers are cumulative in the slave, so skipping
 * corrupt frames loses nothing — the next good frame reports the same value. */
/* Payload-CRC failure: increment and bail. payload_crc_errors counts EVERY
 * received frame without a valid payload CRC — whether the CRC is invalid or
 * the payload never arrived (frame cut short / unparseable). A frame too short
 * to contain the payload cannot have a valid payload CRC, so it counts. */
static inline uint64_t pl_fail(void) {
    atomic_fetch_add_explicit(&g_stats.payload_crc_errors, 1,
                              memory_order_relaxed);
    return UINT64_MAX;
}

uint64_t parse_return_frame(const uint8_t *buf, int len,
                                   int num_slaves, int loopback,
                                   int fcs_ok, int *payload_ok) {
    if (payload_ok) *payload_ok = 0;
    if (len < ETH_HDR_LEN + ECAT_HDR_LEN) return pl_fail();

    int pos = ETH_HDR_LEN + ECAT_HDR_LEN;

    /* Datagram 0: NOP — extract sequence number from payload */
    if (pos + ECAT_DG_HDR_LEN > len) return pl_fail();

    /* FOREIGN-FRAME REJECTION. Our socket is bound to ETH_P_ECAT on a
     * promiscuous interface, so it sees EVERY EtherCAT frame on the wire —
     * including a register probe sharing the link. Every frame we build leads
     * with a NOP datagram (build_frame); a probe leads with APRD. Without this
     * check a probe reply falls through to the payload-CRC path and inflates
     * the BER numerator.
     *
     * This makes the §3.4 partition THREE-way rather than two: valid payload
     * CRC / counted in payload_crc_errors / foreign. A corrupted frame of OURS
     * is still counted correctly, because corruption is caught by the FCS
     * residual check before this point and counted in rx_bad_fcs_computed. */
    if (buf[pos] != ECAT_CMD_NOP) {
        atomic_fetch_add_explicit(&g_rx_foreign, 1, memory_order_relaxed);
        return UINT64_MAX;
    }
    uint16_t lf = le16get(buf + pos + 6);   /* LE per ETG.1000.4 */
    uint16_t dg_len = lf & 0x07FF;
    pos += ECAT_DG_HDR_LEN;
    if (pos + dg_len + ECAT_DG_WKC_LEN > len) return pl_fail();

    uint64_t ret_seq = UINT64_MAX;
    if (dg_len < PL_HDR_LEN) {
        /* Payload too small to even hold seq+CRC — no valid payload CRC. */
        atomic_fetch_add_explicit(&g_stats.payload_crc_errors, 1,
                                  memory_order_relaxed);
    } else {
        const uint8_t *pl = buf + pos;
        memcpy(&ret_seq, pl + PL_SEQ_OFF, PL_SEQ_LEN);

        /* Independent payload integrity check: recompute CRC32C over
         * (seq ++ random payload) and compare to the embedded field. This is
         * independent of the Ethernet FCS — it catches corruption that a slave
         * regenerating a valid FCS would otherwise mask. */
        uint32_t embedded;
        memcpy(&embedded, pl + PL_CRC_OFF, PL_CRC_LEN);
        int rnd_len = (int)dg_len - PL_DATA_OFF;
        if (rnd_len < 0) rnd_len = 0;
        uint32_t calc = crc32c_chain(pl + PL_SEQ_OFF, PL_SEQ_LEN,
                                     pl + PL_DATA_OFF, (size_t)rnd_len);
        if (calc != embedded)
            atomic_fetch_add_explicit(&g_stats.payload_crc_errors, 1,
                                      memory_order_relaxed);
        else if (payload_ok)
            *payload_ok = 1;   /* payload CRC32C verified good */
    }
    pos += dg_len + ECAT_DG_WKC_LEN;

    if (loopback || num_slaves == 0) return ret_seq;

    /* Datagram 1: BRD — read WKC */
    if (pos + ECAT_DG_HDR_LEN > len) return ret_seq;
    lf = le16get(buf + pos + 6);   /* LE */
    dg_len = lf & 0x07FF;
    pos += ECAT_DG_HDR_LEN + dg_len;
    if (pos + ECAT_DG_WKC_LEN > len) return ret_seq;
    uint16_t brd_wkc = le16get(buf + pos);   /* WKC is LE too */
    pos += ECAT_DG_WKC_LEN;

    if (fcs_ok && brd_wkc != (uint16_t)num_slaves) {
        atomic_fetch_add_explicit(&g_stats.brd_wkc_mismatches, 1,
                                  memory_order_relaxed);
    }

    /* Datagrams 2..N+1: APRD per slave */
    for (int s = 0; s < num_slaves && s < MAX_SLAVES; s++) {
        if (pos + ECAT_DG_HDR_LEN > len) break;
        lf = le16get(buf + pos + 6);   /* LE */
        dg_len = lf & 0x07FF;
        pos += ECAT_DG_HDR_LEN;
        if (pos + dg_len + ECAT_DG_WKC_LEN > len) break;

        uint16_t aprd_wkc = le16get(buf + pos + dg_len);   /* WKC is LE */

        /* Gated on FCS-valid frames with WKC==1 (README §3.7): a corrupt frame
         * carries garbage in these byte positions and one garbage value would
         * permanently poison the 8-bit delta accumulation. The registers are
         * cumulative in the slave, so skipping corrupt frames loses nothing. */
        if (fcs_ok && aprd_wkc == 1 && dg_len >= ESC_DIAG_LEN) {
            const uint8_t *d = buf + pos;
            for (int p = 0; p < 4; p++) {
                uint8_t cur = d[p * 2];              /* 0x0300+2p invalid    */
                g_stats.esc_crc[s][p] += (uint8_t)(cur - g_stats.esc_crc_prev[s][p]);
                g_stats.esc_crc_prev[s][p] = cur;
                g_stats.esc_raw_crc[s][p]  = cur;

                uint8_t rx = d[p * 2 + 1];           /* 0x0301+2p RX error   */
                g_stats.esc_rxerr[s][p] += (uint8_t)(rx - g_stats.esc_rxerr_prev[s][p]);
                g_stats.esc_rxerr_prev[s][p] = rx;
                g_stats.esc_raw_rxerr[s][p]  = rx;

                uint8_t fw = d[8 + p];               /* 0x0308+p forwarded   */
                g_stats.esc_fwderr[s][p] += (uint8_t)(fw - g_stats.esc_fwderr_prev[s][p]);
                g_stats.esc_fwderr_prev[s][p] = fw;
                g_stats.esc_raw_fwderr[s][p]  = fw;

                uint8_t ll = d[16 + p];              /* 0x0310+p lost link   */
                g_stats.esc_lostlnk[s][p] += (uint8_t)(ll - g_stats.esc_lostlnk_prev[s][p]);
                g_stats.esc_lostlnk_prev[s][p] = ll;

                uint8_t ex = d[20 + p];              /* 0x0314+p extended RX */
                g_stats.esc_extrx[s][p] += (uint8_t)(ex - g_stats.esc_extrx_prev[s][p]);
                g_stats.esc_extrx_prev[s][p] = ex;
                g_stats.esc_raw_extrx[s][p]  = ex;

                /* 0x0320+2p RX error code — a latched REASON, not a counter. */
                uint16_t code = le16get(d + 32 + p * 2);
                if (code) {
                    if (g_stats.esc_rxcode[s][p] != code)
                        g_stats.esc_rxcode_seen[s][p]++;
                    g_stats.esc_rxcode[s][p] = code;
                }
            }
            uint8_t pu = d[12];                      /* 0x030C proc unit     */
            g_stats.esc_puerr[s] += (uint8_t)(pu - g_stats.esc_puerr_prev[s]);
            g_stats.esc_puerr_prev[s] = pu;
            g_stats.esc_raw_puerr[s]  = pu;

            uint8_t pd = d[13];                      /* 0x030D PDI0 error    */
            g_stats.esc_pdierr[s] += (uint8_t)(pd - g_stats.esc_pdierr_prev[s]);
            g_stats.esc_pdierr_prev[s] = pd;
            g_stats.esc_raw_pdierr[s]  = pd;
        }

        pos += dg_len + ECAT_DG_WKC_LEN;
    }

    return ret_seq;
}

