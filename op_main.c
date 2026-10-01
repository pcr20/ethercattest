/* ecat_op — drive selected slaves to OP and watch for link drops.
 *
 * WRITES TO THE SLAVES. See ecat_master.h for what and why.
 *
 * The experiment: the TwinCAT capture of 2026-09-23 shows two M400+EVE-NETs
 * dropping the link between them about every 12 s while in OP, with every
 * error counter at zero. Our rig has never dropped a link — and has never put
 * a drive in OP. This runs the same conditions with our instrumentation
 * watching, so that when a link drops we capture FLDS and learn which Fast
 * Link Drop criterion fired (§6.2 of FINDINGS: the EVE-NET is the only device
 * in the chain with FLD armed, on RX error count and signal/energy loss).
 *
 * Every slave in the chain is monitored whatever its state — ESC error
 * counters work in INIT — so the nine non-EVE-NET slaves stay instrumented
 * while only the --op positions are configured. */
#include "ecat_master.h"
#include "faultcap.h"
#include "pace.h"
#include "acyclic.h"
#include "recovery.h"
#include "atrisk.h"
#include "logbuf.h"
#include "nic.h"
#include "everest_pdo.h"
#include <getopt.h>
#include <signal.h>

static volatile sig_atomic_t g_run = 1;
static void on_sig(int s) { (void)s; g_run = 0; }

static void usage(const char *p)
{
    printf("Usage: %s -i <iface> -s <chain> --op <positions> [options]\n"
           "  -i <iface>        interface (required)\n"
           "  -s <n>            total slaves in the chain; every one has its\n"
           "                    0x0300-0x0327 block polled ~10 times a second\n"
           "  --op <list>       positions to drive to OP, e.g. --op 5,6\n"
           "                    (always explicit; never inferred)\n"
           "  -r <hz>           cycle rate, default 1000 (1 ms, as TwinCAT)\n"
           "  --random <ms>     jitter the acyclic timers by +/- this many\n"
           "                    cycles, redrawn on each send. TwinCAT's own\n"
           "                    spread is about +/-5 ms. Default 0 = exact.\n"
           "  --no-clear        do not send BWR 0x0300. TwinCAT clears the\n"
           "                    error counters 9.6 times a second; suppress it\n"
           "                    if those counts are the evidence you want.\n"
           "  --burst <n>       pad every cycle to n frames back to back,\n"
           "                    to test whether burst length drives the fault\n"
           "  -d <sec>          duration, default 3600\n"
           "  -F <dir>          fault capture: PHY probe on a lost link\n"
           "  --observe         do NOT recover from a link drop — probe and log\n"
           "                    it, then leave the chain severed. At most one\n"
           "                    drop per run is then measurable. This was the\n"
           "                    only behaviour before recovery existed.\n"
           "  --link-timeout <s> give up waiting for a dropped link to come back\n"
           "                    after this long, default 30\n"
           "  --settle-timeout <s> after reopening the port, give up if the chain\n"
           "                    does not answer within this long, default 10\n"
           "  -t <ms>           transaction timeout, default 50\n"
           "  -v                verbose\n"
           "\n"
           "  THIS TOOL WRITES TO THE SLAVES. Nothing is persistent: the SII\n"
           "  is never written, and every configured slave is returned to INIT\n"
           "  on exit.\n", p);
}

/* Per-slave copy of the diagnostic block, for delta detection. */
static uint8_t prev_diag[OP_MAX_SLAVES][ESC_DIAG_LEN];
static int     have_prev[OP_MAX_SLAVES];
static uint64_t tot_invalid[OP_MAX_SLAVES][4], tot_rxerr[OP_MAX_SLAVES][4];
static uint64_t tot_fwd[OP_MAX_SLAVES][4],     tot_lost[OP_MAX_SLAVES][4];
static uint64_t lost_events;

/* prev_diag[s][16..17] holds the last lost-link counters for slave s; those
 * are NOT cleared by BWR 0x0300, so they are differenced. The error counters
 * at 0x0300 ARE cleared every ~104 ms, so those are accumulated instead --
 * see the cycle loop. */

