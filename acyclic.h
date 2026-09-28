#ifndef ECAT_ACYCLIC_H
#define ECAT_ACYCLIC_H
/* ── Acyclic job scheduling, shaped like TwinCAT's ──────────────────────────
 *
 * Measured from the 2026-09-23 capture, steady state, healthy windows only:
 *
 *   FPRD 0x0300 x2  read error counters   103.8 ms  (101.6 - 114.0)
 *   BWR  0x0300     clear them            103.8 ms  (101.7 - 112.0)
 *   APRD 0x0310 x2  lost-link counters    202.9 ms  (193.7 - 211.0)
 *
 * The clear follows the read by 0 or 1 cycle -- offset +0.0 ms in 72 cases and
 * +2.0 ms in 49, which under the capture's 2 ms batching means the same cycle
 * or the next. They are a locked read-then-clear pair, not two jobs that
 * happen to share a period. The lost-link poll drifts independently: its
 * offset from the read has a median of -16 ms and its period is not exactly
 * double (202.9 against 207.6), so it runs off its own timer.
 *
 * The +-5 ms spread says these are software timers serviced by the cyclic
 * task rather than strict cycle counts. By default we use exact periods, which
 * is a deviation; --random N reproduces the spread by drawing a fresh delay in
 * [-N, +N] cycles each time a job fires.
 *
 * Each job owns its PRNG state. Seeding them identically would lock the two
 * timers into a fixed relationship, which is precisely the behaviour the
 * capture says TwinCAT does NOT have. */
#include <stdint.h>

typedef struct {
    long     period;      /* base period in cycles (1 cycle = 1 ms)          */
    long     next;        /* cycle number this job next fires on             */
    int      jitter;      /* --random N: delay drawn from [-N, +N] cycles    */
    uint64_t rng;         /* private PRNG state; never shared between jobs   */
    uint64_t fires;       /* how many times it has fired                     */
} AcyJob;

/* xorshift64. Same generator as the payload fill in frame.c, kept local so
 * this header stays free-standing and testable on its own. */
static inline uint64_t acy_rand(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    *s = x;
    return x;
}

/* period: base period in cycles. phase: cycle number of the first fire.
 * jitter: 0 for exact periods, else the +-N in cycles. seed must differ
 * between jobs; a zero seed is replaced, since xorshift64 is stuck at 0. */
static inline void acy_init(AcyJob *j, long period, long phase, int jitter,
                            uint64_t seed)
{
    j->period = period > 0 ? period : 1;
    j->next   = phase;
    j->jitter = jitter < 0 ? 0 : jitter;
    j->rng    = seed ? seed : 0x9E3779B97F4A7C15ULL;
    j->fires  = 0;
}

/* Is this job due on this cycle? */
static inline int acy_due(const AcyJob *j, long cycle)
{
    return cycle >= j->next;
}

/* Called after the job has been emitted. The next fire is scheduled from the
 * one just taken, so a long interval is followed by an ordinary one rather
 * than being compensated for -- which is how a timer behaves, and keeps the
 * mean period equal to the base. */
static inline void acy_advance(AcyJob *j, long cycle)
{
    long delay = j->period;
    if (j->jitter) {
        /* Uniform over the 2N+1 values in [-N, +N]. */
        uint64_t r = acy_rand(&j->rng);
        delay += (long)(r % (uint64_t)(2 * j->jitter + 1)) - j->jitter;
        if (delay < 1) delay = 1;
    }
    j->next = cycle + delay;
    j->fires++;
}

#endif /* ECAT_ACYCLIC_H */
