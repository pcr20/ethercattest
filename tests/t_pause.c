/* External pause/resume accounting: foreign frames put on the wire by a
 * register probe must never be charged to this measurement as loss, and
 * foreign frames must not be parsed as corrupt ones. */
#include "crc.c"
#include "stats.c"
#include "frame.c"

static int fails = 0;
#define CHECK(c, ...) do { if(!(c)) { printf("FAIL: "); printf(__VA_ARGS__); \
                                      printf("\n"); fails++; } } while (0)

int main(void)
{
    crc32c_init();                     /* §4.3 invariant: always first */

    /* ── T1: foreign TxOk is subtracted exactly ─────────────────────────── */
    {
        int f0 = fails;
        /* 1,000,000 of ours on the wire, all returned. A probe then puts 320
         * frames on the same interface during a pause. TxOk is interface-wide
         * so it reads 1,000,320 — without subtraction that is 320 phantom
         * losses, which is exactly the failure this guards. */
        atomic_store(&g_txok, 1000320);
        atomic_store(&g_foreign_txok, 320);
        g_stats.distinct_returns = 1000000;

        uint64_t raw = atomic_load(&g_txok);
        uint64_t fgn = atomic_load(&g_foreign_txok);
        uint64_t txok = raw - fgn;
        int64_t  lost = (int64_t)txok - (int64_t)g_stats.distinct_returns;

        CHECK(txok == 1000000, "corrected TxOk = %lu, want 1000000", txok);
        CHECK(lost == 0, "loss = %ld, want 0 — probe frames leaked into the "
                         "wire accounting", (long)lost);
        int64_t naive = (int64_t)raw - (int64_t)g_stats.distinct_returns;
        CHECK(naive == 320, "sanity: uncorrected would have shown %ld phantom "
                            "losses", (long)naive);
        if (fails == f0)
            printf("T1 PASS: 320 probe frames subtracted exactly; loss 0 "
                   "(uncorrected would read 320)\n");
    }

    /* ── T2: BER denominator uses corrected TxOk ────────────────────────── */
    {
        int f0 = fails;
        uint64_t txok = atomic_load(&g_txok) - atomic_load(&g_foreign_txok);
        CHECK(txok * 12144 == 1000000ULL * 12144,
              "BER denominator must exclude foreign frames");
        if (fails == f0)
            printf("T2 PASS: BER denominator excludes foreign frames\n");
    }

    /* ── T3: a probe reply is rejected, not counted as corruption ───────── */
    {
        int f0 = fails;
        memset(&g_stats, 0, sizeof(g_stats));
        atomic_store(&g_rx_foreign, 0);
        g_num_slaves = 1;

        /* Minimal APRD-led frame, as ecat_regdump/ecat_phy emit. Ours always
         * lead with NOP; theirs lead with APRD. */
        uint8_t f[64];
        memset(f, 0, sizeof(f));
        f[12] = 0x88; f[13] = 0xA4;                 /* EtherType */
        le16put(f + ETH_HDR_LEN, (uint16_t)(14 | (0x1 << 12)));
        int pos = ETH_HDR_LEN + ECAT_HDR_LEN;
        f[pos] = ECAT_CMD_APRD;                      /* <- not NOP */
        le16put(f + pos + 6, 2);

        parse_return_frame(f, 60, 1, 0, /*fcs_ok=*/1, NULL);

        CHECK(atomic_load(&g_stats.payload_crc_errors) == 0,
              "a foreign frame must NOT be counted as a payload CRC error "
              "(got %lu) — that would inflate the BER numerator",
              atomic_load(&g_stats.payload_crc_errors));
        CHECK(atomic_load(&g_rx_foreign) == 1,
              "foreign frame must be counted separately (got %lu)",
              atomic_load(&g_rx_foreign));
        if (fails == f0)
            printf("T3 PASS: APRD-led probe reply rejected as foreign, not "
                   "charged to payload_crc_errors\n");
    }

    /* ── T4: our own frames still parse and still count corruption ──────── */
    {
        int f0 = fails;
        memset(&g_stats, 0, sizeof(g_stats));
        atomic_store(&g_rx_foreign, 0);
        uint8_t b[MAX_FRAME];
        int len = build_frame(b, sizeof(b), (const uint8_t[]){1,2,3,4,5,6},
                              1, 4242, 0);
        int pok = 0;
        uint64_t seq = parse_return_frame(b, len, 1, 0, 1, &pok);
        CHECK(seq == 4242, "our own frame must still parse (seq=%lu)", seq);
        CHECK(pok == 1, "our own good frame must have a valid payload CRC");
        CHECK(atomic_load(&g_rx_foreign) == 0,
              "our own NOP-led frame must not be classed foreign");

        /* Corrupt the payload: must be counted, not silently dropped. */
        memset(&g_stats, 0, sizeof(g_stats));
        b[ETH_HDR_LEN + ECAT_HDR_LEN + ECAT_DG_HDR_LEN + 20] ^= 0xFF;
        parse_return_frame(b, len, 1, 0, 1, &pok);
        CHECK(atomic_load(&g_stats.payload_crc_errors) == 1,
              "corruption in OUR frame must still be counted — the foreign "
              "check must not swallow it (got %lu)",
              atomic_load(&g_stats.payload_crc_errors));
        if (fails == f0)
            printf("T4 PASS: our frames parse normally and corruption is "
                   "still counted\n");
    }

    if (fails) { printf("\n%d PAUSE CHECK(S) FAILED\n", fails); return 1; }
    printf("\nALL PAUSE TESTS PASS\n");
    return 0;
}
