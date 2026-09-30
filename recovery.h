#ifndef ECAT_RECOVERY_H
#define ECAT_RECOVERY_H
/* ── Link-drop recovery, shaped like TwinCAT's ──────────────────────────────
 *
 * Measured from the 2026-09-23 capture (TWINCAT.md §6.6), identical to the
 * millisecond on all three drops:
 *
 *   TwinCAT forces port 1 closed (0x0101 = 0xFC)        +2.0 ms
 *   physical link returns                               +2.0012 / +2.0008 / +1.9842 s
 *   TwinCAT reopens the port    (0x0101 = 0xF4)         +3.0011 / +3.0010 / +2.9841 s
 *   drive reads AL status INIT, re-initialised to OP    ~+40 ms
 *
 * Two constants. Only ONE of them is TwinCAT's:
 *
 *   - The 2.000 s from drop to link return is the PHY re-negotiating. It is
 *     not a timer anyone sets, so we must WAIT FOR THE LINK rather than sleep.
 *     Sleeping 2 s would look identical on this hardware and would silently
 *     mis-measure any unit that negotiates faster or slower.
 *   - The 1.000 s from link return to reopen IS a TwinCAT debounce timer, and
 *     is reproduced as a timer.
 *
 * Why this exists at all: without it the first drop ends the experiment. The
 * 4-slave run of 2026-09-30 broke at t=8.7 s and spent the remaining 70 s with
 * a severed chain, so 89% of its cycles were unusable and no drop RATE could
 * be extracted from it. TwinCAT recovers in 3.00 s every time and keeps
 * running, which is the only reason it could record three drops in 25 s.
 *
 * This header is pure: no I/O, no clock of its own, no EtherCAT. It is handed
 * a time and a link state and returns the action to take. That keeps the
 * sequencing testable without hardware, which matters because every previous
 * defect in this tool was caught only on the rig.
 *
 * NOT a claim of fidelity: TwinCAT's re-init is 86 writes we replay at
 * bring-up, and what it does after a drop was not separately decoded — the
 * capture shows only "~40 ms, AL status INIT to OP". We re-run our own
 * bring-up, which is that same sequence. */
#include <stdint.h>

typedef enum {
    REC_IDLE = 0,     /* nothing wrong                                      */
    REC_CLOSED,       /* port forced closed; waiting for the physical link  */
    REC_DEBOUNCE,     /* link back; waiting out the 1.000 s timer           */
    REC_SETTLE,       /* port reopened; waiting for the slaves to answer    */
    REC_REINIT        /* chain reachable; slaves need bringing back to OP   */
} RecState;

typedef enum {
    REC_ACT_NONE = 0,
    REC_ACT_CLOSE_PORT,   /* write 0x0101 with this port forced closed      */
    REC_ACT_OPEN_PORT,    /* write 0x0101 back to auto-close                */
    REC_ACT_REINIT,       /* re-run bring-up for the slaves beyond the gap  */
    REC_ACT_GIVE_UP,      /* the link never came back; stop waiting         */
    REC_ACT_GIVE_UP_SETTLE/* reopened, but the slaves never answered        */
} RecAction;

typedef struct {
    RecState state;
    int      slave;              /* position of the slave reporting the drop */
    int      port;               /* its port that lost the link              */
    uint64_t t_drop;             /* ns, when recovery started                */
    uint64_t t_link_back;        /* ns, when the physical link returned      */
    uint64_t t_reopened;         /* ns, when the port was reopened           */
    int      closed_issued;      /* the 0x0101 close has been written        */
    uint64_t debounce_ns;        /* TwinCAT's 1.000 s                        */
    uint64_t link_timeout_ns;    /* give up if the link never returns        */
    uint64_t settle_timeout_ns;  /* give up if the chain never comes back    */
    uint64_t recoveries;         /* completed                                */
    uint64_t timeouts;           /* abandoned                                */
    uint64_t outage_ns_total;    /* summed drop -> reinit, for the summary   */
    uint64_t outage_ns_max;
    uint64_t settle_ns_total;    /* reopen -> slaves answering again         */
    uint64_t settle_ns_max;
} RecCtx;

static inline void rec_init(RecCtx *r, uint64_t debounce_ns,
                            uint64_t link_timeout_ns, uint64_t settle_timeout_ns)
{
    r->state = REC_IDLE;
    r->slave = r->port = -1;
    r->t_drop = r->t_link_back = r->t_reopened = 0;
    r->closed_issued = 0;
    r->debounce_ns       = debounce_ns;
    r->link_timeout_ns   = link_timeout_ns;
    r->settle_timeout_ns = settle_timeout_ns;
    r->recoveries = r->timeouts = 0;
    r->outage_ns_total = r->outage_ns_max = 0;
    r->settle_ns_total = r->settle_ns_max = 0;
}

static inline int rec_busy(const RecCtx *r) { return r->state != REC_IDLE; }

/* Which input rec_step() will actually consult, so a caller can skip the
 * transaction for the other one instead of polling the wire pointlessly. */
static inline int rec_needs_link(const RecCtx *r)  { return r->state == REC_CLOSED; }
static inline int rec_needs_chain(const RecCtx *r) { return r->state == REC_SETTLE; }

