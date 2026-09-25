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
#include "nic.h"
#include <getopt.h>
#include <signal.h>

static volatile sig_atomic_t g_run = 1;
static void on_sig(int s) { (void)s; g_run = 0; }

static void usage(const char *p)
{
    printf("Usage: %s -i <iface> -s <chain> --op <positions> [options]\n"
           "  -i <iface>        interface (required)\n"
           "  -s <n>            total slaves in the chain, all monitored\n"
           "  --op <list>       positions to drive to OP, e.g. --op 5,6\n"
           "                    (always explicit; never inferred)\n"
           "  -r <hz>           cycle rate, default 500 (2 ms, as TwinCAT)\n"
           "  -d <sec>          duration, default 3600\n"
           "  -F <dir>          fault capture: PHY probe on a lost link\n"
           "  --observe         reserved. TwinCAT's recovery is not implemented\n"
           "                    yet, so watching is already the only behaviour\n"
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

/* Compare one slave's diagnostic block against the previous cycle and report
 * every counter that moved. Returns 1 if a LOST LINK was among them, which is
 * the event this whole tool exists to catch. */
static int diff_diag(int s, const uint8_t *d, uint64_t t, uint64_t t0)
{
    if (!have_prev[s]) { memcpy(prev_diag[s], d, ESC_DIAG_LEN);
                         have_prev[s] = 1; return 0; }
    const uint8_t *p = prev_diag[s];
    int link_lost = 0;

    for (int port = 0; port < 4; port++) {
        uint8_t c, q;
        c = d[port * 2];      q = p[port * 2];       /* 0x0300+2y invalid    */
        if (c != q) { unsigned dl = (uint8_t)(c - q); tot_invalid[s][port] += dl;
            faultcap_esc_event(t, s, port, "invalid", dl);
            printf("  [%8.3f] slave %2d port %d  invalid frame +%u (now %u)\n",
                   (double)(t - t0) / 1e9, s, port, dl, c); }
        c = d[port * 2 + 1];  q = p[port * 2 + 1];   /* 0x0301+2y RX error   */
        if (c != q) { unsigned dl = (uint8_t)(c - q); tot_rxerr[s][port] += dl;
            faultcap_esc_event(t, s, port, "rxerr", dl);
            printf("  [%8.3f] slave %2d port %d  RX error     +%u (now %u)\n",
                   (double)(t - t0) / 1e9, s, port, dl, c); }
        c = d[8 + port];      q = p[8 + port];       /* 0x0308+y forwarded   */
        if (c != q) { unsigned dl = (uint8_t)(c - q); tot_fwd[s][port] += dl;
            faultcap_esc_event(t, s, port, "fwderr", dl);
            printf("  [%8.3f] slave %2d port %d  forwarded    +%u (now %u)\n",
                   (double)(t - t0) / 1e9, s, port, dl, c); }
        c = d[16 + port];     q = p[16 + port];      /* 0x0310+y lost link   */
        if (c != q) { unsigned dl = (uint8_t)(c - q); tot_lost[s][port] += dl;
            lost_events += dl; link_lost = 1;
            faultcap_esc_event(t, s, port, "lostlink", dl);
            printf("  [%8.3f] slave %2d port %d  *** LOST LINK +%u (now %u) ***\n",
                   (double)(t - t0) / 1e9, s, port, dl, c); }
    }
    memcpy(prev_diag[s], d, ESC_DIAG_LEN);
    return link_lost;
}

int main(int argc, char **argv)
{
    const char *iface = NULL, *faultdir = NULL;
    int chain = 0, rate = 500, dur = 3600, observe = 0, verbose = 0, tmo = 50;
    int op_pos[OP_MAX_SLAVES], n_op = 0;

    static struct option lo[] = {
        {"op",      required_argument, 0, 1},
        {"observe", no_argument,       0, 2},
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
    for (int i = 0; i < n_op; i++) {
        m.sl[i].position = op_pos[i];
        m.sl[i].station  = (uint16_t)(1001 + i);
        m.sl[i].log_addr = 0x01000000u + (uint32_t)(11 * i);
        m.sl[i].log_len  = 11;             /* as TwinCAT mapped these drives */
        m.sl[i].mbx_bit  = (uint8_t)i;
    }

    printf("EtherCAT OP driver\n");
    printf("Interface:  %s\n", iface);
    printf("Chain:      %d slave(s), all monitored every cycle\n", chain);
    printf("Driven OP:  ");
    for (int i = 0; i < n_op; i++)
        printf("%d%s", op_pos[i], i + 1 < n_op ? "," : "");
    printf("   (station %d..%d)\n", 1001, 1000 + n_op);
    printf("Cycle:      %d Hz (%.2f ms)\n", rate, 1000.0 / rate);
    printf("Duration:   %d s\n", dur);
    printf("On a drop:  probe FLDS/RECR, log it, keep watching\n");
    printf("            TwinCAT's recovery (close port, wait 2 s, reopen,\n");
    printf("            re-init) is NOT implemented in this build%s\n",
           observe ? " — and --observe is the only behaviour there is"
                   : ", so --observe is currently the only behaviour");
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
        printf("Bringing position %d to OP...\n", m.sl[i].position);
        if (op_bring_up(&m, &m.sl[i]) != 0) {
            fprintf(stderr, "  FAILED — leaving the bus in INIT\n");
            op_shutdown(&m); esc_close(&m.ctx); return 1;
        }
        printf("  position %d is in OP\n", m.sl[i].position);
    }

    printf("\nCyclic exchange running. Ctrl-C to stop.\n\n");

    OpCycle cyc; memset(&cyc, 0, sizeof cyc);
    cyc.pd_len = (uint16_t)(11 * n_op);     /* controlword stays 0: disabled */

    PaceState pace; uint64_t t0 = now_ns();
    pace_init(&pace, rate, t0);
    uint64_t end = t0 + (uint64_t)dur * 1000000000ULL;
    uint64_t cycles = 0, timeouts = 0, wkc_bad = 0;
    uint16_t wkc_expected = 0xFFFF;

    while (g_run && now_ns() < end) {
        uint64_t now = now_ns();
        uint64_t dl  = pace_deadline(&pace, now);
        if (dl > now) sleep_until_ns(dl);

        if (op_cycle(&m, &cyc, m.sl[0].log_addr) != 0) {
            timeouts++;
        } else {
            cycles++;
            if (wkc_expected == 0xFFFF && cycles > 10) {
                wkc_expected = cyc.lrw_wkc;         /* learn it, don't assume */
                printf("  steady-state process-data WKC = %u\n", wkc_expected);
            } else if (wkc_expected != 0xFFFF && cyc.lrw_wkc != wkc_expected) {
                wkc_bad++;
                if (wkc_bad < 20)
                    printf("  [%8.3f] process-data WKC %u (expected %u) — a slave "
                           "stopped answering\n", (double)(now_ns() - t0) / 1e9,
                           cyc.lrw_wkc, wkc_expected);
            }
            int dropped = 0;
            for (int s = 0; s < chain; s++)
                if (cyc.diag_wkc[s] >= 1)
                    dropped |= diff_diag(s, cyc.diag[s], now_ns(), t0);

            if (dropped && faultdir) {
                faultcap_flush();
                int np = faultcap_probe(iface, now_ns() - t0);
                printf("  -> probed %d position(s) for FLDS/RECR\n", np);
            }
            /* TwinCAT's recovery — close the port, wait for the link, wait a
             * further second, reopen, re-init — is not reproduced here. It
             * cannot have caused the FIRST drop, which is the one we are
             * hunting, and driving it badly would provoke drops of our own.
             * The banner says so; --observe is reserved for when it exists. */
            (void)observe;
        }
        pace_on_sent(&pace, now_ns());
    }

    printf("\nStopping. Returning configured slaves to INIT...\n");
    op_shutdown(&m);

    double secs = (double)(now_ns() - t0) / 1e9;
    printf("\n── Summary ────────────────────────────────────────────────\n");
    printf("  Elapsed:        %.1f s\n", secs);
    printf("  Cycles:         %lu  (%.0f Hz achieved)\n", cycles, cycles / secs);
    printf("  Frame timeouts: %lu\n", timeouts);
    printf("  WKC mismatches: %lu\n", wkc_bad);
    printf("  Cycle interval: mean %.1f us  min %.1f  max %.1f  late %lu  missed %lu\n",
           pace.count ? (double)pace.sum_ns / pace.count / 1000.0 : 0.0,
           pace.min_ns / 1000.0, pace.max_ns / 1000.0, pace.late, pace.missed);
    printf("  *** LOST LINK events: %lu ***\n", lost_events);
    for (int s = 0; s < chain; s++)
        for (int p = 0; p < 4; p++)
            if (tot_invalid[s][p] || tot_rxerr[s][p] || tot_fwd[s][p] || tot_lost[s][p])
                printf("    slave %2d port %d: invalid %lu  rxerr %lu  fwderr %lu  "
                       "lostlink %lu\n", s, p, tot_invalid[s][p], tot_rxerr[s][p],
                       tot_fwd[s][p], tot_lost[s][p]);
    if (!lost_events)
        printf("    no counter moved on any slave\n");

    if (faultdir) faultcap_close();
    esc_close(&m.ctx);
    return 0;
}
