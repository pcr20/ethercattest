#include "threads.h"
#include "frame.h"
#include "crc.h"
#include "stats.h"
#include "nic.h"

/* ── Thread infrastructure ──────────────────────────────────────────────────
 *
 * Two dedicated, CPU-pinned threads share one raw socket:
 *
 *   TX thread — builds and sends frames as fast as the NIC will accept them.
 *               Its ONLY backpressure is EAGAIN/ENOBUFS from a full TX ring,
 *               which on a saturated link means "the wire is busy". It never
 *               waits on the RX side. It also owns seq_mark_sent + seq_retire.
 *
 *   RX thread — batch-drains the socket with recvmmsg() into a large buffer
 *               pool, marks each returned seq, and parses ESC registers. It
 *               runs flat-out on its own core so the kernel RX queue is kept
 *               empty and never overflows. It owns seq_mark_returned.
 *
 * Kernel RX drops (queue overflow) are polled separately via PACKET_STATISTICS
 * and reported. If they are ever non-zero the drain saturated and the BER
 * numbers for that interval are invalid — the tool says so loudly.
 */


/* Pin the calling thread to a single CPU core. Returns 0 on success. */
int pin_to_core(int core) {
    if (core < 0) return 0;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

/* Try to raise the calling thread to SCHED_FIFO real-time priority.
 * Best-effort: silently continues at normal priority if not permitted.
 * Skipped entirely on systems with fewer than 4 online CPUs, where a
 * SCHED_FIFO thread could monopolise the only core and starve the
 * supervisor/other worker (observed as an apparent hang). */
static void try_realtime(int prio) {
    if (sysconf(_SC_NPROCESSORS_ONLN) < 4) return;
    struct sched_param sp = { .sched_priority = prio };
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
}

/* ── TX thread ──────────────────────────────────────────────────────────── */
/* Maximum frames allowed outstanding (sent but not yet returned) before TX
 * pauses. This bounds the sent/received gap so backpressure reflects the WIRE
 * draining frames, not the kernel TX ring / socket buffer swallowing them.
 * Without this cap, large buffers + a high-latency link (e.g. a passive
 * loopback plug) let TX build a huge backlog that then shows up as spurious
 * "loss" at shutdown. On a real EtherCAT chain the RTT is microseconds so the
 * cap is essentially never hit; it only clamps pathological buffering.
 * ~4000 frames ≈ 0.5s of wire time at 8100 fps — ample in-flight headroom. */
#define MAX_INFLIGHT 4000

void *tx_thread(void *arg) {
    ThreadCtx *ctx = (ThreadCtx *)arg;
    pin_to_core(ctx->tx_core);
    try_realtime(80);

    uint8_t  tx_buf[MAX_FRAME];
    uint64_t seq          = 0;
    uint64_t interval_ns  = (ctx->rate_hz > 0) ? (1000000000ULL / ctx->rate_hz) : 0;
    uint64_t next_send_ns = now_ns();

    while (g_tx_running) {
        if (interval_ns) {
            uint64_t now = now_ns();
            if (now < next_send_ns) {
                /* Rate-limited mode: sleep the remaining time. */
                sleep_ns(next_send_ns - now);
            }
        }

        /* Issue 2: TX never halts. There is no backlog cap and no credit
         * window. The kernel/PHY discards frames when there is no link (those
         * frames are simply not counted in TxOk, correctly excluded from BER),
         * so there is nothing to protect against by pausing — and any pause
         * risks a deadlock. TX just keeps offering frames: on success, count and
         * advance; on EAGAIN/ENOBUFS (ring full = line-rate backpressure, or no
         * carrier), a short 10µs sleep and retry the SAME frame. This guarantees
         * low-latency resumption: the instant the link returns, the next send()
         * succeeds and frames flow, because TX never stopped trying. */

        int frame_len = build_frame(tx_buf, sizeof(tx_buf), ctx->src_mac,
                                    ctx->loopback ? 0 : ctx->num_slaves,
                                    seq, ctx->loopback);

        /* Publish the actual on-wire bits per frame once (constant for the
         * run: only seq/payload contents vary, not the size). frame_len + the
         * 4-byte FCS appended by the NIC = the CRC-protected on-wire bytes. */
        if (atomic_load_explicit(&g_wire_bits_per_frame, memory_order_relaxed) == 0)
            atomic_store_explicit(&g_wire_bits_per_frame,
                                  ((uint64_t)frame_len + 4) * 8,
                                  memory_order_relaxed);

        int sent = send(ctx->sock, tx_buf, frame_len, 0);
        if (sent > 0) {
            /* Accounting is count-based and TxOk-anchored; TX only counts what
             * it enqueued (for the backlog cap and pipeline display). Loss is
             * TxOk − distinct returns, computed elsewhere. */
            atomic_fetch_add_explicit(&g_stats.frames_enqueued, 1, memory_order_relaxed);
            seq++;
            if (interval_ns) next_send_ns += interval_ns;

        } else if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS) {
            /* TX ring/qdisc full = backpressure. Sleep a short bounded time and
             * retry the SAME seq — nothing is lost (the frame was not enqueued). */
            atomic_fetch_add_explicit(&g_stats.tx_backpressure, 1, memory_order_relaxed);
            sleep_ns(10000);   /* 10 microseconds */
        } else {
            static _Atomic uint64_t send_errors = 0;
            uint64_t e = atomic_fetch_add_explicit(&send_errors, 1, memory_order_relaxed);
            if (e < 5) {
                fprintf(stderr, "send() failed (frame_len=%d): %s\n",
                        frame_len, strerror(errno));
                if (errno == EMSGSIZE)
                    fprintf(stderr, "  -> frame too large for interface MTU.\n");
            }
            if (interval_ns) next_send_ns += interval_ns;
        }
    }

    /* Publish the final enqueued seq count (for the termination logic, which
     * waits for the RX high-water mark to reach it on a good frame). */
    atomic_store_explicit(&g_tx_final_seq, seq, memory_order_release);
    return NULL;
}

