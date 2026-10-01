#ifndef ECAT_ATRISK_H
#define ECAT_ATRISK_H
/* ── At-risk time ───────────────────────────────────────────────────────────
 *
 * A link-drop rate is only meaningful over the time the chain was actually
 * capable of dropping a link: every slave in OP, process data flowing. Time
 * spent with the chain severed is not evidence of anything.
 *
 * This existed implicitly and got it wrong. Run 6 of LINKDROPS.md dropped at
 * t=2.163 s, failed to re-initialise, and then ran 306 s with three of four
 * slaves out of OP. ecat_op reported "At-risk time: 308.2 s -> 0.0065
 * drops/s" because it subtracted only the 2.9 s recovery outage from the
 * elapsed time. The true figure is 2.163 s and the true rate is ~140x higher.
 * A number that wrong in the conservative direction is worse than none: it
 * makes a reproduction look like a near-negative.
 *
 * The test is per cycle and needs nothing extra on the wire: the process-data
 * working counter either matches what a healthy chain returns, or it does not.
 *
 * Pure: no clock of its own, no I/O. Unit-tested in t_recovery. */
#include <stdint.h>

typedef struct {
    uint64_t ns;        /* accumulated at-risk time                          */
    uint64_t prev;      /* timestamp of the previous cycle                   */
    uint64_t cycles;    /* cycles counted as at-risk                         */
    int      started;   /* a healthy WKC has been established at least once  */
} AtRisk;

static inline void ar_init(AtRisk *a, uint64_t t0)
{ a->ns = 0; a->prev = t0; a->cycles = 0; a->started = 0; }

/* Call once per cycle. `chain_ok` is non-zero when this cycle's process-data
 * WKC equalled the healthy value. The interval since the previous cycle is
 * credited only when it was healthy, so the accounting never credits time
 * during which the chain was known to be broken.
 *
 * Nothing is credited before the first healthy cycle: at start-up the
 * expected WKC is not yet known, and guessing would credit bring-up. */
static inline void ar_cycle(AtRisk *a, uint64_t now, int chain_ok)
{
    if (chain_ok) {
        if (a->started && now > a->prev) a->ns += now - a->prev;
        a->started = 1;
        a->cycles++;
    }
    a->prev = now;
}

static inline double ar_seconds(const AtRisk *a)
{ return (double)a->ns / 1e9; }

/* Drops per second over the at-risk time. Returns 0 when no at-risk time has
 * accumulated, rather than dividing by zero — a run that never reached OP has
 * no rate, not an infinite one. */
static inline double ar_rate(const AtRisk *a, uint64_t drops)
{ return a->ns ? (double)drops * 1e9 / (double)a->ns : 0.0; }

#endif /* ECAT_ATRISK_H */
