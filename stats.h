#ifndef ECAT_STATS_H
#define ECAT_STATS_H
#include "ecat_common.h"

/* ── Frame layout helpers ───────────────────────────────────────────────── */
typedef struct {
    uint8_t  cmd;
    uint8_t  idx;
    uint32_t addr;      /* network byte order */
    uint16_t len_flags; /* network byte order: bits[10:0]=len, bit[15]=more */
    uint16_t irq;       /* network byte order */
    /* data follows, then 2-byte WKC */
} __attribute__((packed)) EcatDg;

/* ── Statistics ─────────────────────────────────────────────────────────── */
typedef struct {
    /* Per-slave ESC counters (accumulated from 8-bit wrap-around registers).
     * Written only by the RX thread. */
    uint64_t esc_crc[MAX_SLAVES][4];      /* ports 0-3 */
    uint64_t esc_lostlnk[MAX_SLAVES][4]; /* ports 0-3 */
    uint8_t  esc_crc_prev[MAX_SLAVES][4];
    uint8_t  esc_lostlnk_prev[MAX_SLAVES][4];
    /* The APRD at 0x0300 already fetches 16 bytes every frame; only the four
     * invalid-frame counters were ever used. These are the rest of that same
     * datagram — no extra frames, no extra bandwidth. Layout per Beckhoff ESC
     * Section II Register Description v3.3 §2.9:
     *   0x0300+y*2 invalid frame counter port y   (was already tracked)
     *   0x0301+y*2 RX error counter port y        <- esc_rxerr
     *   0x0308+y   forwarded RX error counter     <- esc_fwderr
     *   0x030C     ECAT processing unit errors    <- esc_puerr
     *   0x030D     PDI0 error counter             <- esc_pdierr
     * All are 8-bit and STOP at 0xFF. Once saturated the delta accumulation
     * reads zero forever, so the last raw value is kept alongside to make
     * saturation visible instead of looking like "no more errors". */
    uint64_t esc_rxerr[MAX_SLAVES][4];
    uint64_t esc_fwderr[MAX_SLAVES][4];
    uint64_t esc_puerr[MAX_SLAVES];
    uint64_t esc_pdierr[MAX_SLAVES];
    uint8_t  esc_rxerr_prev[MAX_SLAVES][4];
    uint8_t  esc_fwderr_prev[MAX_SLAVES][4];
    uint8_t  esc_puerr_prev[MAX_SLAVES];
    uint8_t  esc_pdierr_prev[MAX_SLAVES];
    uint8_t  esc_raw_crc[MAX_SLAVES][4];    /* last raw value, for saturation */
    uint8_t  esc_raw_rxerr[MAX_SLAVES][4];
    uint8_t  esc_raw_fwderr[MAX_SLAVES][4];
    uint8_t  esc_raw_puerr[MAX_SLAVES];
    uint8_t  esc_raw_pdierr[MAX_SLAVES];
    /* 0x0314-0x0317 extended RX error: counts even when the port is CLOSED,
     * unlike 0x0300-0x030B which only count while the loop is open. */
    uint64_t esc_extrx[MAX_SLAVES][4];
    uint8_t  esc_extrx_prev[MAX_SLAVES][4];
    uint8_t  esc_raw_extrx[MAX_SLAVES][4];
    /* 0x0320-0x0327 RX ERROR CODE, 2 bytes per port. Not a counter: it names
     * the REASON for the first event that took the extended counter to 1
     * (ESC Sec II v3.3 §2.9.7). Latched value, so we keep the last non-zero
     * one seen per port. This is the register that can say "IFG too short"
     * (0x58), "FIFO overrun/underrun" (0x50/0x51) or "RX_CLK too fast/slow"
     * (0x20/0x21) instead of merely "a port errored". */
    uint16_t esc_rxcode[MAX_SLAVES][4];
    uint64_t esc_rxcode_seen[MAX_SLAVES][4];
    /* BASELINE ON FIRST READ. These counters are cumulative IN THE SLAVE and
     * survive across sessions, so accumulating from prev=0 imported whatever
     * the slave had collected since power-on and reported it as if this run
     * had found it. On a 7-slave chain that showed 82 RX errors on one port
     * and 125 on another for a run that was in fact completely clean.
     * The first FCS-valid WKC==1 read per slave now SETS the baseline instead
     * of accumulating. The pre-existing values are kept and reported
     * separately, so the history is visible but never counted as this run's. */
    uint8_t  esc_baselined[MAX_SLAVES];
    uint8_t  esc_base_crc[MAX_SLAVES][4];
    uint8_t  esc_base_rxerr[MAX_SLAVES][4];
    uint8_t  esc_base_fwderr[MAX_SLAVES][4];
    uint8_t  esc_base_lost[MAX_SLAVES][4];
    uint8_t  esc_base_extrx[MAX_SLAVES][4];
    uint8_t  esc_base_puerr[MAX_SLAVES];
    uint8_t  esc_base_pdierr[MAX_SLAVES];
    uint16_t esc_base_rxcode[MAX_SLAVES][4];
    uint32_t brd_wkc_expected;   /* = num_slaves */
    /* Cross-thread counters — atomic. Owner in comment. */
    _Atomic uint64_t frames_enqueued;  /* TX thread — send() accepted (ring)   */
    _Atomic uint64_t frames_transmitted;/* errq thread — TX ts confirmed on wire*/
    _Atomic uint64_t frames_received;  /* RX thread                            */
    _Atomic uint64_t frames_lost;      /* TX thread (retire)                   */
    _Atomic uint64_t seq_bad;          /* RX thread                            */
    _Atomic uint64_t tx_backpressure;  /* TX thread — EAGAIN/ENOBUFS retries   */
    _Atomic uint64_t credit_writeoff_events;/* TX thread — valve fired count    */
    _Atomic uint64_t brd_wkc_mismatches;/* RX thread                           */
    _Atomic uint64_t distinct_returns; /* RX thread — deduped returned seqs     */
    _Atomic uint64_t rx_len_errors;    /* RX thread — frame length != TX length */
    _Atomic uint64_t kernel_drops;     /* main thread, from PACKET_STATISTICS  */
    _Atomic uint64_t payload_crc_errors;/* RX thread — CRC32C mismatch         */
    _Atomic uint64_t rx_bad_fcs_auxdata;/* RX thread — kernel PACKET_AUXDATA    */
    _Atomic uint64_t rx_bad_fcs_computed;/* RX thread — self-computed Eth FCS   */
    _Atomic uint64_t rx_truncated;     /* RX thread — frame too short to parse  */
    /* Frames whose EtherType is not 0x88A4 because the leading bytes of the
     * frame never arrived (prefix loss). Before the socket was moved to
     * ETH_P_ALL these were discarded by the kernel's protocol dispatch and
     * counted only in netdev rx_dropped — they are damage, and they were
     * previously invisible. See README §4.5. */
    _Atomic uint64_t rx_hdr_damaged;   /* RX thread — EtherType destroyed       */
    _Atomic uint64_t tx_ts_completions;/* errqueue thread — sw TX timestamps   */
    /* On-wire TX packet count (ethtool tx_packets). Owned by supervisor. */
    uint64_t tx_wire_packets;          /* cumulative, from ethtool             */
    uint64_t tx_wire_base;             /* baseline at start                    */
    uint64_t tx_err_base;              /* TxER baseline                        */
    uint64_t qdisc_drop_base;          /* qdisc tx_dropped baseline            */
    /* HOST-SIDE RX baselines. The wire-truth model was asymmetric: TX had
     * TxOk/TxER/qdisc-drop each separated with a documented rationale, while
     * RX had only the AF_PACKET socket's tp_drops. A frame dropped in the NIC
     * ring or the netdev layer never reaches the socket, so tp_drops stays 0
     * while the frame is counted as wire LOSS. These close that gap. */
    uint64_t rx_missed_base;           /* NIC ring overflow (rx_missed_errors) */
    uint64_t rx_dropped_base;          /* netdev rx_dropped                    */
    uint64_t rx_fifo_base;             /* rx_fifo_errors                       */
    uint64_t rx_nic_err_base;          /* rx_errors                            */
    uint64_t rx_nic_crc_base;          /* rx_crc_errors                        */
    /* REAL qdisc drops, read via tc at session start and end only (nic.c).
     * Distinct from qdisc_drop_base above, which is the netdev-level
     * statistics/tx_dropped and reads 0 on this rig while fq_codel drops
     * ~100M frames per run. Excluded from BER either way — never on wire. */
    uint64_t qdisc_real_start;
    uint64_t qdisc_real_end;
    int      qdisc_real_ok;            /* 0 = tc unavailable / unparseable     */
    int      tx_ts_supported;          /* 1 if sw TX timestamping active       */
    /* Link-loss tracking. sysfs counter deltas owned by supervisor; nl_* by the
     * netlink cross-check thread; carrier-poll events + link_state_up by the
     * POLLPRI carrier thread. */
    uint64_t carrier_down;             /* sysfs carrier_down_count delta       */
    uint64_t carrier_up;               /* sysfs carrier_up_count delta         */
    uint64_t carrier_changes;          /* sysfs carrier_changes delta          */
    uint64_t carrier_down_base;
    uint64_t carrier_up_base;
    uint64_t carrier_changes_base;
    _Atomic uint64_t link_down_nl;     /* netlink down events (cross-check)    */
    _Atomic uint64_t link_up_nl;       /* netlink up events (cross-check)      */
    _Atomic uint64_t netlink_overflows;/* netlink NETLINK_OVERRUN/ENOBUFS      */
    _Atomic uint64_t link_down_cp;     /* POLLPRI carrier down events          */
    _Atomic uint64_t link_up_cp;       /* POLLPRI carrier up events            */
    _Atomic int      link_state_up;    /* current carrier state (POLLPRI owns) */
} Stats;