/* ── RX thread ──────────────────────────────────────────────────────────── */
#define RX_BATCH 256
/* Received frames can carry a 4-byte trailing FCS (rx-fcs on) and, with
 * rx-all on, may be corrupt/oversized. Give the RX buffer headroom. */
#define RX_BUF   (MAX_FRAME + 8)
/* Ethernet FCS length appended when rx-fcs is enabled. */
#define FCS_LEN  4
/* CRC32 residual of a VALID frame computed over (frame + its FCS) using our
 * eth_crc32 (final ^0xFFFFFFFF) with the FCS appended little-endian. Verified
 * stable across all frame lengths/contents. NOTE: the hardware's delivered FCS
 * byte order may differ — the tool prints the observed good-frame residual on
 * startup so this can be confirmed against the real NIC and corrected in one
 * line if needed. */
/* Alternate residual for the no-final-inversion / different-byte-order FCS
 * delivery convention. Accepting both makes detector 2 robust without
 * auto-calibrating. */
void *rx_thread(void *arg) {
    ThreadCtx *ctx = (ThreadCtx *)arg;
    pin_to_core(ctx->rx_core);
    try_realtime(90);   /* RX slightly higher: draining must never fall behind */

    /* Use a NON-BLOCKING socket and drive the wait with poll(). The recvmmsg
     * `timeout` argument is unreliable — on Linux it is only evaluated after
     * at least one datagram is received, so on an empty queue a blocking
     * recvmmsg ignores the timeout and blocks forever (this caused a shutdown
     * hang). poll() gives us a real, interruptible wait and lets us re-check
     * g_running every tick. */
    int flags = fcntl(ctx->sock, F_GETFL, 0);
    fcntl(ctx->sock, F_SETFL, flags | O_NONBLOCK);

    static uint8_t bufs[RX_BATCH][RX_BUF];
    static char    ctrls[RX_BATCH][CMSG_SPACE(sizeof(struct tpacket_auxdata))];
    struct mmsghdr msgs[RX_BATCH];
    struct iovec   iov[RX_BATCH];

    memset(msgs, 0, sizeof(msgs));
    for (int i = 0; i < RX_BATCH; i++) {
        iov[i].iov_base = bufs[i];
        iov[i].iov_len  = RX_BUF;
        msgs[i].msg_hdr.msg_iov        = &iov[i];
        msgs[i].msg_hdr.msg_iovlen     = 1;
        msgs[i].msg_hdr.msg_control    = ctrls[i];
        msgs[i].msg_hdr.msg_controllen = sizeof(ctrls[i]);
    }

    struct pollfd pfd = { .fd = ctx->sock, .events = POLLIN };

    /* Per-frame handler: strip/verify FCS, check auxdata FCS-fail, parse.
     * rx_fcs_on / rx_all_on come from ctx (set from ethtool state at startup). */
    #define HANDLE_FRAME(idx) do {                                              \
        int raw_len = msgs[idx].msg_len;                                        \
        atomic_fetch_add_explicit(&g_stats.frames_received, 1,                  \
                                  memory_order_relaxed);                       \
        rx_check_len(raw_len, ctx->rx_fcs_on);                                 \
        /* Grab PACKET_AUXDATA tp_status (detector 1 source). */               \
        uint32_t tp_status = 0; int have_aux = 0;                              \
        for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msgs[idx].msg_hdr); cm;       \
             cm = CMSG_NXTHDR(&msgs[idx].msg_hdr, cm)) {                       \
            if (cm->cmsg_level == SOL_PACKET &&                               \
                cm->cmsg_type  == PACKET_AUXDATA) {                           \
                struct tpacket_auxdata aux;                                    \
                memcpy(&aux, CMSG_DATA(cm), sizeof(aux));                     \
                tp_status = aux.tp_status; have_aux = 1;                       \
            }                                                                 \
        }                                                                     \
        int content_len = raw_len;                                             \
        /* Detector 2: Ethernet FCS validity via the CRC32 RESIDUAL method.    \
         * Compute CRC32 over the ENTIRE delivered frame INCLUDING its 4-byte  \
         * FCS trailer; a valid frame yields a fixed magic residual. For our   \
         * eth_crc32 (which applies the final ^0xFFFFFFFF), that constant is   \
         * 0x2144DF9C. This is convention/byte-order robust — it's how the     \
         * hardware validates — unlike recomputing and comparing the trailer.  \
         * The EtherCAT content (for datagram parsing / payload CRC) is the    \
         * frame minus the 4 FCS bytes. */                                     \
        int comp_bad = 0;                                                      \
        if (ctx->rx_fcs_on && raw_len >= (int)(ETH_HDR_LEN + FCS_LEN)) {       \
            content_len = raw_len - FCS_LEN;                                    \
            uint32_t residual = eth_crc32(bufs[idx], raw_len);                 \
            /* Accept either standard residual: 0x2144DF1C (our LE-append       \
             * convention) or 0xDEBB20E3 (no-final-inversion convention). A     \
             * frame matching NEITHER is genuinely bad. This covers the two     \
             * common FCS delivery conventions without auto-calibration. */     \
            if (residual != ETH_FCS_RESIDUAL && residual != ETH_FCS_RESIDUAL2)  \
                comp_bad = 1;                                                    \
            static _Atomic int shown = 0;                                     \
            if (!comp_bad &&                                                  \
                !atomic_exchange_explicit(&shown, 1, memory_order_relaxed))    \
                fprintf(stderr, "[FCS] good-frame residual = 0x%08x\n",         \
                        residual);                                              \
        }                                                                     \
        if (comp_bad) {                                                        \
            atomic_fetch_add_explicit(&g_stats.rx_bad_fcs_computed, 1,         \
                                      memory_order_relaxed);                   \
            /* Sanity: if nearly every frame reads bad early on, our FCS       \
             * offset/byte-order assumption is wrong, not the link. Warn once. */\
            static _Atomic int warned = 0;                                    \
            uint64_t rc = atomic_load_explicit(&g_stats.frames_received,       \
                                               memory_order_relaxed);         \
            uint64_t bc = atomic_load_explicit(&g_stats.rx_bad_fcs_computed,   \
                                               memory_order_relaxed);         \
            if (rc > 2000 && bc > rc / 2 &&                                   \
                !atomic_exchange_explicit(&warned, 1, memory_order_relaxed)) { \
                fprintf(stderr,                                               \
                  "\n*** WARNING: >50%% of frames fail the FCS residual check " \
                  "***\n  The residual constant likely differs for this driver "\
                  "(FCS delivered\n  pre-inversion?). Observed residual "        \
                  "0x%08x vs expected 0x%08x.\n  If the observed value is "      \
                  "stable, that IS the good-frame residual —\n  change "         \
                  "ETH_FCS_RESIDUAL to it. Detector 1/payload CRC unaffected.\n",\
                  eth_crc32(bufs[idx], raw_len), ETH_FCS_RESIDUAL);            \
            }                                                                 \
            /* Detector 1 calibration: print kernel tp_status on first few. */ \
            badfcs_push(tp_status, raw_len);   /* non-blocking; printed by  \
                                                  the supervisor thread */    \
        }                                                                     \
        /* Detector 1: count frames the kernel flags via auxdata. We treat a   \
         * frame as auxdata-bad if it lacks CSUM_VALID AND our own check also  \
         * failed — until calibrated, detector 2 is authoritative and this     \
         * mirrors it. Once we know the exact bit from the calibration prints  \
         * above, this can be tightened to a pure kernel signal. */            \
        if (have_aux && comp_bad)                                             \
            atomic_fetch_add_explicit(&g_stats.rx_bad_fcs_auxdata, 1,          \
                                      memory_order_relaxed);                   \
        if (content_len < (int)(ETH_HDR_LEN + ECAT_HDR_LEN + ECAT_DG_HDR_LEN)) \
            atomic_fetch_add_explicit(&g_stats.rx_truncated, 1,                \
                                      memory_order_relaxed);                   \
        int payload_ok = 0;                                                    \
        uint64_t rseq = parse_return_frame(bufs[idx], content_len,             \
                                           ctx->num_slaves, ctx->loopback,     \
                                           !comp_bad, &payload_ok);            \
        /* Count-based accounting (Issue 1). loss = TxOk − good distinct        \
         * returns. A frame counts as a GOOD return iff its Ethernet FCS is     \
         * valid (!comp_bad) AND its payload CRC32C verified (payload_ok) — only \
         * then is its seq trustworthy for the dedup/count. A corrupt frame's    \
         * seq may be garbage, so it does NOT count as a good return; it is      \
         * tracked by the corruption detectors instead, AND — by not being a     \
         * good return — it also falls into loss (loss counts frames that did    \
         * not come back GOOD). So a corrupt frame is deliberately in BOTH loss  \
         * and corruption; these are two overlapping measures (see README).      \
         * rx_new_good_return also advances max_good_seq for the good-frame-     \
         * gated termination. */                                                \
        int good = (!comp_bad) && payload_ok;                                  \
        if (good && rseq != UINT64_MAX && rx_new_good_return(rseq)) {          \
            atomic_fetch_add_explicit(&g_stats.distinct_returns, 1,            \
                                      memory_order_relaxed);                   \
        }                                                                     \
        iov[idx].iov_len = RX_BUF;                                             \
        msgs[idx].msg_hdr.msg_controllen = sizeof(ctrls[idx]);                 \
    } while (0)

    #define DRAIN_ONCE() do {                                                  \
        for (;;) {                                                             \
            int n = recvmmsg(ctx->sock, msgs, RX_BATCH, MSG_DONTWAIT, NULL);   \
            if (n <= 0) break;                                                 \
            for (int i = 0; i < n; i++) HANDLE_FRAME(i);                        \
        }                                                                     \
    } while (0)

    while (g_running) {
        int pr = poll(&pfd, 1, 100);   /* 100ms tick; re-checks g_running */
        if (pr < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "poll: %s\n", strerror(errno));
            break;
        }
        if (pr == 0) continue;         /* timeout — loop to re-check g_running */
        if (pfd.revents & POLLIN)
            DRAIN_ONCE();
    }

    /* Final drain: catch anything still queued at shutdown. Bounded by a short
     * wall-clock budget so we can never hang here. */
    uint64_t deadline = now_ns() + 200 * 1000000ULL;  /* 200ms */
    while (now_ns() < deadline) {
        int pr = poll(&pfd, 1, 20);
        if (pr <= 0) break;
        if (pfd.revents & POLLIN)
            DRAIN_ONCE();
    }
    #undef DRAIN_ONCE
    #undef HANDLE_FRAME
    return NULL;
}
/* ── Error-queue reader thread (model B: TX-confirmation allocator) ──────────
 * Drains MSG_ERRQUEUE. Each completion carries:
 *   - an SCM_TIMESTAMPING cmsg (the software timestamp), and
 *   - a sock_extended_err cmsg whose ee_data is the per-send frame ID
 *     (enabled via SOF_TIMESTAMPING_OPT_ID). Verified empirically that
 *     ee_data == send-order index == our seq (one frame per send, in order).
 *
 * For each completion we call seq_mark_sent(id): THIS is where a frame becomes
 * "outstanding on the wire". Loss is therefore wire-exact — a frame enqueued
 * but never transmitted never gets a completion, is never marked outstanding,
 * and so can never be counted as lost.
 *
 * This thread is now the sole allocator (0->1) and reclaimer (->0) of seq
 * slots, preserving the single-writer invariant of the lock-free tracker
 * (RX still does the only 1->2 transition via CAS).
 *
 * ee_data is 32-bit and wraps at 2^32. At 8127 fps that is ~6 days before the
 * first wrap; we widen it to 64-bit by tracking the high word, so long BER
 * runs remain correct. */
