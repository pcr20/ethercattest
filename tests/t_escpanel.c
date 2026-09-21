/* esc_slave_clean(): the predicate that decides whether a slave's ESC block is
 * printed or collapsed into the "recorded nothing" summary line.
 *
 * The property that matters: "clean" must mean the block would have printed
 * nothing of substance. A false positive silently hides a real error from an
 * unattended run — which is exactly the failure mode that hiding the whole
 * panel behind -v produced. */
#include "crc.c"
#include "stats.c"

static int fails = 0;
#define CHECK(c, ...) do { if(!(c)) { printf("FAIL: "); printf(__VA_ARGS__); \
                                      printf("\n"); fails++; } } while (0)

static void reset(void) { memset(&g_stats, 0, sizeof(g_stats)); g_num_slaves = 4; }

int main(void)
{
    crc32c_init();

    /* ── T1: a chain that saw nothing is clean ───────────────────────────── */
    reset();
    for (int s = 0; s < 4; s++)
        CHECK(esc_slave_clean(s), "T1: untouched slave %d must be clean", s);
    CHECK(!esc_any_lostlink(), "T1: no lost links on an untouched chain");
    printf("T1 PASS: an untouched chain is clean on every slave\n");

    /* ── T2: every session counter breaks cleanliness, on every port ─────── */
    {
        struct { const char *name; uint64_t (*arr)[4]; } c64[] = {
            { "invalid",  g_stats.esc_crc     }, { "lostlnk", g_stats.esc_lostlnk },
            { "rxerr",    g_stats.esc_rxerr   }, { "fwderr",  g_stats.esc_fwderr  },
            { "extrx",    g_stats.esc_extrx   },
        };
        for (size_t i = 0; i < sizeof c64 / sizeof *c64; i++)
            for (int p = 0; p < 4; p++) {
                reset(); c64[i].arr[2][p] = 1;
                CHECK(!esc_slave_clean(2), "T2: %s on port %d must not be clean",
                      c64[i].name, p);
                CHECK(esc_slave_clean(1), "T2: %s on slave 2 must not dirty slave 1",
                      c64[i].name);
            }
        for (int p = 0; p < 4; p++) {
            reset(); g_stats.esc_rxcode[2][p] = 0x0001;
            CHECK(!esc_slave_clean(2), "T2: rxcode on port %d must not be clean", p);
        }
        reset(); g_stats.esc_puerr[2] = 1;
        CHECK(!esc_slave_clean(2), "T2: processing-unit error must not be clean");
        reset(); g_stats.esc_pdierr[2] = 1;
        CHECK(!esc_slave_clean(2), "T2: PDI error must not be clean");
    }
    printf("T2 PASS: every session counter, on every port, breaks cleanliness\n");

    /* ── T3: pre-existing power-on history is not counted in the session
     * totals, so only this predicate can keep it on screen ──────────────── */
    {
        reset(); g_stats.esc_base_crc[0][1] = 7;
        CHECK(!esc_slave_clean(0), "T3: pre-existing invalid-frame history must show");
        reset(); g_stats.esc_base_lost[3][0] = 2;
        CHECK(!esc_slave_clean(3), "T3: pre-existing lost-link history must show");
        reset(); g_stats.esc_base_rxcode[1][2] = 0x0002;
        CHECK(!esc_slave_clean(1), "T3: pre-existing RX error code must show");
        reset(); g_stats.esc_base_pdierr[1] = 1;
        CHECK(!esc_slave_clean(1), "T3: pre-existing PDI error must show");
    }
    printf("T3 PASS: pre-existing power-on history is never collapsed away\n");

    /* ── T4: the case that matters most — a SATURATED counter reads a zero
     * delta forever, so the session total stays 0 and looks exactly like a
     * clean slave. It must still print, or the panel would report a floor as
     * an absence. ───────────────────────────────────────────────────────── */
    {
        reset(); g_stats.esc_raw_crc[2][1] = 0xFF;
        CHECK(!esc_slave_clean(2), "T4: saturated invalid-frame counter must show "
                                   "even with a zero session total");
        CHECK(g_stats.esc_crc[2][1] == 0, "T4: the session total really is zero here");
        reset(); g_stats.esc_raw_rxerr[0][3] = 0xFF;
        CHECK(!esc_slave_clean(0), "T4: saturated RX-error counter must show");
        reset(); g_stats.esc_raw_fwderr[1][0] = 0xFF;
        CHECK(!esc_slave_clean(1), "T4: saturated forwarded-error counter must show");
        reset(); g_stats.esc_raw_puerr[1] = 0xFF;
        CHECK(!esc_slave_clean(1), "T4: saturated PU-error counter must show");
        /* 0xFE is one below the stop value: still counting, still clean. */
        reset(); g_stats.esc_raw_crc[2][1] = 0xFE;
        CHECK(esc_slave_clean(2), "T4: 0xFE is not saturated and stays clean");
    }
    printf("T4 PASS: a saturated counter is never mistaken for a clean slave\n");

    /* ── T5: out-of-range indices are clean, never a read past the array ─── */
    reset();
    CHECK(esc_slave_clean(-1) && esc_slave_clean(MAX_SLAVES),
          "T5: out-of-range slave index must be treated as clean");
    printf("T5 PASS: out-of-range slave indices are handled\n");

    /* ── T6: esc_any_lostlink sees only lost links, and only in range ────── */
    reset(); g_stats.esc_crc[1][1] = 9;
    CHECK(!esc_any_lostlink(), "T6: an invalid-frame count is not a lost link");
    reset(); g_stats.esc_lostlnk[3][2] = 1;
    CHECK(esc_any_lostlink(), "T6: a lost link must be seen");
    reset(); g_stats.esc_lostlnk[3][2] = 1; g_num_slaves = 2;
    CHECK(!esc_any_lostlink(), "T6: slaves beyond g_num_slaves are not scanned");
    printf("T6 PASS: lost-link detection is specific and range-limited\n");

    if (fails) { printf("*** ESC PANEL FAILURES ***\n"); return 1; }
    printf("ALL ESC PANEL TESTS PASS\n");
    return 0;
}