/* g_stats is defined here (ahead of the SeqTrack helpers that update it). */
extern Stats g_stats;

/* ── RX-driven accounting (high-water mark) ─────────────────────────────────
 * Accounting is resolved EXCLUSIVELY by good received frames, never by any
 * TX-side signal. Frames are sent in strict seq order, so a good frame
 * returning with seq N proves that seqs 0..N were all put on the wire (a later
 * frame cannot return before an earlier one on a FIFO wire/loopback). This lets
 * a single good frame back-calculate loss across an entire gap.
 *
 * INVARIANT: the high-water mark `hw` = highest seq that has returned GOOD, +1.
 * When a good frame returns with seq N >= hw, every seq in [hw, N) was provably
 * transmitted, and is resolved as exactly one of:
 *   - returned corrupt  (recorded when the corrupt frame arrived) — counted as
 *     a corruption event, NOT loss.
 *   - absent            (no return recorded) — counted as WIRE LOSS now.
 * Then hw = N+1.
 *
 * Frames at/above hw are simply NOT YET COUNTED — not loss, not anything —
 * until a later good frame resolves them. Kernel-buffered/dropped frames during
 * an outage therefore never get miscounted: they sit above hw until the first
 * good frame on link recovery resolves the whole gap in one step.
 *
 * TERMINATION: the test may only conclude on a good frame (which resolves
 * everything up to it). If the link is down at the intended end, we wait,
 * warning once per second up to 10 times, and error out if no good frame comes.
 *
 * All seq values are uint64 — at 8127 fps a 32-bit counter would wrap in ~6
 * days; uint64 never wraps within any realistic run. The window is a
 * power-of-two ring indexed by (seq & MASK).
 *
 * SIZING: seq increments once per SUCCESSFUL send(), NOT once per frame on the
 * wire. Those differ by a lot: TX never halts, so it offers frames far faster
 * than the 8127 fps link drains them and the qdisc discards the excess. A
 * measured 1857 s run enqueued 114.7M while TxOk was 15.1M — a peak send rate
 * near 130k/s, so the 1M-entry window spans about 8 s of enqueueing, not the
 * ~129 s a naive wire-rate calculation gives. Still enormous next to the
 * in-flight depth (microseconds of chain RTT), so a slot is never aliased
 * before hw passes it — but size this against the ENQUEUE rate, not the wire
 * rate, if the window is ever reduced.
 *
 * THREADING: the RX thread is the SOLE owner of all accounting state (hw, the
 * returned records, loss/corruption counters). No other thread writes it, so no
 * locking is needed. TX reads no accounting state back. */
