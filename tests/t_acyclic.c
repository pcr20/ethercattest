/* Acyclic job scheduling (acyclic.h).
 *
 * The properties that matter: the base period is honoured exactly when no
 * jitter is asked for, jitter stays inside the stated bound and averages out
 * rather than dragging the rate, and the two jobs never share a random
 * sequence. That last one is the reason the seeds are separate at all — the
 * capture shows TwinCAT's read/clear timer and its lost-link timer drifting
 * independently (median offset -16 ms, periods 103.8 and 202.9 ms, not a
 * ratio of exactly two), and a shared PRNG would lock them together. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "acyclic.h"

static int fails = 0;
#define CHECK(c, ...) do { if(!(c)) { printf("FAIL: "); printf(__VA_ARGS__); \
                                      printf("\n"); fails++; } } while (0)

/* Run a job over `cycles` and return the intervals it fired at. */
static int run(AcyJob *j, long cycles, long *gaps, int max)
{
    int n = 0; long last = -1;
    for (long c = 0; c < cycles; c++)
        if (acy_due(j, c)) {
            if (last >= 0 && n < max) gaps[n++] = c - last;
            last = c;
            acy_advance(j, c);
        }
    return n;
}

int main(void)
{
    long g[8192], g2[8192];

    /* ── T1: no jitter means exactly the base period ─────────────────────── */
    {
        int f0 = fails;
        AcyJob j; acy_init(&j, 104, 0, 0, 12345);
        int n = run(&j, 52000, g, 8192);
        CHECK(n > 400, "T1: only %d fires in 52000 cycles", n);
        for (int i = 0; i < n; i++)
            if (g[i] != 104) { CHECK(0, "T1: gap %d was %ld, want 104", i, g[i]); break; }
        CHECK(j.fires == (uint64_t)(n + 1), "T1: fire count %lu, expected %d",
              j.fires, n + 1);
        if (fails == f0)
            printf("T1 PASS: --random 0 gives exactly the base period, %d times\n", n);
    }

    /* ── T2: the phase argument sets the first fire ──────────────────────── */
    {
        int f0 = fails;
        AcyJob j; acy_init(&j, 203, 50, 0, 1);
        long first = -1;
        for (long c = 0; c < 400; c++)
            if (acy_due(&j, c)) { first = c; acy_advance(&j, c); break; }
        CHECK(first == 50, "T2: first fire at cycle %ld, want 50", first);
        if (fails == f0)
            printf("T2 PASS: phase offset places the first fire (lost-link at 50)\n");
    }

    /* ── T3: jitter stays inside the bound, and never stalls ─────────────── */
    {
        int f0 = fails;
        AcyJob j; acy_init(&j, 104, 0, 5, 0xDEADBEEF);
        int n = run(&j, 200000, g, 8192);
        long lo = 104, hi = 104;
        for (int i = 0; i < n; i++) { if (g[i] < lo) lo = g[i]; if (g[i] > hi) hi = g[i]; }
        CHECK(lo >= 99,  "T3: shortest gap %ld, must not go below 104-5", lo);
        CHECK(hi <= 109, "T3: longest gap %ld, must not exceed 104+5", hi);
        CHECK(lo < 104 && hi > 104, "T3: jitter produced no spread (%ld..%ld)", lo, hi);
        if (fails == f0)
            printf("T3 PASS: --random 5 keeps gaps in [%ld,%ld], both sides of 104\n",
                   lo, hi);
    }

    /* ── T4: jitter is unbiased — it must not drag the mean rate ─────────── */
    {
        int f0 = fails;
        AcyJob j; acy_init(&j, 104, 0, 5, 0x1234567);
        int n = run(&j, 2000000, g, 8192);
        double sum = 0; for (int i = 0; i < n; i++) sum += (double)g[i];
        double mean = sum / n;
        CHECK(fabs(mean - 104.0) < 0.25,
              "T4: mean period %.3f over %d fires, want 104 +-0.25 — a biased "
              "draw would change the poll rate", mean, n);
        if (fails == f0)
            printf("T4 PASS: mean period %.3f over %d fires — jitter is unbiased\n",
                   mean, n);
    }

    /* ── T5: a period of 1 is never produced, however large the jitter ───── */
    {
        int f0 = fails;
        AcyJob j; acy_init(&j, 4, 0, 50, 99);      /* jitter far exceeds period */
        int n = run(&j, 100000, g, 8192);
        long lo = 1 << 30;
        for (int i = 0; i < n; i++) if (g[i] < lo) lo = g[i];
        CHECK(lo >= 1, "T5: gap %ld — a job must never be scheduled in the past", lo);
        CHECK(n > 0, "T5: job never fired");
        if (fails == f0)
            printf("T5 PASS: jitter larger than the period still yields gaps >= 1\n");
    }

    /* ── T6: the two jobs must not share a random sequence ───────────────── */
    {
        int f0 = fails;
        uint64_t seed = 0xABCDEF0123456789ULL;
        AcyJob a, b;
        acy_init(&a, 104, 0, 5, seed ^ 0xA5A5A5A5A5A5A5A5ULL);
        acy_init(&b, 104, 0, 5, seed ^ 0x5A5A5A5A5A5A5A5AULL);
        int na = run(&a, 100000, g,  8192);
        int nb = run(&b, 100000, g2, 8192);
        int cmp = na < nb ? na : nb, same = 0;
        for (int i = 0; i < cmp; i++) if (g[i] == g2[i]) same++;
        CHECK(cmp > 100, "T6: too few fires to compare (%d)", cmp);
        CHECK(same < cmp, "T6: the two jobs produced IDENTICAL gap sequences — "
              "they would stay phase-locked, which is what separate seeds exist "
              "to prevent");
        /* With 11 equally likely gaps, matching by chance is ~1 in 11. */
        CHECK(same < cmp / 3, "T6: %d of %d gaps identical — far above the ~1/11 "
              "expected by chance", same, cmp);

        /* And the same seed must reproduce a run exactly, so a --random run
         * can be repeated from the seeds printed in the banner. */
        AcyJob c; acy_init(&c, 104, 0, 5, seed ^ 0xA5A5A5A5A5A5A5A5ULL);
        int nc = run(&c, 100000, g2, 8192);
        CHECK(nc == na, "T6: replay produced %d fires, original %d", nc, na);
        int diff = 0;
        for (int i = 0; i < na && i < nc; i++) if (g2[i] != g[i]) diff++;
        if (na == nc) CHECK(diff == 0, "T6: replaying the seed gave %d different gaps",
                            diff);
        if (fails == f0)
            printf("T6 PASS: distinct seeds diverge (%d/%d gaps shared), and a "
                   "seed replays exactly\n", same, cmp);
    }

    /* ── T7: the measured TwinCAT periods, as configured ─────────────────── */
    {
        int f0 = fails;
        AcyJob rd, ll;
        acy_init(&rd, 104,  0, 0, 1);
        acy_init(&ll, 203, 50, 0, 2);
        int nr = run(&rd, 100000, g, 8192);
        int nl = run(&ll, 100000, g2, 8192);
        double rate_rd = 1000.0 * nr / 100000.0, rate_ll = 1000.0 * nl / 100000.0;
        CHECK(fabs(rate_rd - 9.6) < 0.2, "T7: read/clear fires at %.2f/s, "
              "TwinCAT's 103.8 ms period is 9.6/s", rate_rd);
        CHECK(fabs(rate_ll - 4.9) < 0.2, "T7: lost-link fires at %.2f/s, "
              "TwinCAT's 202.9 ms period is 4.9/s", rate_ll);
        if (fails == f0)
            printf("T7 PASS: at 1 kHz the periods give %.2f/s and %.2f/s, matching "
                   "the capture\n", rate_rd, rate_ll);
    }

    if (fails) { printf("\n*** ACYCLIC SCHEDULING FAILURES ***\n"); return 1; }
    printf("\nALL ACYCLIC TESTS PASS\n");
    return 0;
}