int main(int argc, char **argv)
{
    /* FIRST statement: setvbuf is only honoured before anything is written to
     * the stream (logbuf.h). Anything printed above this line silently pins
     * stdout to full buffering, and a redirected log then stays empty until
     * the run ends. */
    log_line_buffered(stdout);   /* its return cannot detect lateness */

    const char *iface = NULL, *faultdir = NULL;
    int chain = 0, rate = 1000, dur = 3600, observe = 0, verbose = 0, tmo = 50;
    int jitter = 0, no_clear = 0;   /* --random N, --no-clear */
    int link_timeout = 30;          /* --link-timeout, seconds */
    int settle_timeout = 10;        /* --settle-timeout, seconds */
    int min_burst = 0;   /* --burst: pad every cycle to this many frames */
    int op_pos[OP_MAX_SLAVES], n_op = 0;

    static struct option lo[] = {
        {"op",      required_argument, 0, 1},
        {"observe", no_argument,       0, 2},
        {"burst",   required_argument, 0, 3},
        {"random",  required_argument, 0, 4},
        {"no-clear",no_argument,       0, 5},
        {"link-timeout", required_argument, 0, 6},
        {"settle-timeout", required_argument, 0, 7},
        {"help",    no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "i:s:r:d:F:t:vh", lo, NULL)) != -1) {
        switch (c) {
        case 'i': iface = optarg; break;
        case 's': chain = atoi(optarg); break;
        case 'r': rate = atoi(optarg); break;
        case 'd': dur = atoi(optarg); break;
        case 'F': faultdir = optarg; break;
        case 't': tmo = atoi(optarg); break;
        case 'v': verbose = 1; break;
        case 1: {
            char *tok = strtok(optarg, ",");
            while (tok && n_op < OP_MAX_SLAVES) {
                op_pos[n_op++] = atoi(tok); tok = strtok(NULL, ",");
            }
            break; }
        case 2: observe = 1; break;
        case 3: min_burst = atoi(optarg); break;
        case 4: jitter = atoi(optarg); break;
        case 5: no_clear = 1; break;
        case 6: link_timeout = atoi(optarg); break;
        case 7: settle_timeout = atoi(optarg); break;
        default: usage(argv[0]); return c == 'h' ? 0 : 1;
        }
    }
    if (!iface || chain <= 0 || n_op == 0) { usage(argv[0]); return 1; }
    if (chain > OP_MAX_SLAVES) {
        fprintf(stderr, "Error: -s %d exceeds the %d-slave limit\n",
                chain, OP_MAX_SLAVES); return 1; }
    for (int i = 0; i < n_op; i++)
        if (op_pos[i] < 0 || op_pos[i] >= chain) {
            fprintf(stderr, "Error: --op position %d is outside a %d-slave "
                    "chain\n", op_pos[i], chain); return 1; }

    OpMaster m; memset(&m, 0, sizeof m);
    m.chain_len = chain; m.n_op = n_op; m.verbose = verbose;
    m.clear_counters = !no_clear;
    for (int i = 0; i < n_op; i++) {
        m.sl[i].position = op_pos[i];
        m.sl[i].station  = (uint16_t)(1001 + i);
        m.sl[i].log_addr = 0x01000000u + (uint32_t)(EVEREST_PD_BYTES * i);
        m.sl[i].log_len  = EVEREST_PD_BYTES;   /* from the captured maps */
        m.sl[i].mbx_bit  = (uint8_t)i;
    }

    printf("EtherCAT OP driver\n");
    printf("Interface:  %s\n", iface);
    printf("Chain:      %d slave(s), counters polled ~10/s (as TwinCAT does)\n",
           chain);
    printf("Driven OP:  ");
    for (int i = 0; i < n_op; i++)
        printf("%d%s", op_pos[i], i + 1 < n_op ? "," : "");
    printf("   (station %d..%d)\n", 1001, 1000 + n_op);
    printf("Cycle:      %d Hz (%.3f ms), one frame per cycle as TwinCAT\n",
           rate, 1000.0 / rate);
    printf("Acyclic:    read+clear 0x0300 every 104 cycles%s;"
           " lost-link 0x0310 every 203\n", no_clear ? " (clear suppressed)" : "");
    if (min_burst) printf("Burst:      padded to %d frames per cycle\n", min_burst);
    printf("Duration:   %d s\n", dur);
    printf("On a drop:  probe FLDS/RECR, log it, then ");
    if (observe)
        printf("WATCH ONLY (--observe).\n"
               "            The chain stays severed, so expect at most one\n"
               "            measurable drop for the rest of the run.\n");
    else
        printf("recover as TwinCAT does:\n"
               "            force the port closed (0x0101), wait for the physical\n"
               "            link to return (TwinCAT saw 2.00 s; we wait, we do not\n"
               "            assume), hold 1.000 s, reopen, wait for the chain to\n"
               "            answer, then re-init to OP.\n"
               "            Give up after %d s waiting for the link, %d s for\n"
               "            the chain.\n", link_timeout, settle_timeout);
    op_print_write_warning(&m);

    if (esc_open(&m.ctx, iface, 0, tmo) != 0) return 1;
    if (faultdir && faultcap_open(iface, chain, faultdir) != 0) return 1;

    signal(SIGINT, on_sig); signal(SIGTERM, on_sig);

    printf("Checking identity of the --op positions...\n");
    if (op_check_identity(&m) != 0) { esc_close(&m.ctx); return 1; }

    printf("Bus reset...\n");
    if (op_bus_reset(&m, chain) != 0) {
        fprintf(stderr, "  bus reset failed\n"); esc_close(&m.ctx); return 1; }

    for (int i = 0; i < n_op; i++) {
        printf("Bringing position %d to SAFEOP...\n", m.sl[i].position);
        if (op_bring_up(&m, &m.sl[i]) != 0) {
            fprintf(stderr, "  FAILED — leaving the bus in INIT\n");
            op_shutdown(&m); esc_close(&m.ctx); return 1;
        }
        printf("  position %d is in SAFEOP\n", m.sl[i].position);
    }

    /* OP needs process data already flowing, so this cycles throughout. */
    printf("Priming process data and requesting OP...\n");
    if (op_go_operational(&m, m.sl[0].log_addr,
                          (uint16_t)(EVEREST_PD_BYTES * n_op), 5000) != 0) {
        fprintf(stderr, "  FAILED — leaving the bus in INIT\n");
        op_shutdown(&m); esc_close(&m.ctx); return 1;
    }
    for (int i = 0; i < n_op; i++)
        printf("  position %d is in OP\n", m.sl[i].position);

    /* Snapshot 0x0101 now, while every slave is in OP and answering. Reading
     * it during a recovery reads it mid-disruption: run 6 of LINKDROPS.md got
     * 0x00 and run 7 got 0xFF from the same slave, neither matching the 0xF4
     * that run 8 saw here, and in run 7 the resulting "close" wrote a byte
     * the port already had and did nothing at all. */
    uint8_t loop_base[OP_MAX_SLAVES];
    int     loop_have[OP_MAX_SLAVES];
    memset(loop_base, 0, sizeof loop_base);
    memset(loop_have, 0, sizeof loop_have);
    for (int i = 0; i < chain; i++) {
        uint8_t v = 0;
        if (op_aprd(&m, i, REG_DL_CTRL_P, &v, 1) >= 1) {
            loop_base[i] = v; loop_have[i] = 1;
        }
    }
    printf("Port loop control 0x0101 as found:");
    for (int i = 0; i < chain; i++)
        if (loop_have[i]) printf("  %d:0x%02X", i, loop_base[i]);
        else              printf("  %d:??", i);
    printf("\n");

    printf("\nCyclic exchange running. Ctrl-C to stop.\n\n");

    /* ── The cycle, shaped like TwinCAT's ─────────────────────────────────
     * One cyclic frame every 1 ms, plus acyclic jobs on their own timers,
     * all written to the socket consecutively. Periods measured from the
     * capture in healthy windows: read+clear 103.8 ms, lost-link 202.9 ms.
     * The read and the clear are a locked pair in the same cycle; the
     * lost-link poll runs off an independent timer, so it gets its own phase
     * and, under --random, its own PRNG. */
    uint8_t pd[128]; memset(pd, 0, sizeof pd);     /* controlword 0 = disabled */
    uint16_t pd_len = (uint16_t)(EVEREST_PD_BYTES * n_op);
    int n_mbx_bytes = (n_op + 7) / 8;

    uint16_t adp_op[OP_MAX_SLAVES], adp_chain[OP_MAX_SLAVES];
    for (int i = 0; i < n_op; i++) adp_op[i] = m.sl[i].station;
    for (int i = 0; i < chain; i++) adp_chain[i] = (uint16_t)(-(int16_t)i);

    uint64_t seed = (uint64_t)now_ns();
    AcyJob job_rdclr, job_lost;
    acy_init(&job_rdclr, 104,  0, jitter, seed ^ 0xA5A5A5A5A5A5A5A5ULL);
    acy_init(&job_lost,  203, 50, jitter, seed ^ 0x5A5A5A5A5A5A5A5AULL);
    if (jitter)
        printf("Acyclic timers: +/-%d cycles, seeds 0x%016lX / 0x%016lX\n",
               jitter, job_rdclr.rng, job_lost.rng);

    PaceState pace; uint64_t t0 = now_ns();
    pace_init(&pace, rate, t0);
    faultcap_set_epoch(t0);          /* events.csv on the same clock as this log */

    AtRisk risk; ar_init(&risk, t0);
    uint64_t drop_events = 0;      /* physical drops, not counter increments */

    RecCtx rec; rec_init(&rec, 1000000000ULL,
                         (uint64_t)link_timeout * 1000000000ULL,
                         (uint64_t)settle_timeout * 1000000000ULL);
    uint64_t next_beat = t0 + 60000000000ULL;   /* heartbeat every 60 s */
    uint64_t end = t0 + (uint64_t)dur * 1000000000ULL;
    uint64_t cycles = 0, short_burst = 0, wkc_bad = 0;
    uint64_t burst_hist[OP_BURST_MAX + 1]; memset(burst_hist, 0, sizeof burst_hist);
    uint16_t wkc_expected = 0xFFFF;
    OpResult res[64];

    while (g_run && now_ns() < end) {
        uint64_t now = now_ns();
        uint64_t dl  = pace_deadline(&pace, now);
        if (dl > now) sleep_until_ns(dl);

        OpBurst burst; memset(&burst, 0, sizeof burst);
        uint8_t f[1600]; int fl;

        fl = op_build_cyc_frame(f, sizeof f, m.ctx.src_mac, 0,
                                0x09000000u, m.sl[0].log_addr,
                                pd, pd_len, n_mbx_bytes);
        if (fl > 0) op_burst_add(&burst, f, fl, 0);

        int rd_due = acy_due(&job_rdclr, (long)cycles);
        int ll_due = acy_due(&job_lost,  (long)cycles);

        if (rd_due) {
            /* Read before the clear, in that order, so the counts are banked
             * before they are wiped. */
            fl = op_build_multi(f, sizeof f, m.ctx.src_mac, ECAT_CMD_FPRD_M,
                                10, adp_op, REG_ERR_CNT, 8, n_op);
            if (fl > 0) op_burst_add(&burst, f, fl, 10);
            if (!no_clear) {
                uint8_t zero[8] = {0};
                fl = op_build_frame(f, sizeof f, m.ctx.src_mac, ECAT_CMD_BWR_M,
                                    18, 0, REG_ERR_CNT, zero, 8);
                if (fl > 0) op_burst_add(&burst, f, fl, 18);
            }
            acy_advance(&job_rdclr, (long)cycles);
        }
        if (ll_due) {
            fl = op_build_multi(f, sizeof f, m.ctx.src_mac, ECAT_CMD_APRD,
                                20, adp_chain, 0x0310, 2, chain);
            if (fl > 0) { op_burst_add(&burst, f, fl, 20);
                          op_burst_mark_diag(&burst, 20, chain); }
            acy_advance(&job_lost, (long)cycles);
        }
        while (burst.n < min_burst && burst.n < OP_BURST_MAX) {
            uint8_t pb = (uint8_t)(40 + 3 * burst.n);
            fl = op_build_cyc_frame(f, sizeof f, m.ctx.src_mac, pb,
                                    0x09000000u, m.sl[0].log_addr,
                                    pd, pd_len, n_mbx_bytes);
            if (fl <= 0) break;
            op_burst_add(&burst, f, fl, pb);
        }

        int n = op_burst_collect(&m, &burst, res, 64);
        cycles++;
        burst_hist[burst.n <= OP_BURST_MAX ? burst.n : OP_BURST_MAX]++;
        if (n < 0) { short_burst++; pace_on_sent(&pace, now_ns()); continue; }

        int dropped = 0, drop_slave = -1, drop_port = -1, chain_ok = 0;
        uint64_t tn = now_ns();
        for (int i = 0; i < n; i++) {
            OpResult *o = &res[i];
            if (o->cmd == ECAT_CMD_LRW_M) {
                if (wkc_expected == 0xFFFF && cycles > 20 && o->wkc) {
                    wkc_expected = o->wkc;
                    printf("  steady-state process-data WKC = %u\n", wkc_expected);
                } else if (wkc_expected != 0xFFFF && o->wkc != wkc_expected) {
                    if (++wkc_bad < 20)
                        printf("  [%8.3f] process-data WKC %u (expected %u)\n",
                               (double)(tn - t0) / 1e9, o->wkc, wkc_expected);
                } else if (wkc_expected != 0xFFFF) {
                    chain_ok = 1;     /* every slave answered: at risk        */
                }
            } else if (o->cmd == ECAT_CMD_FPRD_M && o->ado == REG_ERR_CNT) {
                /* The counters are zeroed every ~104 ms, so a reading is the
                 * count SINCE THE LAST CLEAR, not a running total. Add what
                 * is read rather than differencing against the last value. */
                int s = -1;
                for (int k = 0; k < n_op; k++)
                    if ((uint8_t)(10 + k) == o->idx) s = m.sl[k].position;
                if (s >= 0 && o->wkc >= 1)
                    for (int port = 0; port < 4 && port * 2 + 1 < o->len; port++) {
                        if (o->data[port * 2]) {
                            tot_invalid[s][port] += o->data[port * 2];
                            faultcap_esc_event(tn, s, port, "invalid", o->data[port * 2]);
                            printf("  [%8.3f] slave %2d port %d  invalid frame +%u\n",
                                   (double)(tn - t0) / 1e9, s, port, o->data[port * 2]);
                        }
                        if (o->data[port * 2 + 1]) {
                            tot_rxerr[s][port] += o->data[port * 2 + 1];
                            faultcap_esc_event(tn, s, port, "rxerr", o->data[port * 2 + 1]);
                            printf("  [%8.3f] slave %2d port %d  RX error     +%u\n",
                                   (double)(tn - t0) / 1e9, s, port, o->data[port * 2 + 1]);
                        }
                    }
            } else if (o->cmd == ECAT_CMD_APRD && o->ado == 0x0310) {
                int s = op_burst_slave_of(&burst, o->idx);
                if (s >= 0 && o->wkc >= 1 && o->len >= 2) {
                    /* Lost-link is NOT cleared by BWR 0x0300, so this is a
                     * running total and must be differenced. */
                    for (int port = 0; port < 2; port++) {
                        uint8_t c = o->data[port];
                        if (have_prev[s] && c != prev_diag[s][16 + port]) {
                            unsigned dl2 = (uint8_t)(c - prev_diag[s][16 + port]);
                            tot_lost[s][port] += dl2; lost_events += dl2; dropped = 1;
                            drop_slave = s; drop_port = port;
                            faultcap_esc_event(tn, s, port, "lostlink", dl2);
                            printf("  [%8.3f] slave %2d port %d  *** LOST LINK +%u "
                                   "(now %u) ***\n", (double)(tn - t0) / 1e9,
                                   s, port, dl2, c);
                        }
                        prev_diag[s][16 + port] = c;
                    }
                    have_prev[s] = 1;
                }
            }
        }
        ar_cycle(&risk, tn, chain_ok);
        if (dropped) drop_events++;

        if (dropped && faultdir) {
            faultcap_flush();
            int np = faultcap_probe(iface, now_ns());
            printf("  -> probed %d position(s) for FLDS/RECR\n", np);
        }

        /* ── Recovery, as TwinCAT does it (TWINCAT.md §6.6) ───────────────
         * Sequencing lives in recovery.h and is unit-tested; this performs
         * the actions it asks for. Without it the first drop ends the
         * experiment: the 2026-09-30 run spent 89% of its cycles on a severed
         * chain and could not yield a rate. */
        if (!observe && dropped)
            if (rec_on_drop(&rec, tn, drop_slave, drop_port))
                printf("  -> recovering slave %d port %d\n", drop_slave, drop_port);

        if (!observe && rec_busy(&rec)) {
            int link_up = 0, chain_back = 0;
            if (rec_needs_link(&rec)) {
                uint8_t dl[2] = {0, 0};
                if (op_aprd(&m, rec.slave, 0x0110, dl, 2) >= 1) {
                    uint16_t v = (uint16_t)(dl[0] | (dl[1] << 8));
                    link_up = rec_link_up(v, rec.port);
                }
            }
            if (rec_needs_chain(&rec)) {
                /* The furthest slave in the chain answers only once every hop
                 * between here and it is forwarding again. */
                uint8_t t = 0;
                chain_back = (op_aprd(&m, chain - 1, REG_TYPE, &t, 1) >= 1);
            }
            switch (rec_step(&rec, now_ns(), link_up, chain_back)) {
            case REC_ACT_CLOSE_PORT: {
                uint8_t base = loop_have[rec.slave] ? loop_base[rec.slave] : 0xF4;
                uint8_t shut = rec_loop_closed(base, rec.port);
                int w = op_apwr(&m, rec.slave, REG_DL_CTRL_P, &shut, 1);
                printf("  [%8.3f] port closed: 0x0101 0x%02X -> 0x%02X (wkc %d)%s\n",
                       (double)(now_ns() - t0) / 1e9, base, shut, w,
                       loop_have[rec.slave] ? "" : "  [base not read at start]");
                break; }
            case REC_ACT_OPEN_PORT: {
                uint8_t base = loop_have[rec.slave] ? loop_base[rec.slave] : 0xF4;
                uint8_t open_b = rec_loop_auto(base, rec.port);
                int w = op_apwr(&m, rec.slave, REG_DL_CTRL_P, &open_b, 1);
                printf("  [%8.3f] link back after %.3f s, debounce done, "
                       "port reopened 0x%02X (wkc %d)\n",
                       (double)(now_ns() - t0) / 1e9,
                       (double)(rec.t_link_back - rec.t_drop) / 1e9, open_b, w);
                break; }
            case REC_ACT_REINIT: {
                int bad = 0;
                for (int k = 0; k < n_op; k++) {
                    if (m.sl[k].position <= rec.slave) continue;
                    if (op_bring_up(&m, &m.sl[k]) != 0) {
                        printf("  [%8.3f] re-init of position %d FAILED "
                               "(AL code 0x%04X %s)\n",
                               (double)(now_ns() - t0) / 1e9, m.sl[k].position,
                               m.sl[k].al_code, op_al_code_name(m.sl[k].al_code));
                        bad = 1;
                    }
                }
                if (!bad && op_go_operational(&m, m.sl[0].log_addr, pd_len,
                                              5000) != 0)
                    bad = 1;
                printf("  [%8.3f] *** RECOVERED in %.3f s (chain answered "
                       "%.3f s after reopen)%s ***\n",
                       (double)(now_ns() - t0) / 1e9,
                       (double)rec.outage_ns_max / 1e9,
                       (double)rec.settle_ns_max / 1e9,
                       bad ? " — but the chain is NOT back in OP" : "");
                /* The counters just re-read across a re-init would look like
                 * fresh deltas; drop the baselines so the next poll re-seeds. */
                for (int k = 0; k < chain; k++) have_prev[k] = 0;
                break; }
            case REC_ACT_GIVE_UP:
                printf("  [%8.3f] *** link on slave %d port %d did not return "
                       "in %d s — giving up, chain stays severed ***\n",
                       (double)(now_ns() - t0) / 1e9, rec.slave, rec.port,
                       link_timeout);
                break;
            case REC_ACT_GIVE_UP_SETTLE:
                printf("  [%8.3f] *** port reopened but the chain never "
                       "answered within %d s — giving up without re-init ***\n",
                       (double)(now_ns() - t0) / 1e9, settle_timeout);
                break;
            case REC_ACT_NONE: break;
            }
        }

        /* Heartbeat, so a killed run still leaves a record of how far it got
         * and how many links dropped — which the 2026-09-29 run did not. */
        if (now_ns() >= next_beat) {
            next_beat += 60000000000ULL;
            printf("  [%8.3f] .. %lu cycles, %lu lost-link event(s), "
                   "%lu recovered, %lu abandoned\n",
                   (double)(now_ns() - t0) / 1e9, cycles, lost_events,
                   rec.recoveries, rec.timeouts);
        }
        pace_on_sent(&pace, now_ns());
    }

    printf("\nStopping. Returning configured slaves to INIT...\n");
    op_shutdown(&m);

    double secs = (double)(now_ns() - t0) / 1e9;
    printf("\n── Summary ────────────────────────────────────────────────\n");
    printf("  Elapsed:        %.1f s\n", secs);
    printf("  Cycles:         %lu  (%.0f Hz achieved)\n", cycles, cycles / secs);
    printf("  Cycles with a frame unreturned: %lu\n", short_burst);
    printf("  Acyclic sends: read/clear %lu, lost-link %lu\n",
           job_rdclr.fires, job_lost.fires);
    printf("  Burst size (frames per cycle):");
    for (int i = 1; i <= OP_BURST_MAX; i++)
        if (burst_hist[i]) printf("  %d:%lu", i, burst_hist[i]);
    printf("\n");
    printf("  WKC mismatches: %lu\n", wkc_bad);
    printf("  Cycle interval: mean %.1f us  min %.1f  max %.1f  late %lu  missed %lu\n",
           pace.count ? (double)pace.sum_ns / pace.count / 1000.0 : 0.0,
           pace.min_ns / 1000.0, pace.max_ns / 1000.0, pace.late, pace.missed);
    printf("  *** LOST LINK events: %lu ***\n", lost_events);
    if (observe) {
        printf("  Recovery:       disabled (--observe)\n");
    } else {
        printf("  Recoveries:     %lu completed, %lu abandoned\n",
               rec.recoveries, rec.timeouts);
        if (rec.recoveries) {
            printf("  Outage:         mean %.3f s, max %.3f s  "
                   "(TwinCAT's was 3.00 s every time)\n",
                   (double)rec.outage_ns_total / rec.recoveries / 1e9,
                   (double)rec.outage_ns_max / 1e9);
            printf("  Reopen->answer: mean %.3f s, max %.3f s\n",
                   (double)rec.settle_ns_total / rec.recoveries / 1e9,
                   (double)rec.settle_ns_max / 1e9);
        }
    }
    /* At-risk time is cycles with every slave answering — NOT elapsed minus
     * the recovery outage, which credited 306 s of severed chain in run 6 of
     * LINKDROPS.md and understated that rate by ~140x. */
    printf("  At-risk time:   %.3f s of %.1f s elapsed (%.1f%% of cycles)\n",
           ar_seconds(&risk), secs,
           cycles ? 100.0 * risk.cycles / cycles : 0.0);
    if (risk.ns)
        printf("  Drop rate:      %.4f drops/s over the at-risk time  "
               "(TwinCAT on this hardware: 0.118/s)\n",
               ar_rate(&risk, drop_events));
    else
        printf("  Drop rate:      no at-risk time — the chain never reached "
               "a healthy WKC\n");
    printf("  Physical drops: %lu  (counter increments: %lu)\n",
           drop_events, lost_events);
    for (int s = 0; s < chain; s++)
        for (int p = 0; p < 4; p++)
            if (tot_invalid[s][p] || tot_rxerr[s][p] || tot_fwd[s][p] || tot_lost[s][p])
                printf("    slave %2d port %d: invalid %lu  rxerr %lu  fwderr %lu  "
                       "lostlink %lu\n", s, p, tot_invalid[s][p], tot_rxerr[s][p],
                       tot_fwd[s][p], tot_lost[s][p]);
    {   int any = 0;
        for (int s = 0; s < chain; s++)
            for (int p = 0; p < 4; p++)
                if (tot_invalid[s][p] || tot_rxerr[s][p] || tot_fwd[s][p] ||
                    tot_lost[s][p]) any = 1;
        if (!any) printf("    no counter moved on any slave\n");
    }

    if (faultdir) faultcap_close();
    esc_close(&m.ctx);
    return 0;
}