#define SEQ_WINDOW (1u << 20)         /* 1,048,576 — ~8 s at peak enqueue rate */
#define SEQ_MASK   (SEQ_WINDOW - 1)

typedef struct {
    /* Per-slot record of which seqs have already been counted as GOOD returns,
     * for DEDUPLICATION only (e.g. the lo interface double-delivers). A slot is
     * a genuine new good return iff not already seen. Owned by RX. */
    uint64_t ret_seq[SEQ_WINDOW];
    uint8_t  ret_seen[SEQ_WINDOW];   /* 1 once this seq has been counted good */
    uint64_t max_good_seq;           /* highest GOOD-return seq seen           */
    int      max_good_valid;         /* 0 until the first good frame           */
} RxAccount;
extern RxAccount g_rx;


/* Record a GOOD returned frame (FCS-valid AND payload-CRC-valid, so its seq is
 * trustworthy), deduplicated. Returns 1 if this is the FIRST good sighting of
 * this seq (caller should count it toward distinct good returns), 0 if it's a
 * duplicate delivery. Also advances max_good_seq (used only for the good-frame-
 * gated termination). RX-thread only. */
static inline int rx_new_good_return(uint64_t seq) {
    if (!g_rx.max_good_valid || seq > g_rx.max_good_seq) {
        g_rx.max_good_seq  = seq;
        g_rx.max_good_valid = 1;
    }
    uint32_t slot = seq & SEQ_MASK;
    if (g_rx.ret_seq[slot] == seq && g_rx.ret_seen[slot])
        return 0;                    /* duplicate */
    g_rx.ret_seq[slot]  = seq;
    g_rx.ret_seen[slot] = 1;
    return 1;                        /* first good sighting */
}
/* ── Wire-truth boundary (hardware tally counters) ──────────────────────────
 * TxOk = frames the r8169 MAC actually clocked onto the wire (hardware DTC
 * counter, via ethtool GSTATS). This is the measurement boundary: "the r8169
 * and everything downstream". Sampled periodically into g_txok_delta by the
 * sampler thread (base subtracted so it's a per-run count).
 *
 * LOSS (physical-layer) = TxOk − distinct frames returned. A frame that reached
 * the wire (counted in TxOk) but never came back is a genuine wire loss. Frames
 * the KERNEL dropped above the r8169 (qdisc tx_dropped) are never in TxOk, so
 * they are excluded automatically. Frames the r8169 rejected at carrier-lost
 * (TxER) are also not in TxOk, so also excluded — reported separately as
 * link-down collateral. BER denominator = TxOk. */