void *errq_thread(void *arg) {
    ThreadCtx *ctx = (ThreadCtx *)arg;
    pin_to_core(ctx->errq_core);
    try_realtime(85);   /* between RX(90) and TX(80): must keep up with TX */

    char ctrl[512];
    struct msghdr msg;
    struct pollfd pfd = { .fd = ctx->sock, .events = POLLERR };

    /* Process one drained completion: count it as a driver-xmit confirmation.
     * This is now DISPLAY-ONLY (TX pipeline diagnostic) — accounting is fully
     * RX-driven and does not use TX timestamps. */
    #define HANDLE_COMPLETION(msgp) do {                                        \
        for (struct cmsghdr *cm = CMSG_FIRSTHDR(msgp); cm;                     \
             cm = CMSG_NXTHDR(msgp, cm)) {                                     \
            if (cm->cmsg_level == SOL_SOCKET &&                               \
                cm->cmsg_type  == SCM_TIMESTAMPING) {                         \
                atomic_fetch_add_explicit(&g_stats.tx_ts_completions, 1,       \
                                          memory_order_relaxed);              \
                atomic_fetch_add_explicit(&g_stats.frames_transmitted, 1,      \
                                          memory_order_relaxed);              \
            }                                                                 \
        }                                                                     \
    } while (0)

    while (g_running) {
        int pr = poll(&pfd, 1, 100);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr > 0) {
            for (;;) {
                memset(&msg, 0, sizeof(msg));
                msg.msg_control = ctrl; msg.msg_controllen = sizeof(ctrl);
                int n = recvmsg(ctx->sock, &msg, MSG_ERRQUEUE | MSG_DONTWAIT);
                if (n < 0) break;
                HANDLE_COMPLETION(&msg);
            }
        }
    }

    /* Final drain — catch completions for the last frames TX pushed out. */
    uint64_t deadline = now_ns() + 500 * 1000000ULL;
    while (now_ns() < deadline) {
        int pr = poll(&pfd, 1, 50);
        if (pr <= 0) { if (pr == 0) break; continue; }
        for (;;) {
            memset(&msg, 0, sizeof(msg));
            msg.msg_control = ctrl; msg.msg_controllen = sizeof(ctrl);
            int n = recvmsg(ctx->sock, &msg, MSG_ERRQUEUE | MSG_DONTWAIT);
            if (n < 0) break;
            HANDLE_COMPLETION(&msg);
        }
    }
    #undef HANDLE_COMPLETION
    return NULL;
}

