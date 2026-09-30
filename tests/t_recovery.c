/* Link-drop recovery sequencing (recovery.h).
 *
 * The properties that matter, all taken from TWINCAT.md §6.6: the port is
 * forced closed exactly once and immediately; the wait for the physical link
 * is a WAIT and not a 2 s sleep, so it tracks whenever the link actually
 * returns; the 1.000 s debounce after link return is a timer and is honoured
 * to the millisecond; the port byte written is byte-identical to TwinCAT's
 * 0xFC/0xF4; and a link that never returns is abandoned rather than hanging
 * the run.
 *
 * The last one is not cosmetic. Without a timeout an unplugged cable would
 * stall the master in REC_CLOSED forever while the remaining slaves went
 * unserved — the run would look alive and measure nothing. */
#include <stdio.h>
#include <string.h>
#include "recovery.h"
#include "atrisk.h"

static int fails = 0;
#define CHECK(c, ...) do { if(!(c)) { printf("FAIL: "); printf(__VA_ARGS__); \
                                      printf("\n"); fails++; } } while (0)

#define MS  1000000ULL
#define SEC 1000000000ULL

/* Drive the machine from t=0, with the link returning at `link_at` ns (0 =
 * never). Records when each action was emitted. Returns total steps. */
typedef struct {
    uint64_t close_at, open_at, reinit_at, giveup_at;
    int closes, opens, reinits, giveups;
} Trace;

static void run(RecCtx *r, Trace *tr, uint64_t link_at, uint64_t until)
{
    memset(tr, 0, sizeof *tr);
    for (uint64_t t = 0; t <= until; t += MS) {          /* 1 ms cycles */
        int link_up = link_at && t >= link_at;
        RecAction a = rec_step(r, t, link_up);
        switch (a) {
        case REC_ACT_CLOSE_PORT: if (!tr->closes++)  tr->close_at  = t; break;
        case REC_ACT_OPEN_PORT:  if (!tr->opens++)   tr->open_at   = t; break;
        case REC_ACT_REINIT:     if (!tr->reinits++) tr->reinit_at = t; break;
        case REC_ACT_GIVE_UP:    if (!tr->giveups++) tr->giveup_at = t; break;
        case REC_ACT_NONE: break;
        }
    }
}

