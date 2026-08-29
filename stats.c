/* Statistics, accounting state, bad-FCS ring consumer, and reporting. */
#include "stats.h"

/* ── Definitions of shared state (extern-declared in headers) ───────────── */
Stats g_stats;
RxAccount g_rx;
_Atomic uint64_t g_txok = 0;
_Atomic uint64_t g_txer = 0;
_Atomic uint64_t g_qdisc_drop = 0;
_Atomic uint64_t g_payload_crc_bytes = 0;
_Atomic uint64_t g_rx_missed  = 0;
_Atomic uint64_t g_rx_dropped = 0;
_Atomic uint64_t g_rx_fifo    = 0;
_Atomic uint64_t g_rx_nic_err = 0;
_Atomic uint64_t g_rx_nic_crc = 0;
_Atomic int      g_tx_paused    = 0;
_Atomic int      g_rx_discard   = 0;
_Atomic int      g_pause_req    = 0;
_Atomic int      g_resume_req   = 0;
_Atomic uint64_t g_foreign_txok = 0;
_Atomic uint64_t g_pause_count  = 0;
_Atomic uint64_t g_pause_forced = 0;
_Atomic uint64_t g_rx_foreign   = 0;
_Atomic uint64_t g_wire_bits_per_frame = 0;
BadFcsEv g_bfe[BADFCS_RING];
_Atomic uint64_t g_bfe_head = 0;
_Atomic uint64_t g_bfe_tail = 0;
_Atomic uint64_t g_bfe_ringdrop = 0;
volatile sig_atomic_t g_running = 1;
volatile sig_atomic_t g_tx_running = 1;
_Atomic uint64_t g_tx_final_seq = 0;
FILE            *g_linkev_csv = NULL;
uint64_t g_start_ns = 0;
pthread_mutex_t g_linkev_mtx = PTHREAD_MUTEX_INITIALIZER;
int g_verbose = 0;
int g_loopback = 0;
int g_num_slaves = 0;

/* Supervisor-side drain: consume all pending events, printing up to *budget
 * of them (decrementing it) and counting the rest into *suppressed. Called
 * only from the supervisor thread, which owns slow console I/O. */
void badfcs_drain(int *budget, uint64_t *suppressed) {
    uint64_t h = atomic_load_explicit(&g_bfe_head, memory_order_acquire);
    uint64_t t = atomic_load_explicit(&g_bfe_tail, memory_order_relaxed);
    while (t < h) {
        BadFcsEv ev = g_bfe[t & (BADFCS_RING - 1)];
        if (*budget > 0) {
            (*budget)--;
            fprintf(stderr, "[bad-FCS frame] t=+%.3fs tp_status=0x%08x len=%d\n",
                    ev.t_ns / 1e9, ev.tp, ev.len);
        } else {
            (*suppressed)++;
        }
        t++;
    }
    atomic_store_explicit(&g_bfe_tail, t, memory_order_release);
}

/* RX error code values, Beckhoff ESC Section II Register Description v3.3,
 * table accompanying §2.9.7. These name WHY a port errored — the difference
 * between "a link dropped" and "the inter-frame gap was too short". */
const char *esc_rx_error_code_name(uint16_t c) {
    switch (c & 0xFF) {
    case 0x20: return "RX_CLK too fast";
    case 0x21: return "RX_CLK too slow";
    case 0x22: return "RX_CLK other error";
    case 0x23: return "TX_CLK too fast";
    case 0x24: return "TX_CLK too slow";
    case 0x25: return "TX_CLK other error";
    case 0x28: return "enhanced link detection prevents link";
    case 0x29: return "MI link detection prevents link";
    case 0x2A: return "unsupported link speed or half duplex";
    case 0x30: return "false carrier";
    case 0x31: return "false carrier extend (RGMII)";
    case 0x32: return "bad SSD";
    case 0x40: return "RX_ER in frame";
    case 0x50: return "FIFO OVERRUN";
    case 0x51: return "FIFO UNDERRUN";
    case 0x52: return "FIFO other error";
    case 0x58: return "FRAME DROPPED (inter-frame gap too short)";
    case 0x60: return "checksum error";
    case 0x61: return "forwarded error (checksum, closed port)";
    case 0x62: return "frame without SFD";
    case 0x63: return "frame too long (>2 KB)";
    case 0x70: return "frame shorter than expected";
    case 0x71: return "frame longer than expected";
    case 0x72: return "circulating frame";
    case 0x77: return "other EtherCAT processing unit error";
    default:   return "unknown code";
    }
}