extern _Atomic uint64_t g_txok;  /* per-run TxOk (frames on wire)     */
extern _Atomic uint64_t g_txer;  /* per-run TxER (carrier-lost etc.)  */
extern _Atomic uint64_t g_qdisc_drop;  /* per-run qdisc tx_dropped (kernel) */
/* Per-run host-side RX counters, base-subtracted. Any of these being nonzero
 * means frames were lost ABOVE the wire and the loss figure is contaminated. */
extern _Atomic uint64_t g_rx_missed;   /* NIC ring overflow                 */
extern _Atomic uint64_t g_rx_dropped;  /* netdev rx_dropped                 */
extern _Atomic uint64_t g_rx_fifo;     /* rx_fifo_errors                    */
extern _Atomic uint64_t g_rx_nic_err;  /* rx_errors                         */
extern _Atomic uint64_t g_rx_nic_crc;  /* rx_crc_errors                     */

/* ── External pause/resume, for out-of-process register probing ─────────────
 * A probe tool (ecat_phy) shares the interface, and its frames would corrupt
 * this measurement three ways: they inflate TxOk (an interface-wide hardware
 * counter) creating phantom loss; their replies reach our promiscuous
 * ETH_P_ECAT socket and are parsed as corrupt frames; and they queue in the
 * socket buffer if we simply stop.
 *
 * SIGUSR1 pauses TX, SIGUSR2 resumes. While paused this process transmits
 * NOTHING, so any TxOk increase in that window is foreign BY DEFINITION and is
 * subtracted exactly — no IPC and no cooperation from the probe is needed.
 *
 * This is NOT the banned TX pause of README §3.5. Those five designs deadlocked
 * because TX waited on a condition its own stalling prevented from clearing.
 * Here the resume condition is external, and a watchdog force-resumes if the
 * probe dies, so no feedback loop exists and no deadlock is possible. */
extern _Atomic int      g_tx_paused;    /* TX thread: stop sending while set  */
extern _Atomic int      g_rx_discard;   /* RX thread: drain but do not count  */
extern _Atomic int      g_pause_req;    /* set by SIGUSR1 handler             */
extern _Atomic int      g_resume_req;   /* set by SIGUSR2 handler             */
extern _Atomic uint64_t g_foreign_txok; /* frames other processes put on wire */
extern _Atomic uint64_t g_pause_count;
extern _Atomic uint64_t g_pause_forced; /* watchdog had to resume us          */
extern _Atomic uint64_t g_rx_foreign;   /* non-NOP frames seen while counting */

/* Actual on-wire bits per frame, published by the TX thread after the first
 * build_frame(): (frame_len + 4-byte FCS) * 8. These are exactly the
 * CRC-protected bits — the bits whose corruption the FCS/payload detectors can
 * observe — which is the principled BER denominator (preamble/SFD/IFG line
 * symbols are excluded: an error there cannot produce a CRC count). 0 until
 * the first frame is built. */
