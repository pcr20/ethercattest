/* Cyclic TX pacing (pace.h), driven by a synthetic clock.
 *
 * The property that matters: after a stall the pacer must SKIP the missed
 * cycles, not send back-to-back to catch up. Catching up turns a 1 ms cadence
 * into a burst at line rate — exactly the traffic pattern a -r run exists to
 * avoid producing. */
#include "crc.c"
#include "stats.c"
#include "pace.h"

static int fails = 0;
#define CHECK(c, ...) do { if(!(c)) { printf("FAIL: "); printf(__VA_ARGS__); \
                                      printf("\n"); fails++; } } while (0)
#define MS 1000000ULL

int main(void)
{
    crc32c_init();

    /* ── T1: saturate mode is entirely inert ─────────────────────────────── */
    {
        int f0 = fails;
        PaceState p; pace_init(&p, 0, 1000);
        CHECK(p.interval_ns == 0, "saturate must leave interval_ns 0");
        CHECK(pace_deadline(&p, 999999) == 0, "saturate must return no deadline");
        pace_on_sent(&p, 5 * MS);
        pace_resume(&p, 9 * MS);
        CHECK(p.count == 0 && p.late == 0 && p.missed == 0,
              "saturate must record nothing (count=%lu late=%lu missed=%lu)",
              p.count, p.late, p.missed);
        if (fails == f0) printf("T1 PASS: saturate mode records nothing — "
                                "count==0 is the 'not measured' marker\n");
    }

    /* ── T2: an on-time 1 kHz sequence ───────────────────────────────────── */
    {
        int f0 = fails;
        PaceState p; pace_init(&p, 1000, 0);
        CHECK(p.interval_ns == MS, "1000 Hz must be 1 ms, got %lu", p.interval_ns);
        /* The deadline sequence starts at the init time and steps by one
         * period per send, so sends land at 0, 1 ms, 2 ms ... */
        for (int i = 0; i < 100; i++) {
            uint64_t now = (uint64_t)i * MS;
            CHECK(pace_deadline(&p, now) == now,
                  "deadline for cycle %d should be %lu, got %lu",
                  i, now, p.next_send_ns);
            pace_on_sent(&p, now);
        }
        CHECK(p.count == 99, "99 intervals from 100 sends, got %lu", p.count);
        CHECK(p.sum_ns / p.count == MS, "mean must be 1 ms, got %lu", p.sum_ns / p.count);
        CHECK(p.min_ns == MS && p.max_ns == MS, "min/max must both be 1 ms");
        CHECK(p.late == 0 && p.missed == 0, "on-time run must have no late/missed");
        if (fails == f0) printf("T2 PASS: on-time 1 kHz — mean/min/max 1000.0 us, "
                                "no late, no missed\n");
    }

    /* ── T3: one late wakeup is counted, and does not shift the deadline ─── */
    {
        int f0 = fails;
        PaceState p; pace_init(&p, 1000, 0);
        pace_on_sent(&p, 0);                        /* cycle 0              */
        pace_on_sent(&p, 1 * MS);
        pace_on_sent(&p, 2 * MS + 800000);          /* 1.8 ms: late (>1.5x) */
        pace_on_sent(&p, 3 * MS);                   /* back on schedule     */
        CHECK(p.late == 1, "one late cycle expected, got %lu", p.late);
        CHECK(p.missed == 0, "a late wakeup is not a missed cycle, got %lu", p.missed);
        CHECK(p.max_ns == 1800000, "max must be the late interval, got %lu", p.max_ns);
        /* Four sends, so the deadline is exactly 4 periods on from init —
         * it advances per send, never from when the send actually landed, so
         * a late wakeup cannot drag the schedule with it. */
        CHECK(p.next_send_ns == 4 * MS,
              "deadline must be 4 periods on regardless of the late send, got %lu",
              p.next_send_ns);
        if (fails == f0) printf("T3 PASS: a late wakeup is counted but does not "
                                "drift the deadline\n");
    }

    /* ── T4: a 200 ms stall SKIPS cycles, never bursts ───────────────────── */
    {
        int f0 = fails;
        PaceState p; pace_init(&p, 1000, 0);
        pace_on_sent(&p, 0);                      /* cycle 0, deadline 1 ms */
        pace_on_sent(&p, 1 * MS);                 /* cycle 1, deadline 2 ms */
        uint64_t after = 202 * MS;                /* 200 ms later           */
        uint64_t dl = pace_deadline(&p, after);
        CHECK(p.missed == 200, "200 ms stall at 1 kHz = 200 missed, got %lu", p.missed);
        CHECK(dl == after + MS,
              "must resync one period ahead (burst guard), got %lu vs %lu",
              (unsigned long)dl, (unsigned long)(after + MS));
        CHECK(dl > after, "resync deadline must be in the FUTURE — a deadline in "
                          "the past is exactly what produces a catch-up burst");
        /* and the gap across the stall must not pollute the interval stats */
        uint64_t before_count = p.count, before_max = p.max_ns;
        pace_on_sent(&p, dl);
        CHECK(p.count == before_count,
              "the stall gap is not a cycle; count moved %lu -> %lu",
              before_count, p.count);
        CHECK(p.max_ns == before_max,
              "the stall must not become max, got %lu", p.max_ns);
        if (fails == f0) printf("T4 PASS: 200 ms stall skips 200 cycles, resyncs "
                                "forward, and does not pollute min/max\n");
    }

    /* ── T5: a pause resyncs without counting missed cycles ──────────────── */
    {
        int f0 = fails;
        PaceState p; pace_init(&p, 1000, 0);
        pace_on_sent(&p, 0);
        pace_on_sent(&p, 1 * MS);
        pace_resume(&p, 500 * MS);                /* deliberate 0.5 s pause */
        CHECK(p.missed == 0, "a deliberate pause is not a missed cycle, got %lu",
              p.missed);
        CHECK(p.next_send_ns == 501 * MS, "resume must schedule one period on");
        uint64_t before = p.count;
        pace_on_sent(&p, 501 * MS);
        CHECK(p.count == before, "the paused gap is not an interval, %lu -> %lu",
              before, p.count);
        if (fails == f0) printf("T5 PASS: a pause resyncs cleanly — not counted as "
                                "missed, not measured as an interval\n");
    }

    /* ── T6: TX runs real-time only when paced ───────────────────────────
     * In saturate mode a SCHED_FIFO TX thread spins without ever sleeping and
     * starves lower-priority RT kernel threads on its core (the WiFi IRQ
     * thread, twice, ending in an rtnl_lock deadlock). The decision must track
     * the pacer exactly: RT if and only if pace_init actually paces, so the two
     * can never disagree about which mode we are in. */
    {
        int f0 = fails;
        CHECK(!pace_tx_wants_realtime(0),
              "saturate (-r 0) must run at normal priority");
        CHECK(pace_tx_wants_realtime(1000), "-r 1000 must keep SCHED_FIFO");
        long rates[] = { -5, -1, 0, 1, 10, 500, 1000, 8000, 100000 };
        for (size_t i = 0; i < sizeof rates / sizeof *rates; i++) {
            PaceState q; pace_init(&q, rates[i], 0);
            int paced = q.interval_ns != 0;
            CHECK(pace_tx_wants_realtime(rates[i]) == paced,
                  "rate %ld: realtime=%d but pacer paced=%d — must agree",
                  rates[i], pace_tx_wants_realtime(rates[i]), paced);
        }
        if (fails == f0) printf("T6 PASS: TX is real-time iff paced — saturate "
                                "runs at normal priority\n");
    }

    printf("\n%s\n", fails ? "*** PACE FAILURES ***" : "ALL PACE TESTS PASS");
    return fails;
}