/* ── Print stats ────────────────────────────────────────────────────────── */
void print_stats(FILE *csv, uint64_t elapsed_ns) {
    double elapsed_s  = elapsed_ns / 1e9;

    /* Snapshot atomics once for a consistent-ish view. */
    uint64_t sent     = atomic_load_explicit(&g_stats.frames_enqueued, memory_order_relaxed);
    uint64_t rcvd     = atomic_load_explicit(&g_stats.frames_received, memory_order_relaxed);
    uint64_t distinct = atomic_load_explicit(&g_stats.distinct_returns, memory_order_relaxed);
    uint64_t backp    = atomic_load_explicit(&g_stats.tx_backpressure, memory_order_relaxed);
    uint64_t wkcmm    = atomic_load_explicit(&g_stats.brd_wkc_mismatches, memory_order_relaxed);
    uint64_t kdrops   = atomic_load_explicit(&g_stats.kernel_drops, memory_order_relaxed);
    uint64_t plcrc    = atomic_load_explicit(&g_stats.payload_crc_errors, memory_order_relaxed);
    uint64_t tsc      = atomic_load_explicit(&g_stats.tx_ts_completions, memory_order_relaxed);
    uint64_t wire     = g_stats.tx_wire_packets;

    /* Wire-truth boundary. */
    /* TxOk is an INTERFACE-wide hardware counter, so it also counts frames put
     * on the wire by an external probe while we were paused. We transmit
     * nothing while paused, so that delta is foreign by definition and is
     * subtracted here — otherwise each probe frame becomes a phantom loss and
     * inflates the BER denominator. */
    uint64_t txok_raw = atomic_load_explicit(&g_txok, memory_order_relaxed);
    uint64_t foreign  = atomic_load_explicit(&g_foreign_txok, memory_order_relaxed);
    uint64_t txok     = (txok_raw >= foreign) ? txok_raw - foreign : 0;
    uint64_t txer  = atomic_load_explicit(&g_txer, memory_order_relaxed);
    uint64_t qdrop = atomic_load_explicit(&g_qdisc_drop, memory_order_relaxed);

    /* LOSS = frames that reached the wire (TxOk) but never returned. Clamped:
     * during the run a few frames are legitimately in flight (TxOk counted,
     * not yet returned), and the async TxOk sample may briefly lag returns, so
     * a small transient negative is clamped to 0. Exact once settled. */
    int64_t lost_signed = (int64_t)txok - (int64_t)distinct;
    uint64_t lost = (lost_signed > 0) ? (uint64_t)lost_signed : 0;

    /* BER denominator = frames that actually reached the wire (TxOk). */
    /* BER denominator: TxOk × the ACTUAL on-wire bits per frame (frame + FCS,
     * published by TX after the first build) — not a hard-coded approximation. */
    uint64_t bits_per_frame = atomic_load_explicit(&g_wire_bits_per_frame, memory_order_relaxed);
    uint64_t total_bits = txok * bits_per_frame;

    /* Effective WIRE throughput since last call. Computed from TxOk (frames
     * actually clocked onto the wire), NOT from enqueued — during an outage TX
     * offers frames far faster than the wire rate (they are kernel-dropped), so
     * an enqueued-based rate reads well above the 100BASE-TX line rate. TxOk is
     * the true on-wire rate. */
    static uint64_t prev_txok = 0, prev_ns = 0;
    double fps = 0.0;
    if (prev_ns && elapsed_ns > prev_ns)
        fps = (double)(txok - prev_txok) / ((elapsed_ns - prev_ns) / 1e9);
    prev_txok = txok; prev_ns = elapsed_ns;

    printf("\n── EtherCAT BER Test ─────────────────────────────────────\n");
    printf("  Elapsed:        %.1f s\n", elapsed_s);
    printf("  ── TX pipeline (diagnostic) ────────────────────────────\n");
    printf("  TX enqueued (ring):      %lu\n", sent);
    if (g_stats.tx_ts_supported)
        printf("  TX driver-xmit (sw ts):  %lu\n", tsc);
    printf("  ── Wire boundary (r8169 hardware tally) ────────────────\n");
    if (txok == 0 && sent > 1000) {
        printf("  TxOk: 0  *** hardware tally unavailable on this interface ***\n");
        printf("    (loopback 'lo' has no r8169 tally counters; loss/BER need enp2s0)\n");
    } else {
        printf("  TxOk  (on wire, BER denom): %lu\n", txok);
    }
    printf("  TxER  (r8169 rejected, carrier-lost etc.): %lu  (excluded from BER)\n", txer);
    /* Two DIFFERENT counters; the old panel conflated them. */
    printf("  netdev tx_dropped (driver-level): %lu  (excluded — never on wire)\n", qdrop);

    {
        uint64_t np = atomic_load_explicit(&g_pause_count,  memory_order_relaxed);
        uint64_t nf = atomic_load_explicit(&g_pause_forced, memory_order_relaxed);
        uint64_t rf = atomic_load_explicit(&g_rx_foreign,   memory_order_relaxed);
        if (np || foreign || rf) {
            printf("  \u2500\u2500 External probe pauses \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\n");
            printf("  Pauses: %lu   TxOk raw %lu - foreign %lu = %lu used\n",
                   np, txok_raw, foreign, txok);
            if (nf) printf("  *** %lu pause(s) needed the WATCHDOG to resume — the\n"
                           "  *** probe died mid-pause; check the orchestration. ***\n", nf);
            if (rf) printf("  *** %lu foreign EtherCAT frame(s) seen WHILE COUNTING.\n"
                           "  *** Another master is on the wire outside the pause\n"
                           "  *** windows; this run is not isolated. ***\n", rf);
        }
    }    if (g_stats.qdisc_real_ok) {
        uint64_t qreal = (g_stats.qdisc_real_end >= g_stats.qdisc_real_start)
                       ? g_stats.qdisc_real_end - g_stats.qdisc_real_start : 0;
        printf("  qdisc drops (root qdisc, real): %lu  (excluded — never on wire)\n",
               qreal);
        printf("    TX offers far faster than the link drains (§3.5 TX never\n"
               "    halts); the qdisc absorbs the excess. Not an error.\n");
    } else {
        printf("  qdisc drops (root qdisc, real): unknown  (tc unavailable)\n");
    }
    printf("  ── Accounting (wire-side) ──────────────────────────────\n");
    printf("  Frames rcvd (raw):      %lu\n", rcvd);
    printf("  Good distinct returns:  %lu  (FCS+payload valid, deduped)\n", distinct);
    printf("  Wire throughput:%.0f fps  (%.1f Mbit/s)  [from TxOk]\n",
           fps, fps * (double)bits_per_frame / 1e6);
    printf("  Lost frames:    %lu  (TxOk − good returns; includes corrupt)\n", lost);
    printf("  Payload CRC err:%lu  %s\n", plcrc,
           plcrc ? "(frames without a valid payload CRC — invalid or missing)"
                 : "(clean)");
    { uint64_t bfc = atomic_load_explicit(&g_stats.rx_bad_fcs_computed, memory_order_relaxed);
      uint64_t bfa = atomic_load_explicit(&g_stats.rx_bad_fcs_auxdata, memory_order_relaxed);
      uint64_t trunc = atomic_load_explicit(&g_stats.rx_truncated, memory_order_relaxed);
      printf("  Bad FCS (computed/kernel): %lu / %lu  %s\n", bfc, bfa,
             bfc ? "*** CORRUPT FRAMES RECEIVED ***" : "(none)");
      if (trunc)
          printf("  Truncated frames: %lu  (too short to parse)\n", trunc);
      uint64_t lerr = atomic_load_explicit(&g_stats.rx_len_errors, memory_order_relaxed);
      printf("  Rx frame length errors: %lu  %s\n", lerr,
             lerr ? "(received length != TX frame length)" : "(all correct length)");
    }
    printf("  TX backpressure:%lu  (EAGAIN ring-full, normal)\n", backp);
    printf("  Kernel RX drops:%lu  %s\n", kdrops,
           kdrops ? "*** DRAIN SATURATED — BER INVALID ***" : "(none, drain healthy)");

    /* HOST-SIDE RX. Above the wire: a frame dropped here was transmitted and
     * may have returned, but never reached us, so it lands in "lost" without
     * any wire-side error. If any of these are nonzero the loss figure is
     * contaminated and the BER bound is not valid. */
    {
        uint64_t rmiss = atomic_load_explicit(&g_rx_missed,  memory_order_relaxed);
        uint64_t rdrop = atomic_load_explicit(&g_rx_dropped, memory_order_relaxed);
        uint64_t rfifo = atomic_load_explicit(&g_rx_fifo,    memory_order_relaxed);
        uint64_t rerr  = atomic_load_explicit(&g_rx_nic_err, memory_order_relaxed);
        uint64_t rcrc  = atomic_load_explicit(&g_rx_nic_crc, memory_order_relaxed);
        printf("  \u2500\u2500 Host-side RX (above the wire) \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\n");
        printf("  NIC ring overflow (rx_missed): %lu\n", rmiss);
        printf("  netdev rx_dropped: %lu   rx_fifo: %lu\n", rdrop, rfifo);
        printf("  NIC rx_errors: %lu   rx_crc_errors: %lu\n", rerr, rcrc);
        if (rmiss || rfifo || kdrops)
            printf("  *** WARNING: frames were lost ABOVE the wire. The loss\n"
                   "  *** figure below includes host-side drops and the BER\n"
                   "  *** bound is NOT valid for this run. ***\n");
    }    /* ── Link (host NIC) — three independent sources ───────────── */
    uint64_t nl_down = atomic_load_explicit(&g_stats.link_down_nl, memory_order_relaxed);
    uint64_t nl_up   = atomic_load_explicit(&g_stats.link_up_nl, memory_order_relaxed);
    uint64_t nl_ovf  = atomic_load_explicit(&g_stats.netlink_overflows, memory_order_relaxed);
    uint64_t cp_down = atomic_load_explicit(&g_stats.link_down_cp, memory_order_relaxed);
    uint64_t cp_up   = atomic_load_explicit(&g_stats.link_up_cp, memory_order_relaxed);
    int link_up      = atomic_load_explicit(&g_stats.link_state_up, memory_order_relaxed);
    {
        printf("  ── Link (host NIC) ─────────────────────────────────────\n");
        printf("  Now: %s\n", link_up ? "UP" : "DOWN");
        /* Authoritative counts: kernel sysfs counters (never coalesce). */
        printf("  Transitions (sysfs, authoritative): down %lu / up %lu / changes %lu\n",
               g_stats.carrier_down, g_stats.carrier_up, g_stats.carrier_changes);
        if (g_stats.carrier_down + g_stats.carrier_up != g_stats.carrier_changes)
            printf("    (down+up != changes — counters advanced mid-read; benign)\n");
        /* Carrier thread: current-state + timed events (POLLPRI, or fast
         * timed re-read fallback; may coalesce either way). */
        printf("  Timed events (carrier thread):      down %lu / up %lu%s\n",
               cp_down, cp_up,
               (cp_down < g_stats.carrier_down)
                 ? "   (fewer than sysfs: fast flaps coalesced)" : "");
        /* Netlink cross-check (coalesces + can overflow). */
        printf("  Cross-check (netlink):              down %lu / up %lu%s\n",
               nl_down, nl_up,
               (nl_down < g_stats.carrier_down)
                 ? "   (undercounts fast flaps — expected)" : "");
        if (nl_ovf)
            printf("    netlink overflows: %lu  (messages dropped — cross-check incomplete)\n",
                   nl_ovf);
    }
    printf("  BRD WKC mismatches: %lu\n", wkcmm);
    printf("  Total wire bits:%.3e  (%lu bits/frame, frame+FCS, measured)\n",
           (double)total_bits, bits_per_frame);
    if (total_bits > 0) {
        /* BER upper-bound estimator: (errors + 0.5) / N. The +0.5 gives a
         * conservative upper bound even when zero errors have been observed
         * (BER <= 0.5/N), and adds a consistent half-count margin once errors
         * appear. Both numerators come from the tool's own SOFTWARE detectors
         * (not the NIC's hardware CRC counter, which reads 0 under rx-all since
         * bad frames are delivered to us rather than dropped-and-counted):
         *   BER (FCS)     — whole-frame CRC (rx_bad_fcs_computed), ~100% coverage
         *   BER (payload) — payload CRC32C, coverage DEPENDS ON SLAVE COUNT:
         *                   more slaves means more datagrams and a smaller NOP
         *                   payload, so the detector covers less of the frame.
         *                   Reported below from the measured value. For random bit errors the
         *                   two should track within ~2%; a payload count much
         *                   below the FCS count means errors are concentrated in
         *                   the frame headers (front-of-frame link disruption),
         *                   not uniformly-distributed bit errors. */
        uint64_t bfc = atomic_load_explicit(&g_stats.rx_bad_fcs_computed, memory_order_relaxed);
        printf("  BER (FCS) <=:   %.2e   ((%lu + 0.5) / N)  [whole frame]\n",
               ((double)bfc + 0.5) / (double)total_bits, bfc);
        uint64_t plb = atomic_load_explicit(&g_payload_crc_bytes, memory_order_relaxed);
        uint64_t fl  = bits_per_frame / 8;
        printf("  BER (payload)<=:%.2e   ((%lu + 0.5) / N)  [no valid payload CRC,\n"
               "                                     covers %lu of %lu B = %.1f%% of frame]\n",
               ((double)plcrc + 0.5) / (double)total_bits, plcrc,
               plb, fl, fl ? 100.0 * (double)plb / (double)fl : 0.0);
        printf("  Frame loss rate:%.2e  (lost / TxOk)\n",
               txok ? (double)lost / (double)txok : 0.0);
    }

    if (g_verbose && !g_loopback) {
        printf("  ── Per-slave ESC counters (invalid-frame / lost-link) ──\n");
        for (int s = 0; s < g_num_slaves && s < MAX_SLAVES; s++) {
            printf("  Slave %2d CRC:  P0=%lu P1=%lu P2=%lu P3=%lu\n", s,
                   g_stats.esc_crc[s][0], g_stats.esc_crc[s][1],
                   g_stats.esc_crc[s][2], g_stats.esc_crc[s][3]);
            printf("  Slave %2d lost: P0=%lu P1=%lu P2=%lu P3=%lu  (link-down events)\n", s,
                   g_stats.esc_lostlnk[s][0], g_stats.esc_lostlnk[s][1],
                   g_stats.esc_lostlnk[s][2], g_stats.esc_lostlnk[s][3]);

            printf("  Slave %2d rxerr:P0=%lu P1=%lu P2=%lu P3=%lu  (0x0301+2y RX errors)\n", s,
                   g_stats.esc_rxerr[s][0], g_stats.esc_rxerr[s][1],
                   g_stats.esc_rxerr[s][2], g_stats.esc_rxerr[s][3]);
            printf("  Slave %2d fwderr:P0=%lu P1=%lu P2=%lu P3=%lu (0x0308+y forwarded)\n", s,
                   g_stats.esc_fwderr[s][0], g_stats.esc_fwderr[s][1],
                   g_stats.esc_fwderr[s][2], g_stats.esc_fwderr[s][3]);
            printf("  Slave %2d proc-unit err (0x030C): %lu   PDI err (0x030D): %lu\n",
                   s, g_stats.esc_puerr[s], g_stats.esc_pdierr[s]);
            printf("  Slave %2d extRX: P0=%lu P1=%lu P2=%lu P3=%lu  "
                   "(0x0314+y, counts even when port closed)\n", s,
                   g_stats.esc_extrx[s][0], g_stats.esc_extrx[s][1],
                   g_stats.esc_extrx[s][2], g_stats.esc_extrx[s][3]);
            for (int p = 0; p < 4; p++)
                if (g_stats.esc_rxcode[s][p])
                    printf("  Slave %2d P%d RX ERROR CODE 0x%04X = %s  "
                           "(seen %lu distinct)\n", s, p,
                           g_stats.esc_rxcode[s][p],
                           esc_rx_error_code_name(g_stats.esc_rxcode[s][p]),
                           g_stats.esc_rxcode_seen[s][p]);
            /* All these counters STOP at 0xFF (ESC Sec II v3.3 §2.9). Once
             * saturated the delta reads zero forever, which looks identical to
             * "no errors" — so say so explicitly. */
            {
                int sat = 0;
                for (int p = 0; p < 4; p++)
                    if (g_stats.esc_raw_crc[s][p] == 0xFF ||
                        g_stats.esc_raw_rxerr[s][p] == 0xFF ||
                        g_stats.esc_raw_fwderr[s][p] == 0xFF) sat = 1;
                if (g_stats.esc_raw_puerr[s] == 0xFF || g_stats.esc_raw_pdierr[s] == 0xFF)
                    sat = 1;
                if (sat)
                    printf("  Slave %2d *** a counter reads 0xFF = SATURATED; it has\n"
                           "           *** stopped counting and the total above is a\n"
                           "           *** FLOOR. Clear with ecat_escreset. ***\n", s);
            }        }
    }
    /* Distribution of link drops across the chain. Spread roughly evenly ->
     * simply more links, more opportunity. Concentrated toward the far end ->
     * something cumulative. In a chain port 0 faces the master and port 1 the
     * next slave, so the segment between slave N and N+1 shows up as slave N
     * port 1 AND slave N+1 port 0 — two independent witnesses per segment. */
    if (g_verbose && !g_loopback && g_num_slaves > 1) {
        uint64_t tot = 0, worst = 0; int worst_s = -1, worst_p = -1;
        for (int s = 0; s < g_num_slaves && s < MAX_SLAVES; s++)
            for (int p = 0; p < 4; p++) {
                uint64_t v = g_stats.esc_lostlnk[s][p];
                tot += v;
                if (v > worst) { worst = v; worst_s = s; worst_p = p; }
            }
        printf("  ── Link-drop distribution across the chain ───────────────\n");
        if (!tot) {
            printf("  No lost-link events on any slave or port.\n");
        } else {
            printf("  slave:  ");
            for (int s = 0; s < g_num_slaves && s < MAX_SLAVES; s++)
                printf("%5d", s);
            printf("\n  drops:  ");
            for (int s = 0; s < g_num_slaves && s < MAX_SLAVES; s++) {
                uint64_t v = 0;
                for (int p = 0; p < 4; p++) v += g_stats.esc_lostlnk[s][p];
                printf("%5lu", v);
            }
            printf("\n  total %lu; worst slave %d port %d with %lu\n",
                   tot, worst_s, worst_p, worst);
            printf("  [segment N<->N+1 appears as slave N port 1 and slave "
                   "N+1 port 0]\n");
        }
    }
    printf("──────────────────────────────────────────────────────────\n");

    /* CSV row */
    if (csv) {
        uint64_t nl_d = atomic_load_explicit(&g_stats.link_down_nl, memory_order_relaxed);
        uint64_t nl_u = atomic_load_explicit(&g_stats.link_up_nl, memory_order_relaxed);
        uint64_t nl_o = atomic_load_explicit(&g_stats.netlink_overflows, memory_order_relaxed);
        uint64_t cp_d = atomic_load_explicit(&g_stats.link_down_cp, memory_order_relaxed);
        uint64_t cp_u = atomic_load_explicit(&g_stats.link_up_cp, memory_order_relaxed);
        uint64_t bfc  = atomic_load_explicit(&g_stats.rx_bad_fcs_computed, memory_order_relaxed);
        uint64_t bfa  = atomic_load_explicit(&g_stats.rx_bad_fcs_auxdata, memory_order_relaxed);
        uint64_t trunc= atomic_load_explicit(&g_stats.rx_truncated, memory_order_relaxed);
        uint64_t lerr = atomic_load_explicit(&g_stats.rx_len_errors, memory_order_relaxed);
        fprintf(csv, "%.3f,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,"
                     "%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,"
                     "%lu,%lu,%lu,%lu,%lu,%ld,%ld",
                elapsed_s, sent, wire, txok, txer, qdrop, distinct, rcvd, lost,
                plcrc, wkcmm, kdrops,
                g_stats.carrier_down, g_stats.carrier_up, g_stats.carrier_changes,
                cp_d, cp_u, nl_d, nl_u, nl_o, bfc, bfa, trunc, lerr,
                atomic_load_explicit(&g_rx_missed,  memory_order_relaxed),
                atomic_load_explicit(&g_rx_dropped, memory_order_relaxed),
                atomic_load_explicit(&g_rx_fifo,    memory_order_relaxed),
                atomic_load_explicit(&g_rx_nic_err, memory_order_relaxed),
                atomic_load_explicit(&g_rx_nic_crc, memory_order_relaxed),
                (long)lost_signed,
                g_stats.qdisc_real_ok
                    ? (long)(g_stats.qdisc_real_end >= g_stats.qdisc_real_start
                             ? g_stats.qdisc_real_end - g_stats.qdisc_real_start : 0)
                    : -1L);
        for (int s = 0; s < g_num_slaves && s < MAX_SLAVES; s++)
            fprintf(csv, ",%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu"
                         ",%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu"
                         ",%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu",
                    g_stats.esc_crc[s][0], g_stats.esc_crc[s][1],
                    g_stats.esc_crc[s][2], g_stats.esc_crc[s][3],
                    g_stats.esc_lostlnk[s][0], g_stats.esc_lostlnk[s][1],
                    g_stats.esc_lostlnk[s][2], g_stats.esc_lostlnk[s][3],
                    g_stats.esc_rxerr[s][0], g_stats.esc_rxerr[s][1],
                    g_stats.esc_rxerr[s][2], g_stats.esc_rxerr[s][3],
                    g_stats.esc_fwderr[s][0], g_stats.esc_fwderr[s][1],
                    g_stats.esc_fwderr[s][2], g_stats.esc_fwderr[s][3],
                    g_stats.esc_puerr[s], g_stats.esc_pdierr[s],
                    g_stats.esc_extrx[s][0], g_stats.esc_extrx[s][1],
                    g_stats.esc_extrx[s][2], g_stats.esc_extrx[s][3],
                    (uint64_t)g_stats.esc_rxcode[s][0], (uint64_t)g_stats.esc_rxcode[s][1],
                    (uint64_t)g_stats.esc_rxcode[s][2], (uint64_t)g_stats.esc_rxcode[s][3]);
        fprintf(csv, "\n");
        fflush(csv);
    }
}