/* IFF_LOWER_UP = carrier present. Defined here to avoid pulling in
 * <linux/if.h>, which conflicts with the already-included <net/if.h>. Value is
 * stable ABI (bit 16). */
#ifndef IFF_LOWER_UP
#define IFF_LOWER_UP 0x10000
#endif

/* ── POLLPRI carrier-state thread (primary current-state + timing) ──────────
 * Opens /sys/class/net/<if>/carrier and blocks in poll() on POLLPRI|POLLERR.
 * The netdev core calls sysfs_notify() on "carrier" at every transition, which
 * wakes this thread — event-driven, no polling loop, minimal latency. On each
 * wake it re-reads the attribute (lseek to 0 first, as sysfs requires) and
 * updates link_state_up with the TRUE CURRENT value, and logs a timestamped
 * transition.
 *
 * NOTE ON COALESCING: sysfs_notify coalesces — if carrier changes twice before
 * we read, we get one wake and see only the latest value. So this reliably
 * tracks CURRENT STATE and gives timing for transitions it resolves, but is not
 * a guaranteed count of every bounce. The authoritative transition COUNT is the
 * kernel's carrier_*_count sysfs counters (read by the supervisor). This thread
 * owns link_state_up because current-state is exactly what coalescing preserves. */
/* ── Wire-truth sampler thread ──────────────────────────────────────────────
 * Periodically (every ~20ms) reads the hardware tally counters (TxOk via
 * ethtool GSTATS, TxER) and the kernel qdisc tx_dropped, base-subtracted into
 * g_txok / g_txer / g_qdisc_drop. TxOk is the measurement boundary and drives
 * both the loss figure (loss = TxOk − distinct returns) and the TX credit
 * window (credit = enqueued − TxOk). The diagnostic confirmed TxOk updates
 * smoothly (~per-frame under saturation), so 20ms sampling gives a sharp
 * boundary with at most ~a few hundred frames of edge fuzz during an outage
 * transition. Runs unpinned; the ioctl is cheap. */