int main(void)
{
    /* ── T1: the whole TwinCAT sequence, with the link back at 2.000 s ───── */
    {
        int f0 = fails;
        RecCtx r; rec_init(&r, 1 * SEC, 30 * SEC);
        Trace tr;
        CHECK(!rec_busy(&r), "T1: machine busy before any drop");
        rec_on_drop(&r, 0, 1, 1);
        CHECK(rec_busy(&r), "T1: not busy after a drop");
        run(&r, &tr, 2 * SEC, 5 * SEC);

        CHECK(tr.closes == 1, "T1: port closed %d times, want exactly 1", tr.closes);
        CHECK(tr.close_at == 0, "T1: port closed at %.3f s, want immediately",
              (double)tr.close_at / 1e9);
        CHECK(tr.opens == 1, "T1: port reopened %d times, want 1", tr.opens);
        CHECK(tr.open_at == 3 * SEC,
              "T1: reopened at %.3f s — TwinCAT reopens at 3.000 s (link back at "
              "2.000 + 1.000 s debounce)", (double)tr.open_at / 1e9);
        CHECK(tr.reinits == 1, "T1: %d re-inits, want 1", tr.reinits);
        CHECK(r.recoveries == 1 && r.timeouts == 0,
              "T1: recoveries %lu timeouts %lu", r.recoveries, r.timeouts);
        CHECK(!rec_busy(&r), "T1: still busy after the sequence completed");
        if (fails == f0)
            printf("T1 PASS: close at 0.000 s, reopen at %.3f s, re-init — "
                   "TwinCAT's 3.00 s outage\n", (double)tr.open_at / 1e9);
    }

    /* ── T2: the 2 s is the PHY's, not ours — a faster link recovers sooner ─ */
    {
        int f0 = fails;
        RecCtx r; rec_init(&r, 1 * SEC, 30 * SEC);
        Trace tr;
        rec_on_drop(&r, 0, 1, 1);
        run(&r, &tr, 200 * MS, 5 * SEC);
        CHECK(tr.open_at == 1200 * MS,
              "T2: link back at 0.200 s should reopen at 1.200 s, got %.3f — a "
              "hard-coded 2 s sleep would give 3.000",
              (double)tr.open_at / 1e9);
        if (fails == f0)
            printf("T2 PASS: link back at 0.200 s reopens at 1.200 s — the wait "
                   "tracks the PHY, it is not a fixed 2 s\n");
    }

    /* ── T3: ...and a slower one waits ───────────────────────────────────── */
    {
        int f0 = fails;
        RecCtx r; rec_init(&r, 1 * SEC, 30 * SEC);
        Trace tr;
        rec_on_drop(&r, 0, 1, 1);
        run(&r, &tr, 6 * SEC, 10 * SEC);
        CHECK(tr.open_at == 7 * SEC, "T3: link back at 6 s should reopen at 7 s, "
              "got %.3f", (double)tr.open_at / 1e9);
        CHECK(tr.reinits == 1, "T3: %d re-inits", tr.reinits);
        if (fails == f0)
            printf("T3 PASS: a 6 s link return still gets its full 1.000 s "
                   "debounce\n");
    }

    /* ── T4: a link that never returns is abandoned, not waited on forever ─ */
    {
        int f0 = fails;
        RecCtx r; rec_init(&r, 1 * SEC, 5 * SEC);
        Trace tr;
        rec_on_drop(&r, 0, 1, 1);
        run(&r, &tr, 0, 20 * SEC);
        CHECK(tr.giveups == 1, "T4: gave up %d times, want 1", tr.giveups);
        CHECK(tr.giveup_at == 5 * SEC, "T4: gave up at %.3f s, want the 5 s "
              "timeout", (double)tr.giveup_at / 1e9);
        CHECK(tr.opens == 0 && tr.reinits == 0,
              "T4: reopened/re-inited a port whose link never came back");
        CHECK(r.timeouts == 1 && r.recoveries == 0,
              "T4: timeouts %lu recoveries %lu", r.timeouts, r.recoveries);
        CHECK(!rec_busy(&r), "T4: still busy after giving up — the run would "
              "never recover");
        if (fails == f0)
            printf("T4 PASS: a dead link is abandoned after the timeout and the "
                   "machine returns to idle\n");
    }

    /* ── T5: repeat reports of one drop do not restart the sequence ──────── */
    {
        int f0 = fails;
        RecCtx r; rec_init(&r, 1 * SEC, 30 * SEC);
        CHECK(rec_on_drop(&r, 0, 1, 1) == 1, "T5: first drop refused");
        /* 0x0310 is polled every ~203 ms and one physical drop bumped it by 2
         * on the 2026-09-30 run, so the same event gets reported again. */
        CHECK(rec_on_drop(&r, 100 * MS, 1, 1) == 0,
              "T5: a second report of the same drop started a second recovery");
        CHECK(rec_on_drop(&r, 150 * MS, 0, 1) == 0,
              "T5: a drop on another slave preempted one in flight");
        CHECK(r.slave == 1 && r.port == 1,
              "T5: machine switched to slave %d port %d mid-recovery",
              r.slave, r.port);
        if (fails == f0)
            printf("T5 PASS: duplicate and concurrent drop reports are absorbed\n");
    }

    /* ── T6: the port byte is byte-identical to TwinCAT's ────────────────── */
    {
        int f0 = fails;
        /* TWINCAT.md §6.6: the chain sits at 0xF4 and TwinCAT writes 0xFC. */
        uint8_t closed = rec_loop_closed(0xF4, 1);
        uint8_t back   = rec_loop_auto(closed, 1);
        CHECK(closed == 0xFC, "T6: closing port 1 of 0xF4 gave 0x%02X, TwinCAT "
              "writes 0xFC", closed);
        CHECK(back == 0xF4, "T6: reopening gave 0x%02X, want the original 0xF4",
              back);
        /* and it must touch only the two bits it owns */
        CHECK(rec_loop_closed(0x00, 0) == 0x03, "T6: port 0 closed of 0x00 = 0x%02X",
              rec_loop_closed(0x00, 0));
        CHECK(rec_loop_closed(0xFF, 2) == 0xFF, "T6: port 2 already closed changed");
        CHECK(rec_loop_auto(0xFF, 3) == 0x7F, "T6: port 3 auto of 0xFF = 0x%02X",
              rec_loop_auto(0xFF, 3));
        if (fails == f0)
            printf("T6 PASS: 0xF4 -> 0xFC -> 0xF4, byte-identical to the capture\n");
    }

    /* ── T7: the DL-status physical-link bit ─────────────────────────────── */
    {
        int f0 = fails;
        /* bits 4..7 are the per-port physical link */
        CHECK(rec_link_up(0x0030, 1) == 1, "T7: port 1 link should be up in 0x0030");
        CHECK(rec_link_up(0x0010, 1) == 0, "T7: port 1 link should be down in 0x0010");
        CHECK(rec_link_up(0x0010, 0) == 1, "T7: port 0 link should be up in 0x0010");
        /* the loop/communication bits above must not be mistaken for it */
        CHECK(rec_link_up(0x0C00, 1) == 0,
              "T7: communication bits were read as a physical link");
        if (fails == f0)
            printf("T7 PASS: the physical-link bit is read, not the loop bits\n");
    }

    /* ── T8: outage accounting, which is what the summary reports ────────── */
    {
        int f0 = fails;
        RecCtx r; rec_init(&r, 1 * SEC, 30 * SEC);
        Trace tr;
        rec_on_drop(&r, 0, 1, 1);      run(&r, &tr, 2 * SEC, 5 * SEC);
        RecCtx s = r;                  /* keep the first outage for comparison */
        rec_on_drop(&r, 10 * SEC, 1, 1);
        for (uint64_t t = 10 * SEC; t <= 20 * SEC; t += MS)
            rec_step(&r, t, t >= 10500 * MS);
        CHECK(r.recoveries == 2, "T8: %lu recoveries, want 2", r.recoveries);
        CHECK(s.outage_ns_max == 3001 * MS || s.outage_ns_max == 3 * SEC,
              "T8: first outage recorded as %.3f s, want ~3.000",
              (double)s.outage_ns_max / 1e9);
        double mean = (double)r.outage_ns_total / r.recoveries / 1e9;
        CHECK(mean > 2.2 && mean < 2.8, "T8: mean outage %.3f s over 3.00 and "
              "1.50 s outages, want ~2.25", mean);
        if (fails == f0)
            printf("T8 PASS: outage totals track (max %.3f s, mean %.3f s)\n",
                   (double)r.outage_ns_max / 1e9, mean);
    }

    /* ── T9: at-risk time counts only cycles with the chain healthy ──────
     *
     * Regression for run 6 of SAGENTIA.md. It dropped at t=2.163 s, failed to
     * re-initialise, then ran 306 s with three of four slaves out of OP, and
     * ecat_op reported "At-risk time: 308.2 s" because it subtracted only the
     * 2.9 s recovery outage from the elapsed time. The rate was understated by
     * ~140x — a reproduction made to look like a near-negative. */
    {
        int f0 = fails;
        AtRisk a; ar_init(&a, 0);
        for (uint64_t t = MS; t <= 10 * SEC; t += MS) ar_cycle(&a, t, 1);
        CHECK(ar_seconds(&a) > 9.998 && ar_seconds(&a) < 10.001,
              "T9: a wholly healthy run should credit ~10 s, got %.4f",
              ar_seconds(&a));

        /* The run-6 shape: healthy to 2.163 s, then broken for 306 s. */
        AtRisk b; ar_init(&b, 0);
        for (uint64_t t = MS; t <= 2163 * MS; t += MS) ar_cycle(&b, t, 1);
        for (uint64_t t = 2164 * MS; t <= 308 * SEC; t += MS) ar_cycle(&b, t, 0);
        CHECK(ar_seconds(&b) > 2.160 && ar_seconds(&b) < 2.166,
              "T9: run-6 shape should credit ~2.163 s, got %.4f — 308 s means "
              "the severed chain is still being counted", ar_seconds(&b));
        double rate = ar_rate(&b, 1);
        CHECK(rate > 0.46 && rate < 0.47,
              "T9: one drop in 2.163 s is %.4f /s, want ~0.462 (the broken "
              "accounting gave 0.0065)", rate);

        /* Nothing is credited before the first healthy cycle, so bring-up
         * does not leak in. */
        AtRisk c; ar_init(&c, 0);
        for (uint64_t t = MS; t <= 5 * SEC; t += MS) ar_cycle(&c, t, 0);
        CHECK(ar_seconds(&c) == 0.0, "T9: %.4f s credited with no healthy "
              "cycle at all", ar_seconds(&c));
        CHECK(ar_rate(&c, 3) == 0.0, "T9: a rate was reported with no at-risk "
              "time — that is a divide by zero, not an infinite rate");

        /* A chain that recovers resumes accumulating, and the gap is excluded. */
        AtRisk d; ar_init(&d, 0);
        for (uint64_t t = MS; t <= 1 * SEC; t += MS) ar_cycle(&d, t, 1);
        for (uint64_t t = 1001 * MS; t <= 4 * SEC; t += MS) ar_cycle(&d, t, 0);
        for (uint64_t t = 4001 * MS; t <= 5 * SEC; t += MS) ar_cycle(&d, t, 1);
        CHECK(ar_seconds(&d) > 1.995 && ar_seconds(&d) < 2.002,
              "T9: 1 s healthy + 3 s broken + 1 s healthy should credit ~2 s, "
              "got %.4f", ar_seconds(&d));
        if (fails == f0)
            printf("T9 PASS: at-risk credits only healthy cycles "
                   "(run-6 shape %.3f s, not 308)\n", ar_seconds(&b));
    }

    if (fails) { printf("\n*** RECOVERY FAILURES ***\n"); return 1; }
    printf("\nALL RECOVERY TESTS PASS\n");
    return 0;
}