/* ── CSV header ─────────────────────────────────────────────────────────── */
void write_csv_header(FILE *csv, int num_slaves) {
    fprintf(csv, "elapsed_s,tx_enqueued,tx_wire,txok,txer,qdisc_drop,"
                 "distinct_returns,frames_rcvd,frames_lost,"
                 "payload_crc_errors,"
                 "brd_wkc_mismatches,kernel_rx_drops,"
                 "carrier_down,carrier_up,carrier_changes,"
                 "link_down_cp,link_up_cp,link_down_nl,link_up_nl,netlink_overflows,"
                 "rx_bad_fcs_computed,rx_bad_fcs_auxdata,rx_truncated,rx_len_errors,"
                 "host_rx_missed,host_rx_dropped,host_rx_fifo,host_rx_errors,"
                 "host_rx_crc,txok_minus_returns_signed,qdisc_drops_real");
    for (int s = 0; s < num_slaves; s++)
        fprintf(csv, ",slave%d_p0_crc,slave%d_p1_crc,slave%d_p2_crc,slave%d_p3_crc"
                     ",slave%d_p0_lost,slave%d_p1_lost,slave%d_p2_lost,slave%d_p3_lost"
                     ",slave%d_p0_rxerr,slave%d_p1_rxerr,slave%d_p2_rxerr,slave%d_p3_rxerr"
                     ",slave%d_p0_fwderr,slave%d_p1_fwderr,slave%d_p2_fwderr,slave%d_p3_fwderr"
                     ",slave%d_puerr,slave%d_pdierr"
                     ",slave%d_p0_extrx,slave%d_p1_extrx,slave%d_p2_extrx,slave%d_p3_extrx"
                     ",slave%d_p0_rxcode,slave%d_p1_rxcode,slave%d_p2_rxcode,slave%d_p3_rxcode",
                s, s, s, s, s, s, s, s,
                s, s, s, s, s, s, s, s, s, s,
                s, s, s, s, s, s, s, s);
    fprintf(csv, "\n");
    fflush(csv);
}

