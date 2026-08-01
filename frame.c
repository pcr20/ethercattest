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
    int aprd_bytes   = loopback ? 0 : (num_slaves * (ECAT_DG_OVERHEAD + 16));
    /* Second APRD set (Option C): lost-link counters 0x0310-0x0313, 4 bytes. */
    int aprd2_bytes  = loopback ? 0 : (num_slaves * (ECAT_DG_OVERHEAD + 4));
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
            /* more=1 always: the lost-link APRD set follows. */
            uint16_t aprd_lf = (uint16_t)(16 | 0x8000);
            /* APRD address: upper 16 bits = auto-increment position (negated),
             * lower 16 bits = register offset.
             * Auto-increment: slave 0 sees address 0, slave 1 sees -1, etc.
             * We encode as: addr[31:16] = (uint16_t)(-(s)), addr[15:0] = 0x0300 */
            /* APRD address: auto-increment node (negated s), register 0x0300 */
            uint16_t node_le = (uint16_t)(-(int16_t)s);
            buf[pos++] = ECAT_CMD_APRD;
            buf[pos++] = (uint8_t)s;   /* idx = slave number for easy demux */
            /* addr in little-endian: node_addr (16-bit LE), then reg (16-bit LE) */
            buf[pos++] = (node_le) & 0xFF;
            buf[pos++] = (node_le >> 8) & 0xFF;
            buf[pos++] = 0x00;   /* reg 0x0300 low byte */
            buf[pos++] = 0x03;   /* reg 0x0300 high byte */
            le16put(buf + pos, aprd_lf); pos += 2;   /* LE per ETG.1000.4 */
            buf[pos++] = 0; buf[pos++] = 0;  /* IRQ */
            memset(buf + pos, 0, 16);  /* 16 bytes data (zeroed, slaves fill in) */
            pos += 16;
            buf[pos++] = 0; buf[pos++] = 0;  /* WKC */
        }

        /* Second APRD set (Option C): per-slave lost-link counters, registers
         * 0x0310-0x0313 (four consecutive 8-bit counters, one per port; a port
         * increments its counter each time its link goes down). This is the
         * per-segment outage detector the host NIC cannot provide: a downstream
         * segment can flap without the host carrier ever changing. */
        for (int s = 0; s < num_slaves; s++) {
            int is_last = (s == num_slaves - 1);
            uint16_t aprd2_lf = (uint16_t)(4 | (is_last ? 0 : 0x8000));
            uint16_t node_le = (uint16_t)(-(int16_t)s);
            buf[pos++] = ECAT_CMD_APRD;
            buf[pos++] = (uint8_t)(0x80 | s);   /* idx: lost-link set marker */
            buf[pos++] = (node_le) & 0xFF;
            buf[pos++] = (node_le >> 8) & 0xFF;
            buf[pos++] = 0x10;   /* reg 0x0310 low byte */
            buf[pos++] = 0x03;   /* reg 0x0310 high byte */
            le16put(buf + pos, aprd2_lf); pos += 2;   /* LE */
            buf[pos++] = 0; buf[pos++] = 0;  /* IRQ */
            memset(buf + pos, 0, 4);   /* 4 bytes data */
            pos += 4;
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

        if (fcs_ok && aprd_wkc == 1 && dg_len >= 16) {
            /* Bytes 0-7: CRC error registers 0x0300-0x0307
             * Layout per ETG.1000.6:
             *   0x0300: Port0 invalid frame counter
             *   0x0301: Port0 RX error counter
             *   0x0302: Port1 invalid frame counter
             *   0x0303: Port1 RX error counter
             *   0x0304: Port2 ...
             *   0x0305: Port2 ...
             *   0x0306: Port3 ...
             *   0x0307: Port3 ...
             * Bytes 8-15 cover 0x0308-0x030F (forwarded error counters etc.)
             * Lost link at 0x0310-0x0313 NOT in this read (we'd need another APRD)
             * For now we track the 4 invalid-frame counters */
            for (int p = 0; p < 4; p++) {
                uint8_t cur = buf[pos + p * 2];  /* invalid frame counter for port p */
                uint8_t prev = g_stats.esc_crc_prev[s][p];
                uint8_t delta = (uint8_t)(cur - prev);  /* handles 8-bit wrap */
                g_stats.esc_crc[s][p] += delta;
                g_stats.esc_crc_prev[s][p] = cur;
            }
        }

        pos += dg_len + ECAT_DG_WKC_LEN;
    }

    /* Second APRD set (Option C): per-slave lost-link counters 0x0310-0x0313.
     * Four consecutive 8-bit counters, one per port; each increments when that
     * port's link goes down. Same gating as the CRC set: only trust data from
     * FCS-valid frames with WKC==1. Counters are cumulative in the slave, so
     * skipped (corrupt) frames lose nothing. */
    for (int s = 0; s < num_slaves && s < MAX_SLAVES; s++) {
        if (pos + ECAT_DG_HDR_LEN > len) break;
        lf = le16get(buf + pos + 6);   /* LE */
        dg_len = lf & 0x07FF;
        pos += ECAT_DG_HDR_LEN;
        if (pos + dg_len + ECAT_DG_WKC_LEN > len) break;

        uint16_t ll_wkc = le16get(buf + pos + dg_len);   /* WKC is LE */
        if (fcs_ok && ll_wkc == 1 && dg_len >= 4) {
            for (int p = 0; p < 4; p++) {
                uint8_t cur  = buf[pos + p];   /* lost-link counter, port p */
                uint8_t prev = g_stats.esc_lostlnk_prev[s][p];
                uint8_t delta = (uint8_t)(cur - prev);  /* 8-bit wrap */
                g_stats.esc_lostlnk[s][p] += delta;
                g_stats.esc_lostlnk_prev[s][p] = cur;
            }
        }

        pos += dg_len + ECAT_DG_WKC_LEN;
    }

    return ret_seq;
}