/* A lost-link counter moved on (slave, port). Ignored while a recovery is
 * already in flight: the counter is polled every ~203 ms and one physical
 * drop routinely bumps it by more than one, so a second report is the same
 * event, not a new one. */
static inline int rec_on_drop(RecCtx *r, uint64_t now, int slave, int port)
{
    if (r->state != REC_IDLE) return 0;
    r->state = REC_CLOSED;
    r->slave = slave;
    r->port  = port;
    r->t_drop = now;
    r->t_link_back = r->t_reopened = 0;
    r->closed_issued = 0;
    return 1;
}

/* Advance the machine.
 *
 *   link_up   physical-link bit for (slave, port) from the DL status just
 *             polled. Consulted only in REC_CLOSED.
 *   chain_ok  non-zero when the slaves beyond the gap answer again.
 *             Consulted only in REC_SETTLE.
 *
 * rec_needs_link() / rec_needs_chain() say which one this cycle needs.
 * Returns the action the caller must now perform. */
static inline RecAction rec_step(RecCtx *r, uint64_t now, int link_up,
                                 int chain_ok)
{
    switch (r->state) {
    case REC_IDLE:
        return REC_ACT_NONE;

    case REC_CLOSED:
        /* The close is issued exactly once, on the first step after the
         * drop. Keyed on a flag rather than on `now == t_drop`, so a caller
         * that steps the machine a cycle late still closes the port. */
        if (!r->closed_issued) { r->closed_issued = 1; return REC_ACT_CLOSE_PORT; }
        if (link_up) { r->t_link_back = now; r->state = REC_DEBOUNCE;
                       return REC_ACT_NONE; }
        if (now - r->t_drop >= r->link_timeout_ns) {
            r->timeouts++;
            r->state = REC_IDLE;
            return REC_ACT_GIVE_UP;
        }
        return REC_ACT_NONE;

    case REC_DEBOUNCE:
        if (now - r->t_link_back >= r->debounce_ns) {
            r->state = REC_SETTLE;
            r->t_reopened = now;
            return REC_ACT_OPEN_PORT;
        }
        return REC_ACT_NONE;

    case REC_SETTLE: {
        /* Reopening the port is not instantaneous: the link behind it has to
         * come up before the slaves are addressable. Run 7 of SAGENTIA.md
         * re-initialised 152 ms after the reopen and every slave failed with
         * AL code 0x0000 — no error signalled, no state timeout, simply no
         * answer. TwinCAT's ~40 ms is measured from a different moment: it
         * had already watched the link return while the port was still
         * forced closed. */
        if (chain_ok) {
            uint64_t st = now - r->t_reopened;
            r->settle_ns_total += st;
            if (st > r->settle_ns_max) r->settle_ns_max = st;
            r->state = REC_REINIT;
            return REC_ACT_NONE;
        }
        if (now - r->t_reopened >= r->settle_timeout_ns) {
            r->timeouts++;
            r->state = REC_IDLE;
            return REC_ACT_GIVE_UP_SETTLE;
        }
        return REC_ACT_NONE;
    }

    case REC_REINIT: {
        uint64_t outage = now - r->t_drop;
        r->outage_ns_total += outage;
        if (outage > r->outage_ns_max) r->outage_ns_max = outage;
        r->recoveries++;
        r->state = REC_IDLE;
        return REC_ACT_REINIT;
    }
    }
    return REC_ACT_NONE;
}

/* ── Port loop control, register 0x0101 ─────────────────────────────────────
 * Two bits per port: 00 auto, 01 auto-close, 10 open, 11 closed. TwinCAT's
 * chain sits at 0xF4 (port 0 auto, port 1 auto-close, ports 2/3 closed) and
 * it writes 0xFC to force port 1 closed — which is exactly what these produce
 * for port 1, and t_recovery pins that.
 *
 * DECODE STATUS: the 2-bits-per-port layout is ETG.1000.6 and matches the
 * captured 0xF4/0xFC pair; treat the raw byte as authoritative. */
static inline uint8_t rec_loop_set(uint8_t base, int port, uint8_t bits)
{
    int sh = 2 * port;
    return (uint8_t)((base & ~(0x3u << sh)) | ((bits & 0x3u) << sh));
}
static inline uint8_t rec_loop_closed(uint8_t base, int port)
{ return rec_loop_set(base, port, 0x3); }          /* forced closed */
static inline uint8_t rec_loop_auto(uint8_t base, int port)
{ return rec_loop_set(base, port, 0x1); }          /* auto-close    */

/* Physical-link bit for `port` out of the 16-bit DL status at 0x0110.
 * Bits 4..7 are the per-port physical link; bits 8+ are loop/communication.
 * TWINCAT.md records TwinCAT watching 0x0111 bit 3, which is the port-1
 * COMMUNICATION bit; we gate on the physical link instead, because that is
 * the event being timed, and log the raw word either way.
 * DECODE STATUS: unverified against ETG.1000.6 — raw value is authoritative. */
static inline int rec_link_up(uint16_t dl_status, int port)
{ return (dl_status >> (4 + port)) & 1; }

#endif /* ECAT_RECOVERY_H */
