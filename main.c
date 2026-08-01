#include "ecat_common.h"
#include "crc.h"
#include "frame.h"
#include "stats.h"
#include "nic.h"
#include "threads.h"

/* ── Signal handler ─────────────────────────────────────────────────────── */
/* Stop TX first; the main thread performs the drain barrier and then clears
 * g_running to stop RX. A second signal forces immediate exit. */
static void sig_handler(int sig) {
    (void)sig;
    if (g_tx_running) g_tx_running = 0;   /* first: stop sending, drain */
    else              g_running    = 0;   /* second: hard stop          */
}

/* ── Main ───────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    const char *iface     = NULL;
    const char *csv_path  = "ber_results.csv";
    int   num_slaves      = 0;
    long  rate_hz         = 0;      /* 0 = saturate */
    long  duration_s      = 0;      /* 0 = until Ctrl-C */

    int opt;
    while ((opt = getopt(argc, argv, "i:s:r:d:o:lv")) != -1) {
        switch (opt) {
        case 'i': iface       = optarg;          break;
        case 's': num_slaves  = atoi(optarg);    break;
        case 'r': rate_hz     = atol(optarg);    break;
        case 'd': duration_s  = atol(optarg);    break;
        case 'o': csv_path    = optarg;          break;
        case 'l': g_loopback  = 1;               break;
        case 'v': g_verbose   = 1;               break;
        default:
            fprintf(stderr,
                "Usage: %s -i <iface> -s <slaves> [-r <hz>] [-d <secs>] "
                "[-o <csv>] [-l] [-v]\n", argv[0]);
            return 1;
        }
    }

    if (!iface) { fprintf(stderr, "Error: -i <iface> required\n"); return 1; }
    if (!g_loopback && num_slaves <= 0) {
        fprintf(stderr, "Error: -s <slaves> required (or -l for loopback)\n");
        return 1;
    }
    if (num_slaves > MAX_SLAVES) {
        fprintf(stderr, "Error: max %d slaves supported\n", MAX_SLAVES);
        return 1;
    }

    g_num_slaves = num_slaves;
    g_stats.brd_wkc_expected = num_slaves;

    /* Open raw socket */
    int sock = socket(AF_PACKET, SOCK_RAW, htons(ETHERTYPE_ECAT));
    if (sock < 0) { perror("socket"); return 1; }

    /* Bind to interface */
    int ifindex = get_ifindex(sock, iface);
    if (ifindex < 0) { perror("get_ifindex"); return 1; }

    struct sockaddr_ll sll;
    memset(&sll, 0, sizeof(sll));
    sll.sll_family   = AF_PACKET;
    sll.sll_protocol = htons(ETHERTYPE_ECAT);
    sll.sll_ifindex  = ifindex;
    if (bind(sock, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
        perror("bind"); return 1;
    }

    /* Get MAC */
    uint8_t src_mac[6];
    if (get_mac(sock, iface, src_mac) < 0) { perror("get_mac"); return 1; }

    printf("EtherCAT BER Tester\n");
    printf("Interface:  %s (index %d)\n", iface, ifindex);
    printf("MAC:        %02x:%02x:%02x:%02x:%02x:%02x\n",
           src_mac[0], src_mac[1], src_mac[2],
           src_mac[3], src_mac[4], src_mac[5]);
    if (g_loopback)
        printf("Mode:       Loopback (no slaves)\n");
    else
        printf("Slaves:     %d\n", num_slaves);
    if (rate_hz > 0)
        printf("Rate:       %ld Hz\n", rate_hz);
    else
        printf("Rate:       saturate\n");
    if (duration_s > 0)
        printf("Duration:   %ld s\n", duration_s);
    else
        printf("Duration:   until Ctrl-C\n");
    printf("CSV output: %s\n\n", csv_path);

    /* The RX thread sets the socket non-blocking and waits with poll(), so it
     * always wakes to re-check g_running and can never hang at shutdown. The
     * TX thread relies on EAGAIN/ENOBUFS backpressure (delivered on the same
     * socket when the ring is full). */

    /* Large socket buffers so a momentary scheduling hiccup on the RX thread
     * never overflows the queue. Combined with the dedicated RX core this is
     * what keeps the drain from ever saturating. Request generously; the
     * kernel caps at net.core.rmem_max (raise it in setup.sh). */
    int rcvbuf = 64 * 1024 * 1024;   /* 64 MB */
    int sndbuf = 8  * 1024 * 1024;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVBUFFORCE, &rcvbuf, sizeof(rcvbuf)) < 0)
        setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    /* Report the buffer we actually got. */
    { int got = 0; socklen_t l = sizeof(got);
      getsockopt(sock, SOL_SOCKET, SO_RCVBUF, &got, &l);
      printf("RX socket buffer: %d KB (requested %d KB)\n",
             got / 1024, rcvbuf / 1024);
      if (got < rcvbuf / 2)
          printf("  NOTE: capped by net.core.rmem_max — raise it "
                 "(setup.sh does this) for headroom.\n");
    }

    /* Prime PACKET_STATISTICS baseline (first read clears any startup drops). */
    poll_kernel_drops(sock);
    atomic_store_explicit(&g_stats.kernel_drops, 0, memory_order_relaxed);

    /* Enable PACKET_AUXDATA so each received frame carries a tpacket_auxdata
     * cmsg (tp_status) — detector 1 for bad-FCS frames. */
    { int one = 1;
      if (setsockopt(sock, SOL_PACKET, PACKET_AUXDATA, &one, sizeof(one)) < 0)
          fprintf(stderr, "warning: PACKET_AUXDATA enable failed: %s\n",
                  strerror(errno));
    }

    /* Detect rx-fcs / rx-all state (set externally by setup.sh). */
    int rxfcs_known = 0, rxall_known = 0;
    int rx_fcs_on = ethtool_feature_on(iface, "rx-fcs", &rxfcs_known);
    int rx_all_on = ethtool_feature_on(iface, "rx-all", &rxall_known);
    printf("RX frame capture: rx-fcs=%s rx-all=%s\n",
           rxfcs_known ? (rx_fcs_on ? "on" : "off") : "unknown",
           rxall_known ? (rx_all_on ? "on" : "off") : "unknown");
    if (rx_all_on)
        printf("  -> corrupt (bad-FCS) frames WILL be delivered and counted.\n");
    else
        printf("  -> corrupt frames are dropped by the NIC (enable rx-all to "
               "receive them: setup.sh does this).\n");
    if (rx_fcs_on)
        printf("  -> frames carry a 4-byte FCS trailer; FCS verified in software.\n");

    /* Initialise CRC32C (detect SSE4.2, build table). */
    crc32c_init();
    printf("Payload check: CRC32C over seq+payload, %s; payload = xorshift64\n",
           g_have_sse42 ? "SSE4.2 accelerated" : "software");

    /* Enable software TX timestamping (no PTP HW clock on RTL8125). */
    g_stats.tx_ts_supported = enable_tx_timestamping(sock);
    printf("TX timestamping: %s\n",
           g_stats.tx_ts_supported
             ? "software (driver-xmit stage; ethtool tx_packets is wire truth)"
             : "unavailable");

    /* On-wire TX baseline (ethtool tx_packets = hardware TxOk). */
    g_stats.tx_wire_base = read_nic_tx_packets(iface);
    if (g_stats.tx_wire_base == 0)
        printf("NOTE: could not read ethtool tx_packets — wire count "
               "may be unavailable on this driver.\n");
    /* TxER (carrier-lost etc.) and qdisc-drop baselines for the boundary split. */
    g_stats.tx_err_base = read_nic_tx_errors(iface);
    { int ok=0; g_stats.qdisc_drop_base = read_sysfs_u64(iface, "statistics/tx_dropped", &ok);
      if (!ok) g_stats.qdisc_drop_base = 0; }

    /* Link-loss baselines (sysfs). All three exist on this kernel. */
    { int ok1=0, ok2=0, ok3=0;
      g_stats.carrier_down_base    = read_sysfs_u64(iface, "carrier_down_count", &ok1);
      g_stats.carrier_up_base      = read_sysfs_u64(iface, "carrier_up_count", &ok2);
      g_stats.carrier_changes_base = read_sysfs_u64(iface, "carrier_changes", &ok3);
      printf("Link monitor: sysfs carrier counters %s; netlink events enabled\n",
             (ok1 && ok2 && ok3) ? "available" : "PARTIAL (some missing)");
    }

    /* Open per-event link log: <csv_path> with _linkevents before extension. */
    char linkev_path[512];
    { const char *dot = strrchr(csv_path, '.');
      if (dot) snprintf(linkev_path, sizeof(linkev_path), "%.*s_linkevents%s",
                        (int)(dot - csv_path), csv_path, dot);
      else     snprintf(linkev_path, sizeof(linkev_path), "%s_linkevents.csv", csv_path);
    }
    g_linkev_csv = fopen(linkev_path, "w");
    if (g_linkev_csv) {
        fprintf(g_linkev_csv, "t_rel_s,direction,down_duration_ms\n");
        fflush(g_linkev_csv);
        printf("Link events: %s\n", linkev_path);
    }

    /* Open CSV */
    FILE *csv = fopen(csv_path, "w");
    if (!csv) { perror("fopen csv"); return 1; }
    write_csv_header(csv, num_slaves);

    /* Signal handlers */
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    /* Initial NIC CRC baseline */

    /* Decide CPU pinning. On the 4-core/8-thread Ryzen 5300U we pin TX, RX and
     * the errqueue reader to separate cores; the supervisor (main) stays off
     * them. */
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    int tx_core   = (ncpu >= 3) ? 1 : -1;
    int rx_core   = (ncpu >= 3) ? 2 : -1;
    int errq_core = (ncpu >= 4) ? 3 : -1;
    if (tx_core >= 0)
        printf("Pinning: TX->core %d, RX->core %d, errq->core %d (of %ld)\n",
               tx_core, rx_core, errq_core, ncpu);
    else
        printf("Pinning: disabled (only %ld CPUs online)\n", ncpu);

    ThreadCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.sock       = sock;
    ctx.ifindex    = ifindex;
    ctx.iface_name = iface;
    memcpy(ctx.src_mac, src_mac, 6);
    ctx.num_slaves = num_slaves;
    ctx.loopback   = g_loopback;
    ctx.rx_fcs_on  = rx_fcs_on;
    ctx.rx_all_on  = rx_all_on;
    ctx.rate_hz    = rate_hz;
    ctx.tx_core    = tx_core;
    ctx.rx_core    = rx_core;
    ctx.errq_core  = errq_core;

    uint64_t start_ns    = now_ns();
    uint64_t last_stat_ns = start_ns;
    int      badfcs_budget = BADFCS_PRINT_CAP;   /* prints left this interval */
    uint64_t badfcs_suppressed = 0;
    g_start_ns = start_ns;   /* for netlink relative timestamps */
    /* Assume link up until netlink seeds real state, so the in-flight cap is
     * active from the start rather than disabled by a 0-initialised flag. */
    atomic_store_explicit(&g_stats.link_state_up, 1, memory_order_relaxed);

    printf("Running... (Ctrl-C to stop)\n");

    /* Spawn RX first so it is draining before TX starts producing. */
    pthread_t rx_tid, tx_tid, errq_tid;
    int have_errq = 0;
    if (pthread_create(&rx_tid, NULL, rx_thread, &ctx) != 0) {
        perror("pthread_create rx"); return 1;
    }
    /* Error-queue reader (only if TX timestamping enabled). */
    if (g_stats.tx_ts_supported) {
        if (pthread_create(&errq_tid, NULL, errq_thread, &ctx) == 0)
            have_errq = 1;
        else
            fprintf(stderr, "warning: errqueue thread failed; "
                            "TX driver-xmit count disabled\n");
    }
    /* Wire-truth sampler (TxOk/TxER/qdisc-drop). */
    pthread_t sm_tid; int have_sm = 0;
    if (pthread_create(&sm_tid, NULL, sampler_thread, &ctx) == 0)
        have_sm = 1;
    else
        fprintf(stderr, "warning: sampler thread failed; "
                        "TxOk-based loss/BER unavailable\n");
    /* POLLPRI carrier-state monitor (primary current-state + timing). */
    pthread_t cp_tid; int have_cp = 0;
    if (pthread_create(&cp_tid, NULL, carrier_thread, &ctx) == 0)
        have_cp = 1;
    else
        fprintf(stderr, "warning: carrier thread failed; "
                        "link-state tracking degraded\n");
    /* Netlink link-state monitor (independent cross-check, unpinned). */
    pthread_t nl_tid; int have_nl = 0;
    if (pthread_create(&nl_tid, NULL, netlink_thread, &ctx) == 0)
        have_nl = 1;
    else
        fprintf(stderr, "warning: netlink thread failed; "
                        "link cross-check disabled\n");
    /* Small delay so RX is definitely in its poll loop. */
    sleep_ns(5 * 1000000);
    if (pthread_create(&tx_tid, NULL, tx_thread, &ctx) != 0) {
        perror("pthread_create tx"); g_running = 0;
        pthread_join(rx_tid, NULL);
        if (have_errq) pthread_join(errq_tid, NULL);
        return 1;
    }

    /* Main thread = supervisor: prints stats, polls kernel drops, enforces
     * duration. It touches no hot-path state except reading atomics.
     * g_tx_running is what the supervisor and signal handler clear; g_running
     * stays set until after the drain barrier. */
    while (g_tx_running) {
        sleep_ns(200 * 1000000);   /* 200 ms tick */
        uint64_t now = now_ns();

        if (duration_s > 0 &&
            (now - start_ns) >= (uint64_t)duration_s * 1000000000ULL) {
            g_tx_running = 0;
            break;
        }

        badfcs_drain(&badfcs_budget, &badfcs_suppressed);

        poll_kernel_drops(sock);
        g_stats.tx_wire_packets =
            read_nic_tx_packets(iface) - g_stats.tx_wire_base;
        { int ok;
          uint64_t d = read_sysfs_u64(iface, "carrier_down_count", &ok);
          if (ok) g_stats.carrier_down = d - g_stats.carrier_down_base;
          uint64_t u = read_sysfs_u64(iface, "carrier_up_count", &ok);
          if (ok) g_stats.carrier_up = u - g_stats.carrier_up_base;
          uint64_t c = read_sysfs_u64(iface, "carrier_changes", &ok);
          if (ok) g_stats.carrier_changes = c - g_stats.carrier_changes_base;
        }

        if (now - last_stat_ns >= 5000000000ULL) {
            uint64_t rd = atomic_exchange_explicit(&g_bfe_ringdrop, 0,
                                                   memory_order_relaxed);
            if (badfcs_suppressed || rd)
                fprintf(stderr, "[bad-FCS] %lu print(s) suppressed this "
                        "interval (cap %d)%s%lu%s\n",
                        badfcs_suppressed, BADFCS_PRINT_CAP,
                        rd ? "; ring overflow dropped " : "",
                        rd, rd ? " event(s)" : "");
            print_stats(csv, now - start_ns);
            badfcs_budget = BADFCS_PRINT_CAP;
            badfcs_suppressed = 0;
            last_stat_ns = now;
        }
    }

    /* ── Termination barrier (good-frame-gated) ─────────────────────────────
     * The session may only conclude when a GOOD frame confirms the final
     * enqueued frame — reading its seq is the only trustworthy confirmation
     * that the last frame completed the round trip. We:
     *   1. Stop TX, join it. Note the final enqueued seq.
     *   2. Wait until max_good_seq (highest GOOD-return seq) reaches the last
     *      enqueued seq (enq_final-1) — i.e. a good frame confirms the tail.
     *   3. If the link is down and no such good frame arrives, warn once per
     *      second up to 10 times ("reestablish link to complete test (N/10)").
     *      If a good frame arrives we conclude cleanly; otherwise we exit
     *      INCOMPLETE (non-zero) rather than conclude on an unconfirmed tail.
     * loss stays TxOk − good distinct returns throughout; max_good_seq is used
     * ONLY for this gate. */
    g_tx_running = 0;
    pthread_join(tx_tid, NULL);

    uint64_t enq_final = atomic_load_explicit(&g_tx_final_seq, memory_order_acquire);
    printf("\nTX stopped: %lu frames enqueued. Waiting for a good frame to "
           "confirm the final frame...\n", enq_final);

    int incomplete = 0;
    if (enq_final != 0) {
        uint64_t target = enq_final - 1;   /* last actually-enqueued seq */
        uint64_t warn_deadline = now_ns() + 1000ULL * 1000000ULL;
        int warns = 0;
        for (;;) {
            uint64_t mg = atomic_load_explicit((_Atomic uint64_t *)&g_rx.max_good_seq,
                                               memory_order_relaxed);
            int mgv = g_rx.max_good_valid;
            if (mgv && mg >= target) break;   /* good frame confirmed the tail */

            int link_up = atomic_load_explicit(&g_stats.link_state_up, memory_order_relaxed);
            if (now_ns() >= warn_deadline) {
                warns++;
                fprintf(stderr, "reestablish link to complete test (%d/10)%s "
                        "— max_good_seq=%lu target=%lu\n",
                        warns, link_up ? " [link reports UP]" : " [link DOWN]",
                        mgv ? mg : 0, target);
                if (warns >= 10) { incomplete = 1; break; }
                warn_deadline = now_ns() + 1000ULL * 1000000ULL;
            }
            sleep_ns(10 * 1000000);   /* 10ms poll */
        }
    }

    /* Stop RX and errq. */
    g_running = 0;
    pthread_join(rx_tid, NULL);
    if (have_errq) pthread_join(errq_tid, NULL);

    if (incomplete)
        fprintf(stderr, "\n*** TEST INCOMPLETE: link did not recover; the final "
                        "span could not be resolved from a good frame. Loss/BER "
                        "figures below are NOT final. ***\n");

    /* Final kernel-drop poll, wire count, sysfs carrier counters, and stats. */
    poll_kernel_drops(sock);
    g_stats.tx_wire_packets = read_nic_tx_packets(iface) - g_stats.tx_wire_base;
    { int ok;
      uint64_t d = read_sysfs_u64(iface, "carrier_down_count", &ok);
      if (ok) g_stats.carrier_down = d - g_stats.carrier_down_base;
      uint64_t u = read_sysfs_u64(iface, "carrier_up_count", &ok);
      if (ok) g_stats.carrier_up = u - g_stats.carrier_up_base;
      uint64_t c = read_sysfs_u64(iface, "carrier_changes", &ok);
      if (ok) g_stats.carrier_changes = c - g_stats.carrier_changes_base;
    }
    if (have_nl) pthread_join(nl_tid, NULL);
    if (have_cp) pthread_join(cp_tid, NULL);
    if (have_sm) pthread_join(sm_tid, NULL);
    uint64_t end_ns  = now_ns();
    badfcs_drain(&badfcs_budget, &badfcs_suppressed);
    if (badfcs_suppressed)
        fprintf(stderr, "[bad-FCS] %lu print(s) suppressed in final interval\n",
                badfcs_suppressed);
    print_stats(csv, end_ns - start_ns);

    if (g_linkev_csv) fclose(g_linkev_csv);
    fclose(csv);
    close(sock);

    if (incomplete) {
        printf("\nTEST INCOMPLETE — results in %s are not final.\n", csv_path);
        return 2;
    }
    printf("\nDone. Results written to %s\n", csv_path);
    return 0;
}