void *sampler_thread(void *arg) {
    ThreadCtx *ctx = (ThreadCtx *)arg;
    while (g_running) {
        uint64_t txok = read_nic_tx_packets(ctx->iface_name);
        uint64_t txer = read_nic_tx_errors(ctx->iface_name);
        int ok = 0;
        uint64_t qd = read_sysfs_u64(ctx->iface_name, "statistics/tx_dropped", &ok);
        if (txok >= g_stats.tx_wire_base)
            atomic_store_explicit(&g_txok, txok - g_stats.tx_wire_base, memory_order_relaxed);
        if (txer >= g_stats.tx_err_base)
            atomic_store_explicit(&g_txer, txer - g_stats.tx_err_base, memory_order_relaxed);
        if (ok && qd >= g_stats.qdisc_drop_base)
            atomic_store_explicit(&g_qdisc_drop, qd - g_stats.qdisc_drop_base, memory_order_relaxed);
        sleep_ns(20 * 1000000ULL);   /* 20ms */
    }
    /* Final sample so the last stats/termination see current values. */
    uint64_t txok = read_nic_tx_packets(ctx->iface_name);
    uint64_t txer = read_nic_tx_errors(ctx->iface_name);
    if (txok >= g_stats.tx_wire_base)
        atomic_store_explicit(&g_txok, txok - g_stats.tx_wire_base, memory_order_relaxed);
    if (txer >= g_stats.tx_err_base)
        atomic_store_explicit(&g_txer, txer - g_stats.tx_err_base, memory_order_relaxed);
    return NULL;
}

