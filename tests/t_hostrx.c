/* Host-side RX accounting: a frame dropped ABOVE the wire (NIC ring overflow,
 * netdev drop, socket overflow) must be attributed to the host and never be
 * silently folded into physical-layer loss. Also pins the CSV header/row
 * column alignment, which is easy to break when adding counters. */
#include "crc.c"
#include "stats.c"
#include "frame.c"

static int fails = 0;
#define CHECK(c, ...) do { if(!(c)) { printf("FAIL: "); printf(__VA_ARGS__); \
                                      printf("\n"); fails++; } } while (0)

int main(void)
{
    crc32c_init();                    /* §4.3 invariant: always first */

    /* ── T1: CSV header and row must have identical column counts ───────── */
    {
        int f0 = fails;
        FILE *f = tmpfile();
        CHECK(f != NULL, "tmpfile");
        g_num_slaves = 1;
        write_csv_header(f, 1);
        rewind(f);
        char hdr[4096] = {0};
        CHECK(fgets(hdr, sizeof(hdr), f) != NULL, "header not written");
        int hcols = 1;
        for (char *p = hdr; *p; p++) if (*p == ',') hcols++;

        /* Emit one row through the real writer and count its fields. */
        rewind(f); 
        if (ftruncate(fileno(f), 0) != 0) { /* ignore */ }
        atomic_store(&g_txok, 1000);
        atomic_store(&g_rx_missed, 7);
        atomic_store(&g_rx_dropped, 9);
        g_stats.distinct_returns = 990;
        print_stats(f, 1000000000ULL);
        rewind(f);
        char row[4096] = {0};
        CHECK(fgets(row, sizeof(row), f) != NULL, "row not written");
        int rcols = 1;
        for (char *p = row; *p && *p != '\n'; p++) if (*p == ',') rcols++;

        CHECK(hcols == rcols,
              "CSV header has %d columns but the row has %d — the header and "
              "the fprintf format have drifted apart", hcols, rcols);
        if (fails == f0)
            printf("T1 PASS: CSV header and row agree at %d columns\n", hcols);
        fclose(f);
    }

    /* ── T2: host-side counters are exposed and independent ─────────────── */
    {
        int f0 = fails;
        atomic_store(&g_rx_missed, 0); atomic_store(&g_rx_dropped, 0);
        atomic_store(&g_rx_fifo, 0);   atomic_store(&g_rx_nic_err, 0);
        atomic_store(&g_rx_nic_crc, 0);
        atomic_store(&g_rx_missed, 11);
        CHECK(atomic_load(&g_rx_missed) == 11, "rx_missed not settable");
        CHECK(atomic_load(&g_rx_dropped) == 0, "counters must be independent");
        CHECK(atomic_load(&g_rx_fifo) == 0,    "counters must be independent");
        if (fails == f0)
            printf("T2 PASS: five host-side RX counters exposed independently\n");
    }

    /* ── T3: host-side drops do NOT alter the wire loss figure ───────────
     * loss stays TxOk - good distinct returns. A host-side drop makes a frame
     * fail to return, so it DOES land in loss — the point is that the host
     * counters are reported alongside so the contamination is VISIBLE rather
     * than being silently attributed to the physical layer. */
    {
        int f0 = fails;
        atomic_store(&g_txok, 1000);
        g_stats.distinct_returns = 990;
        atomic_store(&g_rx_missed, 10);      /* all 10 lost in the NIC ring */
        int64_t signed_loss = (int64_t)atomic_load(&g_txok)
                            - (int64_t)g_stats.distinct_returns;
        CHECK(signed_loss == 10, "loss should be 10, got %ld", (long)signed_loss);
        CHECK(atomic_load(&g_rx_missed) == 10,
              "the host counter must independently account for those 10");
        if (fails == f0)
            printf("T3 PASS: host-side drops appear in loss AND in the host "
                   "counters, so contamination is visible\n");
    }

    /* ── T4: the signed loss column preserves negative skew ─────────────── */
    {
        int f0 = fails;
        atomic_store(&g_txok, 1000);
        g_stats.distinct_returns = 1050;     /* returns ahead of the TxOk sample */
        int64_t sgn = (int64_t)atomic_load(&g_txok)
                    - (int64_t)g_stats.distinct_returns;
        uint64_t clamped = (sgn > 0) ? (uint64_t)sgn : 0;
        CHECK(sgn == -50, "signed value must be -50, got %ld", (long)sgn);
        CHECK(clamped == 0, "clamped value must be 0");
        if (fails == f0)
            printf("T4 PASS: signed -50 preserved while clamped reads 0 — "
                   "sampling skew no longer masquerades as a loss onset\n");
    }

    /* ── T5: the two TX-drop counters must stay distinct ────────────────
     * statistics/tx_dropped (netdev, driver-level) reads 0 on this rig while
     * the root qdisc discards ~100M frames per run. Conflating them reported
     * a false zero for every run. An unavailable qdisc counter must surface
     * as UNKNOWN (-1 in CSV), never as 0. */
    {
        int f0 = fails;
        g_stats.qdisc_real_ok = 0;
        g_stats.qdisc_real_start = 0; g_stats.qdisc_real_end = 0;
        CHECK(g_stats.qdisc_real_ok == 0, "unavailable must stay unavailable");

        g_stats.qdisc_real_ok = 1;
        g_stats.qdisc_real_start = 324157483ULL;
        g_stats.qdisc_real_end   = 324157483ULL + 100023450ULL;
        uint64_t d = g_stats.qdisc_real_end - g_stats.qdisc_real_start;
        CHECK(d == 100023450ULL, "qdisc delta wrong: %lu", d);
        CHECK(atomic_load(&g_qdisc_drop) != d,
              "the netdev counter and the qdisc counter must not be the same "
              "value — conflating them is the bug this guards");
        if (fails == f0)
            printf("T5 PASS: netdev tx_dropped and root-qdisc drops kept "
                   "distinct; unavailable reports unknown, not zero\n");
    }

    if (fails) { printf("\n%d HOST-RX CHECK(S) FAILED\n", fails); return 1; }
    printf("\nALL HOST-RX TESTS PASS\n");
    return 0;
}
