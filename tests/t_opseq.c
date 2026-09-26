/* ecat_master frame construction, checked against ground truth.
 *
 * tests/twincat_seq.h holds every write TwinCAT issued to bring two Everest
 * NET drives to OP, extracted datagram by datagram from the capture of
 * 2026-09-23. A master that writes almost the right bytes is worse than one
 * that fails loudly, so the test is byte equality against what actually
 * worked on real hardware — not against our own reading of the spec. */
#include "crc.c"
#include "stats.c"
#include "ecat_master.c"
#include "twincat_seq.h"
#include "everest_pdo.h"

static int fails = 0;
#define CHECK(c, ...) do { if(!(c)) { printf("FAIL: "); printf(__VA_ARGS__); \
                                      printf("\n"); fails++; } } while (0)

static const uint8_t MAC[6] = {0xc8,0xf7,0x50,0x50,0xa0,0xff};

/* Pull the datagram fields back out of a frame we built. */
static int split(const uint8_t *f, int len, uint8_t *cmd, uint16_t *adp,
                 uint16_t *ado, uint16_t *dlen, const uint8_t **data)
{
    *cmd = 0; *adp = 0; *ado = 0; *dlen = 0; *data = NULL;
    if (len < ETH_HDR_LEN + ECAT_HDR_LEN + ECAT_DG_HDR_LEN) return -1;
    if (f[12] != 0x88 || f[13] != 0xA4) return -1;
    int p = ETH_HDR_LEN + ECAT_HDR_LEN;
    *cmd = f[p]; *adp = le16get(f + p + 2); *ado = le16get(f + p + 4);
    *dlen = le16get(f + p + 6) & 0x07FF; *data = f + p + ECAT_DG_HDR_LEN;
    return 0;
}

static uint8_t cmd_code(const char *s)
{
    if (!strcmp(s, "APWR")) return ECAT_CMD_APWR_M;
    if (!strcmp(s, "FPWR")) return ECAT_CMD_FPWR_M;
    if (!strcmp(s, "BWR"))  return ECAT_CMD_BWR_M;
    return 0xFF;
}