void *carrier_thread(void *arg) {
    ThreadCtx *ctx = (ThreadCtx *)arg;

    char path[256];
    snprintf(path, sizeof(path), "/sys/class/net/%s/carrier", ctx->iface_name);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "carrier open(%s): %s (POLLPRI state disabled)\n",
                path, strerror(errno));
        return NULL;
    }

    /* Prime: read initial value and consume the initial POLLPRI that poll()
     * always reports immediately for a freshly-opened sysfs attribute. */
    char c = '1';
    { ssize_t r = pread(fd, &c, 1, 0); if (r < 1) c = '1'; }
    int prev_up = (c == '1');
    atomic_store_explicit(&g_stats.link_state_up, prev_up, memory_order_relaxed);

    uint64_t last_down_ns = 0;
    struct pollfd pfd = { .fd = fd, .events = POLLPRI | POLLERR };

    /* Detect whether POLLPRI notification actually works on this attribute.
     * Not all drivers/kernels call sysfs_notify() on "carrier" (some virtual
     * interfaces don't). Probe once: a working attribute reports POLLPRI on the
     * priming poll of a fresh fd. If it doesn't, fall back to a fast timed
     * re-read so link_state_up stays correct regardless. */
    int pollpri_works;
    {
        struct pollfd probe = { .fd = fd, .events = POLLPRI | POLLERR };
        int pr = poll(&probe, 1, 0);
        pollpri_works = (pr > 0 && (probe.revents & POLLPRI));
        /* consume the priming event */
        char tmp; if (pread(fd, &tmp, 1, 0) < 0) { /* ignore */ }
    }
    if (!pollpri_works)
        fprintf(stderr, "note: POLLPRI carrier notification unavailable on %s; "
                        "using fast timed re-read fallback (10ms)\n", ctx->iface_name);

    while (g_running) {
        int pr;
        if (pollpri_works) {
            pr = poll(&pfd, 1, 200);
            if (pr < 0) { if (errno == EINTR) continue; break; }
            if (pr == 0) continue;
            if (!(pfd.revents & (POLLPRI | POLLERR))) continue;
        } else {
            /* Fallback: fast timed re-read. 10ms cadence catches all but the
             * briefest flaps; still far better than nothing, and the sysfs
             * counters remain the authoritative total regardless. */
            sleep_ns(10 * 1000000);
        }

        /* Re-read current carrier value (must pread from offset 0). */
        char v = '0';
        if (pread(fd, &v, 1, 0) < 1) continue;
        int up_now = (v == '1');
        if (up_now == prev_up) continue;        /* spurious wake / no change */

        uint64_t now = now_ns();
        double t_rel = (now - g_start_ns) / 1e9;

        if (!up_now) {
            atomic_fetch_add_explicit(&g_stats.link_down_cp, 1, memory_order_relaxed);
            last_down_ns = now;
            fprintf(stderr, "[+%.3fs] LINK DOWN (host NIC %s)\n", t_rel, ctx->iface_name);
            if (g_linkev_csv) {
                pthread_mutex_lock(&g_linkev_mtx);
                fprintf(g_linkev_csv, "%.3f,DOWN,\n", t_rel);
                fflush(g_linkev_csv);
                pthread_mutex_unlock(&g_linkev_mtx);
            }
        } else {
            atomic_fetch_add_explicit(&g_stats.link_up_cp, 1, memory_order_relaxed);
            double down_ms = last_down_ns ? (now - last_down_ns) / 1e6 : -1.0;
            if (down_ms >= 0) fprintf(stderr, "[+%.3fs] LINK UP (down %.0fms)\n", t_rel, down_ms);
            else              fprintf(stderr, "[+%.3fs] LINK UP\n", t_rel);
            if (g_linkev_csv) {
                pthread_mutex_lock(&g_linkev_mtx);
                if (down_ms >= 0) fprintf(g_linkev_csv, "%.3f,UP,%.0f\n", t_rel, down_ms);
                else              fprintf(g_linkev_csv, "%.3f,UP,\n", t_rel);
                fflush(g_linkev_csv);
                pthread_mutex_unlock(&g_linkev_mtx);
            }
        }
        atomic_store_explicit(&g_stats.link_state_up, up_now, memory_order_relaxed);
        prev_up = up_now;
    }

    close(fd);
    return NULL;
}