extern _Atomic uint64_t g_wire_bits_per_frame;
/* Bytes actually covered by the payload CRC32C. Depends on slave count: more
 * slaves means more datagrams and a smaller NOP payload, so the payload
 * detector covers less of the frame (94% at 1 slave, ~70% at 8). Published by
 * build_frame so the panel reports the real figure instead of a stale one. */
extern _Atomic uint64_t g_payload_crc_bytes;

/* Rx frame length check: every TX frame in a run has the SAME size (published
 * in g_wire_bits_per_frame as (frame_len + FCS) * 8), so every return must
 * arrive with exactly that size. raw_len is the delivered length, which
 * includes the 4-byte FCS trailer iff rx-fcs is on. Returns 1 (and counts) on
 * a length mismatch; 0 if the length is correct or the expected size is not
 * yet known (TX hasn't built the first frame). */
static inline int rx_check_len(int raw_len, int rx_fcs_on) {
    uint64_t bpf = atomic_load_explicit(&g_wire_bits_per_frame, memory_order_relaxed);
    if (!bpf) return 0;
    int expected = (int)(bpf / 8) - (rx_fcs_on ? 0 : 4);
    if (raw_len != expected) {
        atomic_fetch_add_explicit(&g_stats.rx_len_errors, 1, memory_order_relaxed);
        return 1;
    }
    return 0;
}


/* Cross-module run state (defined in stats.c; g_running/g_tx_running/
 * g_start_ns are declared in ecat_common.h). */
extern _Atomic uint64_t g_tx_final_seq;   /* seq count at TX stop */
extern FILE            *g_linkev_csv;     /* per-event link log (may be NULL) */

/* ── Bad-FCS event ring (SPSC, lock-free) ───────────────────────────────────
 * The RX thread must NEVER block on console I/O (a slow terminal would stall
 * the drain → kernel RX drops → BER invalid). So bad-FCS events are pushed
 * into this single-producer (RX thread) / single-consumer (supervisor)
 * lock-free ring; the supervisor drains and prints them on its 200ms tick,
 * capped at BADFCS_PRINT_CAP prints per stats interval. Push on a full ring
 * drops the event (counted) — it never waits. */
#define BADFCS_RING       4096          /* power of two */
#define BADFCS_PRINT_CAP  1000          /* prints per stats interval */
typedef struct { uint64_t t_ns; int32_t len; uint32_t tp; } BadFcsEv;
extern BadFcsEv g_bfe[BADFCS_RING];
extern _Atomic uint64_t g_bfe_head;      /* producer-written */
extern _Atomic uint64_t g_bfe_tail;      /* consumer-written */
extern _Atomic uint64_t g_bfe_ringdrop;  /* events dropped, ring full */

static inline void badfcs_push(uint32_t tp, int len) {
    uint64_t h = atomic_load_explicit(&g_bfe_head, memory_order_relaxed);
    uint64_t t = atomic_load_explicit(&g_bfe_tail, memory_order_acquire);
    if (h - t >= BADFCS_RING) {          /* full — drop, never block */
        atomic_fetch_add_explicit(&g_bfe_ringdrop, 1, memory_order_relaxed);
        return;
    }
    g_bfe[h & (BADFCS_RING - 1)] =
        (BadFcsEv){ now_ns() - g_start_ns, (int32_t)len, tp };
    atomic_store_explicit(&g_bfe_head, h + 1, memory_order_release);
}

extern pthread_mutex_t g_linkev_mtx;   /* serialises link-event CSV writes */

/* Supervisor-side drain of the bad-FCS ring: prints up to *budget events,
 * counts the rest into *suppressed. Supervisor thread only (owns console). */
void badfcs_drain(int *budget, uint64_t *suppressed);

void print_stats(FILE *csv, uint64_t elapsed_ns);
void write_csv_header(FILE *csv, int num_slaves);
/* Decode an ESC RX error code (0x0320+2y) into a human-readable reason.
 * Values per Beckhoff ESC Section II Register Description v3.3 §2.9.7 —
 * these say WHY a port errored (IFG too short, FIFO over/underrun,
 * RX_CLK/TX_CLK out of tolerance, ...). Defined in stats.c. */
const char *esc_rx_error_code_name(uint16_t c);

#endif /* ECAT_STATS_H */