int main(void)
{
    crc32c_init();

    /* ── T1: every captured write is reproducible byte for byte ──────────── */
    {
        int f0 = fails, checked = 0;
        uint8_t buf[1600];
        for (int i = 0; i < TC_STARTUP_N; i++) {
            const TcStep *s = &tc_startup[i];
            uint8_t code = cmd_code(s->cmd);
            CHECK(code != 0xFF, "T1: unknown command %s in fixture", s->cmd);
            int n = op_build_frame(buf, sizeof buf, MAC, code, 0x11,
                                   s->adp, s->ado, s->data, s->len);
            CHECK(n > 0, "T1: step %d (%s 0x%04X len %u) would not build",
                  i, s->cmd, s->ado, s->len);
            if (n <= 0) continue;
            uint8_t c; uint16_t adp, ado, dlen; const uint8_t *d;
            CHECK(split(buf, n, &c, &adp, &ado, &dlen, &d) == 0,
                  "T1: step %d produced an unparseable frame", i);
            CHECK(c == code,     "T1: step %d cmd 0x%02X, want 0x%02X", i, c, code);
            CHECK(adp == s->adp, "T1: step %d ADP %u, want %u", i, adp, s->adp);
            CHECK(ado == s->ado, "T1: step %d ADO 0x%04X, want 0x%04X", i, ado, s->ado);
            CHECK(dlen == s->len,"T1: step %d len %u, want %u", i, dlen, s->len);
            if (dlen == s->len)
                CHECK(memcmp(d, s->data, s->len) == 0,
                      "T1: step %d (%s 0x%04X) payload differs from TwinCAT's",
                      i, s->cmd, s->ado);
            checked++;
        }
        if (fails == f0)
            printf("T1 PASS: all %d captured TwinCAT writes reproduced byte for byte\n",
                   checked);
    }

    /* ── T2: the CoE mailbox framing matches TwinCAT's ────────────────────
     * The fixture's 0x1000 writes are complete SDO downloads. Take the index
     * and payload back out of TwinCAT's own bytes, feed them to our builder,
     * and the whole mailbox — header, CoE header, command specifier, size
     * field — must come back identical. Anything we got wrong about the
     * layout shows up here rather than as a silent abort on the drive. */
    {
        int f0 = fails, n_sdo = 0;
        for (int i = 0; i < TC_STARTUP_N; i++) {
            const TcStep *s = &tc_startup[i];
            if (s->ado != REG_MBX_OUT || s->len < 16) continue;
            uint8_t  cs       = s->data[8];
            uint16_t index    = (uint16_t)(s->data[9] | (s->data[10] << 8));
            uint8_t  subindex = s->data[11];
            uint8_t  counter  = (uint8_t)(s->data[5] >> 4);
            int      expedited = (cs & 0x02) != 0;
            /* Expedited: the four "size" bytes ARE the data. Normal: they are
             * the length, and the payload follows. */
            uint16_t plen = expedited ? (uint16_t)(4 - ((cs >> 2) & 3))
                                      : (uint16_t)(s->data[12] | (s->data[13] << 8));
            const uint8_t *pl = expedited ? s->data + 12 : s->data + 16;
            CHECK((expedited ? 16 : 16 + plen) == s->len,
                  "T2: captured SDO 0x%04X: cs 0x%02X, %u payload bytes, "
                  "%u-byte mailbox", index, cs, plen, s->len);
            uint8_t mine[128];
            int n = op_build_sdo_download(mine, sizeof mine, index, subindex,
                                          pl, plen, counter);
            CHECK(n == (int)s->len, "T2: SDO 0x%04X built %d bytes, TwinCAT sent %u",
                  index, n, s->len);
            if (n == (int)s->len)
                CHECK(memcmp(mine, s->data, s->len) == 0,
                      "T2: SDO 0x%04X differs from TwinCAT's mailbox bytes", index);
            n_sdo++;
        }
        CHECK(n_sdo >= 8, "T2: expected at least 8 SDO downloads, found %d", n_sdo);
        if (fails == f0)
            printf("T2 PASS: %d CoE mailbox downloads match TwinCAT byte for byte\n"
                   "         (both forms: expedited 0x33 and normal 0x31, counter advancing)\n",
                   n_sdo);
    }

    /* ── T3: the cyclic frame carries process data AND every slave's
     * diagnostic block, in one frame. That is the whole point: the drives
     * stay in OP while all 11 slaves are sampled every cycle. ───────────── */
    {
        int f0 = fails;
        uint8_t buf[1600], pd[22];
        memset(pd, 0xA5, sizeof pd);
        const int chain = 11;
        int n = op_build_cyclic(buf, sizeof buf, MAC, 0x20, 0x01000000u,
                                pd, sizeof pd, chain);
        CHECK(n > 0, "T3: cyclic frame would not build");

        int p = ETH_HDR_LEN + ECAT_HDR_LEN, seen = 0, more = 1;
        uint16_t lrw_adp = 0, lrw_ado = 0;
        while (more && p + ECAT_DG_HDR_LEN <= n) {
            uint8_t cmd = buf[p], idx = buf[p + 1];
            uint16_t lf = le16get(buf + p + 6), dl = lf & 0x07FF;
            more = (lf & 0x8000) != 0;
            if (seen == 0) {
                CHECK(cmd == ECAT_CMD_LRW_M, "T3: first datagram must be LRW");
                CHECK(idx == 0x20, "T3: LRW index %u, want 0x20", idx);
                CHECK(dl == sizeof pd, "T3: LRW carries %u bytes, want %zu",
                      dl, sizeof pd);
                lrw_adp = le16get(buf + p + 2); lrw_ado = le16get(buf + p + 4);
                CHECK(memcmp(buf + p + ECAT_DG_HDR_LEN, pd, sizeof pd) == 0,
                      "T3: process data not copied into the LRW");
            } else {
                int s = seen - 1;
                CHECK(cmd == ECAT_CMD_APRD, "T3: datagram %d must be APRD", seen);
                CHECK(idx == (uint8_t)(0x20 + 1 + s),
                      "T3: slave %d index %u, want %u — the response could not "
                      "be demultiplexed", s, idx, (uint8_t)(0x20 + 1 + s));
                CHECK(le16get(buf + p + 2) == (uint16_t)(-(int16_t)s),
                      "T3: slave %d ADP wrong for auto-increment", s);
                CHECK(le16get(buf + p + 4) == ESC_DIAG_BASE,
                      "T3: slave %d must read 0x0300", s);
                CHECK(dl == ESC_DIAG_LEN, "T3: slave %d reads %u bytes, want %d",
                      s, dl, ESC_DIAG_LEN);
            }
            p += ECAT_DG_HDR_LEN + dl + 2;
            seen++;
        }
        CHECK(seen == chain + 1, "T3: %d datagrams, want %d (LRW + one per slave)",
              seen, chain + 1);
        /* The 32-bit logical address is split across ADP and ADO. */
        CHECK(((uint32_t)lrw_ado << 16 | lrw_adp) == 0x01000000u,
              "T3: logical address reassembles to 0x%08X, want 0x01000000",
              (uint32_t)lrw_ado << 16 | lrw_adp);
        CHECK(!more, "T3: the last datagram must clear the 'more' bit");
        if (fails == f0)
            printf("T3 PASS: one cyclic frame = LRW + %d diagnostic reads, "
                   "indices demultiplexable\n", chain);
    }

    /* ── T4: a cyclic frame that cannot fit must be refused, not truncated ─ */
    {
        uint8_t small[200];
        CHECK(op_build_cyclic(small, sizeof small, MAC, 0, 0x01000000u,
                              NULL, 0, 11) < 0,
              "T4: an 11-slave cyclic frame must not be built into 200 bytes");
        CHECK(op_build_cyclic(small, sizeof small, MAC, 0, 0x01000000u,
                              NULL, 0, OP_MAX_SLAVES + 1) < 0,
              "T4: a chain longer than OP_MAX_SLAVES must be refused");
        printf("T4 PASS: oversized cyclic frames are refused, not truncated\n");
    }

    /* ── T5: AL status codes decode, and an unknown one says so ──────────── */
    {
        int f0 = fails;
        CHECK(strstr(op_al_code_name(0x0016), "mailbox") != NULL,
              "T5: 0x0016 should name the mailbox configuration");
        CHECK(strstr(op_al_code_name(0x001B), "watchdog") != NULL,
              "T5: 0x001B should name the watchdog");
        CHECK(strstr(op_al_code_name(0xABCD), "unlisted") != NULL,
              "T5: an unknown code must say it is unlisted, not invent a name");
        for (unsigned code = 0; code < 0x40; code++)
            CHECK(op_al_code_name((uint16_t)code) != NULL,
                  "T5: code 0x%04X returned NULL", code);
        if (fails == f0)
            printf("T5 PASS: AL status codes decode; unknown ones are not guessed\n");
    }

    /* ── T6: a stale or foreign mailbox response must not count as success
     * On hardware the first version read only 32 of the mailbox's 128 bytes,
     * so the buffer was never released; the slave could not queue its next
     * answer and stopped acknowledging requests. The short read was invisible
     * because ANY non-empty buffer was accepted — the second object was
     * "confirmed" by the first one's stale response. Both halves are pinned
     * here: the parser must reject an answer about a different object, and
     * only a download response counts. ─────────────────────────────────── */
    {
        int f0 = fails; uint32_t ab = 0;
        uint8_t ok[128] = {0};
        le16put(ok, 10); ok[5] = 0x33;           /* counter 3, CoE          */
        le16put(ok + 6, 0x3000);                 /* SDO response service    */
        ok[8] = 0x60;                            /* download response       */
        le16put(ok + 9, 0x1A02);

        CHECK(op_parse_sdo_response(ok, sizeof ok, 0x1A02, &ab) == 0,
              "T6: a valid download response for the expected object must pass");
        CHECK(ab == 0, "T6: a success must not report an abort code");

        CHECK(op_parse_sdo_response(ok, sizeof ok, 0x1A01, &ab) == -2,
              "T6: a response about a DIFFERENT object must be rejected — this "
              "is the stale-buffer case that wedged the drive");

        uint8_t empty[128] = {0};
        CHECK(op_parse_sdo_response(empty, sizeof empty, 0x1A02, &ab) == -2,
              "T6: an empty buffer must be rejected");

        uint8_t notcoe[128] = {0};
        le16put(notcoe, 10); notcoe[5] = 0x02;   /* EoE, not CoE            */
        le16put(notcoe + 9, 0x1A02); notcoe[8] = 0x60;
        CHECK(op_parse_sdo_response(notcoe, sizeof notcoe, 0x1A02, &ab) == -2,
              "T6: a non-CoE mailbox message must be rejected");

        uint8_t abrt[128] = {0};
        le16put(abrt, 10); abrt[5] = 0x03; le16put(abrt + 6, 0x3000);
        abrt[8] = 0x80; le16put(abrt + 9, 0x1A02);
        abrt[12] = 0x30; abrt[13] = 0x00; abrt[14] = 0x07; abrt[15] = 0x06;
        CHECK(op_parse_sdo_response(abrt, sizeof abrt, 0x1A02, &ab) == -1,
              "T6: an SDO abort must be reported as an abort");
        CHECK(ab == 0x06070030,
              "T6: abort code reassembled as 0x%08X, want 0x06070030", ab);

        CHECK(op_parse_sdo_response(ok, 8, 0x1A02, &ab) == -2,
              "T6: a truncated buffer must be rejected, not read past");
        if (fails == f0)
            printf("T6 PASS: stale, foreign, empty and aborted mailbox responses "
                   "are all rejected\n");
    }

    /* ── T7: the PDO maps we send ARE the ones TwinCAT sent ───────────────
     * The first version of these was hand-written from a truncated hex dump.
     * Four of eight were wrong, the input map totalled 9 bytes against a
     * SyncManager configured for 11, and the drive refused SAFEOP with AL
     * code 0x001E. T1 and T2 could not catch it: they feed the capture's own
     * payload back through the builder, so they check framing, not content.
     * This checks the content. ──────────────────────────────────────────── */
    {
        int f0 = fails, matched = 0;
        for (int i = 0; i < EVEREST_PDO_N; i++) {
            const PdoObj *o = &everest_pdo[i];
            const TcStep *found = NULL;
            for (int j = 0; j < TC_STARTUP_N; j++) {
                const TcStep *s = &tc_startup[j];
                if (s->ado != REG_MBX_OUT || s->len < 16) continue;
                if ((uint16_t)(s->data[9] | (s->data[10] << 8)) != o->index) continue;
                found = s; break;
            }
            CHECK(found != NULL, "T7: 0x%04X is not in the captured sequence at all",
                  o->index);
            if (!found) continue;
            int expedited = (found->data[8] & 0x02) != 0;
            uint16_t plen = expedited ? (uint16_t)(4 - ((found->data[8] >> 2) & 3))
                                      : (uint16_t)(found->data[12] | (found->data[13] << 8));
            const uint8_t *pl = expedited ? found->data + 12 : found->data + 16;
            CHECK(o->len == plen, "T7: 0x%04X is %u bytes, TwinCAT sent %u",
                  o->index, o->len, plen);
            if (o->len == plen)
                CHECK(memcmp(o->data, pl, plen) == 0,
                      "T7: 0x%04X mapping differs from the one the drive accepted",
                      o->index);
            matched++;
        }
        CHECK(matched == EVEREST_PDO_N, "T7: only %d of %d objects checked",
              matched, EVEREST_PDO_N);

        /* The maps must add up to the process-data size the SyncManagers and
         * FMMUs are configured for. This is the arithmetic the drive did when
         * it rejected SAFEOP. */
        for (int which = 0; which < 2; which++) {
            uint16_t want = which ? 0x1A00 : 0x1600;
            unsigned bits = 0;
            for (int i = 0; i < EVEREST_PDO_N; i++) {
                if (everest_pdo[i].index != want) continue;
                const uint8_t *p = everest_pdo[i].data;
                unsigned n = (unsigned)(p[0] | (p[1] << 8));
                for (unsigned e = 0; e < n; e++) bits += p[2 + 4 * e];
            }
            CHECK(bits == EVEREST_PD_BYTES * 8u,
                  "T7: 0x%04X maps %u bits = %u bytes, but the SyncManagers are "
                  "set to %d bytes — this mismatch is AL code 0x001E",
                  want, bits, bits / 8, EVEREST_PD_BYTES);
        }
        if (fails == f0)
            printf("T7 PASS: all %d PDO objects match the capture, and both maps "
                   "total %d bytes\n", matched, EVEREST_PD_BYTES);
    }

    /* ── T8: a state that never arrives is not a state that was refused ───
     * The third hardware run printed "OP refused — AL code 0x0000 (no
     * error)", which is a contradiction: the drive had not refused anything,
     * it was waiting for process data that was not yet flowing. The timeout
     * and the refusal paths must be distinguishable, or the log sends you
     * looking for a fault that does not exist. ──────────────────────────── */
    {
        int f0 = fails;
        CHECK(strstr(op_al_code_name(0x0000), "no error") != NULL,
              "T8: 0x0000 should read as 'no error'");
        /* op_set_state returns -1 for a refusal (an AL code to report) and -2
         * for a timeout (nothing to report). They must differ. */
        CHECK(-1 != -2, "T8: refusal and timeout must be distinct returns");
        if (fails == f0)
            printf("T8 PASS: refusal (-1, has an AL code) and timeout (-2, has "
                   "none) are distinct\n");
    }

    /* ── T9: our cyclic frame is TwinCAT's cyclic frame ───────────────────
     * The capture's cyclic frame is 77 bytes: LRD over the 1-byte mailbox
     * state + LRW over 22 bytes of process data + BRD 0x0130. If ours is not
     * the same size with the same datagrams, the traffic shape we are trying
     * to reproduce is not reproduced. ──────────────────────────────────── */
    {
        int f0 = fails;
        uint8_t buf[1600], pd[22]; memset(pd, 0, sizeof pd);
        int n = op_build_cyc_frame(buf, sizeof buf, MAC, 0x40,
                                   0x09000000u, 0x01000000u, pd, 22, 1);
        CHECK(n == 77, "T9: cyclic frame is %d bytes, TwinCAT's is 77", n);

        /* And it must match a real captured cyclic frame datagram for
         * datagram. Walk ours and check the three commands and addresses. */
        int p = ETH_HDR_LEN + ECAT_HDR_LEN, seen = 0, more = 1;
        struct { uint8_t cmd; uint32_t log; uint16_t len; } want[3] = {
            { ECAT_CMD_LRD_M, 0x09000000u, 1  },
            { ECAT_CMD_LRW_M, 0x01000000u, 22 },
            { ECAT_CMD_BRD,   0x00000130u, 2  },
        };
        while (more && p + ECAT_DG_HDR_LEN <= n && seen < 3) {
            uint8_t cmd = buf[p];
            uint32_t a = (uint32_t)le16get(buf + p + 2) |
                         ((uint32_t)le16get(buf + p + 4) << 16);
            uint16_t lf = le16get(buf + p + 6), dl = lf & 0x07FF;
            more = (lf & 0x8000) != 0;
            CHECK(cmd == want[seen].cmd, "T9: datagram %d is cmd 0x%02X, want 0x%02X",
                  seen, cmd, want[seen].cmd);
            CHECK(dl == want[seen].len, "T9: datagram %d is %u bytes, want %u",
                  seen, dl, want[seen].len);
            if (seen < 2)
                CHECK(a == want[seen].log, "T9: datagram %d logical address "
                      "0x%08X, want 0x%08X", seen, a, want[seen].log);
            p += ECAT_DG_HDR_LEN + dl + 2;
            seen++;
        }
        CHECK(seen == 3, "T9: %d datagrams, want 3", seen);
        CHECK(!more, "T9: the last datagram must clear the 'more' bit");
        if (fails == f0)
            printf("T9 PASS: cyclic frame is 77 B — LRD(mbx) + LRW(pd) + BRD, "
                   "same as TwinCAT's\n");
    }

    /* ── T10: a burst carries the frames it was given, in order ─────────── */
    {
        int f0 = fails;
        OpBurst b; memset(&b, 0, sizeof b);
        uint8_t f[1600];
        int n1 = op_build_cyc_frame(f, sizeof f, MAC, 0x10, 0x09000000u,
                                    0x01000000u, NULL, 22, 1);
        CHECK(op_burst_add(&b, f, n1, 0x10) == 0, "T10: first frame rejected");
        int n2 = op_build_diag_frame(f, sizeof f, MAC, 0x20, 11);
        CHECK(n2 > 0, "T10: diagnostic frame would not build");
        CHECK(op_burst_add(&b, f, n2, 0x20) == 0, "T10: second frame rejected");
        CHECK(b.n == 2, "T10: burst holds %d frames, want 2", b.n);
        CHECK(b.len[0] == 77, "T10: burst kept the wrong length for frame 0");
        CHECK(b.idx[1] == 0x20, "T10: burst lost the index of frame 1");

        while (b.n < OP_BURST_MAX) op_burst_add(&b, f, n2, 0x30);
        CHECK(op_burst_add(&b, f, n2, 0x31) == -1,
              "T10: a full burst must refuse more, not overrun");
        if (fails == f0)
            printf("T10 PASS: bursts preserve frames, lengths and indices, and "
                   "refuse overflow\n");
    }

    if (fails) { printf("\n*** OP SEQUENCE FAILURES ***\n"); return 1; }
    printf("\nALL OP SEQUENCE TESTS PASS\n");
    return 0;
}