/* ── Netlink link-state monitor thread (independent cross-check) ─────────────
 * Third measurement of link transitions, independent of the sysfs counters and
 * the POLLPRI thread. Netlink RTM_NEWLINK notifications are generated by the
 * linkwatch subsystem, which is itself rate-limited/coalescing, so netlink is
 * NOT authoritative for counts during a fast flap — it is a cross-check whose
 * divergence from the sysfs counter quantifies flap severity.
 *
 * Improvements over the naive version:
 *  - Large SO_RCVBUFFORCE so a burst of events doesn't overflow the socket.
 *  - NETLINK_OVERRUN / ENOBUFS detected and COUNTED (not silently lost), so a
 *    residual undercount is surfaced honestly.
 *  - Tighter poll timeout + full drain each wake to minimise the overflow
 *    window.
 *  - Does NOT own link_state_up (the POLLPRI thread does) — it only maintains
 *    its own cross-check counters. */
void *netlink_thread(void *arg) {
    ThreadCtx *ctx = (ThreadCtx *)arg;

    int nl = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (nl < 0) {
        fprintf(stderr, "netlink socket: %s (cross-check disabled)\n", strerror(errno));
        return NULL;
    }

    /* (a) Large receive buffer so bursts don't overflow. Link messages are
     * tiny; 4MB holds many thousands. */
    int rcvbuf = 4 * 1024 * 1024;
    if (setsockopt(nl, SOL_SOCKET, SO_RCVBUFFORCE, &rcvbuf, sizeof(rcvbuf)) < 0)
        setsockopt(nl, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    struct sockaddr_nl sa;
    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;
    sa.nl_groups = RTMGRP_LINK;   /* link-state changes only */
    if (bind(nl, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        fprintf(stderr, "netlink bind: %s (cross-check disabled)\n", strerror(errno));
        close(nl);
        return NULL;
    }

    int ok = 0;
    uint64_t carrier = read_sysfs_u64(ctx->iface_name, "carrier", &ok);
    int prev_up = ok ? (carrier != 0) : 1;

    struct pollfd pfd = { .fd = nl, .events = POLLIN };
    uint8_t buf[16384];

    while (g_running) {
        /* (c) Short timeout so we revisit the socket often, minimising the
         * window in which a burst can overflow. */
        int pr = poll(&pfd, 1, 50);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr == 0) continue;

        /* Full drain each wake. */
        for (;;) {
            ssize_t len = recv(nl, buf, sizeof(buf), MSG_DONTWAIT);
            if (len < 0) {
                /* (b) Overflow: kernel dropped messages. Count it, and note the
                 * socket must be re-synced (the datagram is lost). */
                if (errno == ENOBUFS) {
                    atomic_fetch_add_explicit(&g_stats.netlink_overflows, 1,
                                              memory_order_relaxed);
                    continue;   /* keep draining */
                }
                break;          /* EAGAIN — queue empty */
            }
            if (len == 0) break;

            for (struct nlmsghdr *nh = (struct nlmsghdr *)buf;
                 NLMSG_OK(nh, (unsigned)len); nh = NLMSG_NEXT(nh, len)) {
                if (nh->nlmsg_type == NLMSG_DONE) break;
                if (nh->nlmsg_type == NLMSG_OVERRUN) {
                    atomic_fetch_add_explicit(&g_stats.netlink_overflows, 1,
                                              memory_order_relaxed);
                    continue;
                }
                if (nh->nlmsg_type != RTM_NEWLINK && nh->nlmsg_type != RTM_DELLINK)
                    continue;
                struct ifinfomsg *ifi = (struct ifinfomsg *)NLMSG_DATA(nh);
                if (ifi->ifi_index != ctx->ifindex) continue;
                int up_now = (ifi->ifi_flags & IFF_LOWER_UP) ? 1 : 0;
                if (up_now == prev_up) continue;
                if (!up_now) atomic_fetch_add_explicit(&g_stats.link_down_nl, 1, memory_order_relaxed);
                else         atomic_fetch_add_explicit(&g_stats.link_up_nl, 1, memory_order_relaxed);
                prev_up = up_now;
            }
        }
    }

    close(nl);
    return NULL;
}

