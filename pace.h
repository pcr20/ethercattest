#ifndef ECAT_PACE_H
#define ECAT_PACE_H
/* Cyclic TX pacing and cycle measurement.
 *
 * Pure state machine, no I/O and no globals, so it is driven directly by a
 * synthetic clock in tests/t_pace.c. The TX thread owns one PaceState and
 * publishes the counters to g_stats after each send.
 *
 * ONLY ACTIVE WHEN A RATE IS SET (-r). In saturate mode interval_ns is 0 and
 * every function here is a no-op: there is no target period, so "late" and
 * "missed" are undefined, and send-to-send gaps measure the qdisc draining
 * rather than a cadence. Throughput is the right measure there and is already
 * reported. Leaving it off also keeps the cost out of the hot path in the mode
 * that iterates ~74k times/second rather than 1k.
 */
#include "ecat_common.h"

typedef struct {
    uint64_t interval_ns;    /* target period; 0 = saturate, measurement off */
    uint64_t next_send_ns;   /* absolute deadline for the next send          */
    uint64_t last_send_ns;   /* previous send, for the interval             */
    int      has_last;       /* 0 is a legal timestamp, so flag it          */
    /* measurement, TX-thread-local; published to g_stats by the caller */
    uint64_t count, sum_ns, min_ns, max_ns, late, missed;
} PaceState;

static inline void pace_init(PaceState *p, long rate_hz, uint64_t now) {
    memset(p, 0, sizeof(*p));
    p->interval_ns  = (rate_hz > 0) ? (1000000000ULL / (uint64_t)rate_hz) : 0;
    p->next_send_ns = now;
}

/* Deadline to sleep until before the next send, or 0 in saturate mode.
 *
 * If the deadline has slipped more than a full period into the past — a stall,
 * a probe pause, a scheduling delay — SKIP the missed cycles rather than
 * sending back-to-back to catch up. A real master skips; catching up produces
 * a burst at line rate, which is the traffic pattern a -r run exists to avoid.
 */
static inline uint64_t pace_deadline(PaceState *p, uint64_t now) {
    if (!p->interval_ns) return 0;
    if (now > p->next_send_ns + p->interval_ns) {
        p->missed      += (now - p->next_send_ns) / p->interval_ns;
        p->next_send_ns = now + p->interval_ns;
        p->has_last     = 0;      /* the gap across a stall is not a cycle */
    }
    return p->next_send_ns;
}

/* Record a completed send: advance the deadline, measure the interval. */
static inline void pace_on_sent(PaceState *p, uint64_t now) {
    if (!p->interval_ns) return;
    if (p->has_last) {
        uint64_t dt = now - p->last_send_ns;
        p->count++;
        p->sum_ns += dt;
        if (!p->min_ns || dt < p->min_ns) p->min_ns = dt;
        if (dt > p->max_ns)               p->max_ns = dt;
        if (dt > p->interval_ns + p->interval_ns / 2) p->late++;
    }
    p->last_send_ns  = now;
    p->has_last      = 1;
    p->next_send_ns += p->interval_ns;
}

/* A pause transmits nothing; the deadline is stale on resume. Resynchronise
 * without counting the paused period as missed cycles — it was deliberate. */
static inline void pace_resume(PaceState *p, uint64_t now) {
    if (!p->interval_ns) return;
    p->next_send_ns = now + p->interval_ns;
    p->has_last     = 0;
}

#endif /* ECAT_PACE_H */
