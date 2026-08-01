/*
 * ecat_ber.c  —  EtherCAT BER / CRC error logger
 *
 * Sends maximum-rate EtherCAT NOP frames (carrying a UDP-like payload)
 * through a chain of AX58100 slaves, then reads back:
 *   - Lost frame count (frames never returned)
 *   - CRC errors from the host NIC PHY (via ethtool SIOCETHTOOL)
 *   - CRC / lost-link counters from each ESC slave (registers 0x0300–0x0313)
 *
 * Usage:
 *   sudo ./ecat_ber -i eth1 -s 4 [-r 1000] [-d 60] [-o results.csv] [-l]
 *
 *   -i <iface>   Network interface connected to EtherCAT ring (mandatory)
 *   -s <count>   Number of EtherCAT slaves in chain (mandatory)
 *   -r <hz>      Frame rate in Hz (default: max / 0 = saturate)
 *   -d <secs>    Test duration in seconds (default: 0 = run until Ctrl-C)
 *   -o <file>    CSV output file (default: ber_results.csv)
 *   -l           Loopback mode: no slaves, physical RJ45 loopback cable
 *   -v           Verbose: print per-slave ESC registers each poll cycle
 *
 * Build:
 *   gcc -O2 -Wall -o ecat_ber ecat_ber.c -lm
 *
 * Requires:
 *   - Linux, raw packet socket (CAP_NET_RAW / root)
 *   - Interface must NOT have a normal IP stack running on it
 *     (ip addr flush dev eth1 && ip link set eth1 promisc on)
 *
 * Notes on BER measurement:
 *   Each frame carries a 1342-byte NOP payload (max EtherCAT payload with
 *   room for 4 APRD slave-poll datagrams). At 100BASE-TX (100 Mbit/s) with
 *   ~1500-byte frames, maximum frame rate ≈ 8100 frames/s.
 *   Bits per frame ≈ 12000.  To accumulate 10^13 bits: ~10^13/12000 ≈ 8.3×10^8
 *   frames ≈ 28 hours at saturation. Plan accordingly.
 *
 * Loopback cable wiring (100BASE-TX only, prevents 1000BASE-T negotiation):
 *   Pin 1 (TX+) → Pin 3 (RX+)
 *   Pin 2 (TX-) → Pin 6 (RX-)
 *   Leave pins 4,5,7,8 unconnected.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <math.h>
#include <unistd.h>
#include <getopt.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <net/ethernet.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <linux/net_tstamp.h>
#include <linux/errqueue.h>
#include <linux/ethtool.h>
#include <linux/sockios.h>
#include <arpa/inet.h>

/* ── EtherCAT constants ─────────────────────────────────────────────────── */
#define ETHERTYPE_ECAT       0x88A4
#define ECAT_CMD_NOP         0x00
#define ECAT_CMD_BRD         0x07
#define ECAT_CMD_APRD        0x01
#define ECAT_IDX_NOP         0xFF
#define ECAT_IDX_BRD         0xFE
#define ECAT_IDX_APRD_BASE   0x00   /* APRD datagrams use indices 0x00..N */

/* ESC diagnostic register base addresses (ETG.1000.6) */
#define ESC_REG_CRC_BASE     0x0300  /* CRC error counters, one per port pair */
#define ESC_REG_LOSTLNK_BASE 0x0310  /* Lost link counters, one per port      */

/* Max slaves we support for APRD polling */
#define MAX_SLAVES           32

/* Ethernet frame limits.
 * NOTE: 1518 includes the 4-byte FCS which the NIC hardware appends.
 * A raw AF_PACKET socket can only send eth_header(14) + payload(<=1500)
 * = 1514 bytes. Sending more returns EMSGSIZE. */
#define MAX_FRAME            1514
#define ETH_HDR_LEN          14
#define ECAT_HDR_LEN         2
#define ECAT_DG_HDR_LEN      10   /* cmd(1)+idx(1)+addr(4)+len(2)+irq(2) */
#define ECAT_DG_WKC_LEN      2
#define ECAT_DG_OVERHEAD     (ECAT_DG_HDR_LEN + ECAT_DG_WKC_LEN)

/* Poll ESC counters every this many frames */
#define ESC_POLL_INTERVAL    10000

/* ── NOP payload layout ─────────────────────────────────────────────────────
 *   [ 8 bytes ] sequence number (uint64 LE)
 *   [ 4 bytes ] CRC32C over (seq bytes ++ payload bytes)
 *   [ N bytes ] pseudo-random payload (xorshift64 seeded from seq)
 * The CRC covers the 8 seq bytes and all N payload bytes. */
#define PL_SEQ_OFF   0
#define PL_SEQ_LEN   8
#define PL_CRC_OFF   8
#define PL_CRC_LEN   4
#define PL_DATA_OFF  12          /* pseudo-random payload starts here */
#define PL_HDR_LEN   (PL_SEQ_LEN + PL_CRC_LEN)   /* 12 bytes before random data */

/* ── CRC32C (Castagnoli) ────────────────────────────────────────────────────
 * Hardware SSE4.2 path when available (Ryzen 5300U has it), portable
 * table-driven software fallback otherwise. Both ends use the same function,
 * so the choice is transparent to correctness. */
#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#include <nmmintrin.h>   /* _mm_crc32_u8/u64 */
static int g_have_sse42 = 0;
static void crc32c_detect(void) {
    unsigned int eax, ebx, ecx, edx;
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx))
        g_have_sse42 = (ecx & bit_SSE4_2) ? 1 : 0;
}
#else
static int g_have_sse42 = 0;
static void crc32c_detect(void) { g_have_sse42 = 0; }
#endif

static uint32_t g_crc32c_tab[256];
static void crc32c_init_table(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
        g_crc32c_tab[i] = c;
    }
}
static void crc32c_init(void) { crc32c_detect(); crc32c_init_table(); }

static inline uint32_t crc32c_sw(uint32_t crc, const uint8_t *p, size_t n) {
    crc = ~crc;
    for (size_t i = 0; i < n; i++)
        crc = g_crc32c_tab[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static inline uint32_t crc32c(const uint8_t *p, size_t n) {
#if defined(__x86_64__) || defined(__i386__)
    if (g_have_sse42) {
        uint32_t crc = ~0u;
        size_t i = 0;
    #if defined(__x86_64__)
        for (; i + 8 <= n; i += 8) {
            uint64_t v; memcpy(&v, p + i, 8);
            crc = (uint32_t)_mm_crc32_u64(crc, v);
        }
    #endif
        for (; i < n; i++) crc = _mm_crc32_u8(crc, p[i]);
        return ~crc;
    }
#endif
    return crc32c_sw(0, p, n);
}

/* Incremental CRC32C: process a chunk given the running *inverted* state.
 * Call crc32c_begin() to get the initial state, feed chunks with
 * crc32c_update(), finish with crc32c_final(). This lets us CRC two
 * non-contiguous regions (seq and payload, separated by the CRC hole) as one
 * logical byte stream. */
static inline uint32_t crc32c_begin(void) { return ~0u; }
static inline uint32_t crc32c_update(uint32_t crc, const uint8_t *p, size_t n) {
#if defined(__x86_64__) || defined(__i386__)
    if (g_have_sse42) {
        size_t i = 0;
    #if defined(__x86_64__)
        for (; i + 8 <= n; i += 8) {
            uint64_t v; memcpy(&v, p + i, 8);
            crc = (uint32_t)_mm_crc32_u64(crc, v);
        }
    #endif
        for (; i < n; i++) crc = _mm_crc32_u8(crc, p[i]);
        return crc;
    }
#endif
    for (size_t i = 0; i < n; i++)
        crc = g_crc32c_tab[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}
static inline uint32_t crc32c_final(uint32_t crc) { return ~crc; }

/* Convenience: CRC32C over two regions treated as one contiguous stream. */
static inline uint32_t crc32c_chain(const uint8_t *a, size_t na,
                                    const uint8_t *b, size_t nb) {
    uint32_t crc = crc32c_begin();
    crc = crc32c_update(crc, a, na);
    crc = crc32c_update(crc, b, nb);
    return crc32c_final(crc);
}

/* ── xorshift64 PRNG (deterministic per-frame payload fill) ──────────────────
 * Seeded from the sequence number. Spectrally rich enough to properly exercise
 * the 100BASE-TX MLT-3 scrambler (unlike a constant fill). */
static inline uint64_t xorshift64(uint64_t *s) {
    uint64_t x = *s;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    *s = x;
    return x;
}
/* Fill buf[0..n) with pseudo-random bytes seeded from seq. Seed is offset by a
 * constant so seq=0 doesn't yield the xorshift fixed point (0 -> all zeros). */
static inline void fill_random_payload(uint8_t *buf, size_t n, uint64_t seq) {
    uint64_t s = seq ^ 0x9E3779B97F4A7C15ULL;   /* avoid zero-state */
    if (s == 0) s = 0xDEADBEEFCAFEBABEULL;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t r = xorshift64(&s);
        memcpy(buf + i, &r, 8);
    }
    if (i < n) {
        uint64_t r = xorshift64(&s);
        memcpy(buf + i, &r, n - i);
    }
}

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
    uint64_t nic_crc_errors;    /* from ethtool */
    uint64_t nic_crc_errors_prev;
    /* Per-slave ESC counters (accumulated from 8-bit wrap-around registers).
     * Written only by the RX thread. */
    uint64_t esc_crc[MAX_SLAVES][4];      /* ports 0-3 */
    uint64_t esc_lostlnk[MAX_SLAVES][4]; /* ports 0-3 */
    uint8_t  esc_crc_prev[MAX_SLAVES][4];
    uint8_t  esc_lostlnk_prev[MAX_SLAVES][4];
    uint32_t brd_wkc_expected;   /* = num_slaves */
    /* Cross-thread counters — atomic. Owner in comment. */
    _Atomic uint64_t frames_enqueued;  /* TX thread — send() accepted (ring)   */
    _Atomic uint64_t frames_received;  /* RX thread                            */
    _Atomic uint64_t frames_lost;      /* TX thread (retire)                   */
    _Atomic uint64_t seq_bad;          /* RX thread                            */
    _Atomic uint64_t tx_backpressure;  /* TX thread — EAGAIN/in-flight cap     */
    _Atomic uint64_t brd_wkc_mismatches;/* RX thread                           */
    _Atomic uint64_t kernel_drops;     /* main thread, from PACKET_STATISTICS  */
    _Atomic uint64_t payload_crc_errors;/* RX thread — CRC32C mismatch         */
    _Atomic uint64_t tx_ts_completions;/* errqueue thread — sw TX timestamps   */
    /* On-wire TX packet count (ethtool tx_packets). Owned by supervisor. */
    uint64_t tx_wire_packets;          /* cumulative, from ethtool             */
    uint64_t tx_wire_base;             /* baseline at start                    */
    int      tx_ts_supported;          /* 1 if sw TX timestamping active       */
} Stats;

/* g_stats is defined here (ahead of the SeqTrack helpers that update it). */
static Stats g_stats;

/* ── Outstanding-frame tracking window ──────────────────────────────────────
 * Ring of in-flight sequence numbers, shared between the TX and RX threads.
 *
 *   state transitions:     owner
 *     0 -> 1  (mark_sent)     TX
 *     1 -> 2  (mark_returned) RX   (atomic CAS)
 *     {1,2} -> 0 (retire)     TX
 *
 * Because the TX thread is the sole allocator (0->1) and sole reclaimer
 * (->0), and retires a seq before it can reuse that slot (seq+WINDOW),
 * the only cross-thread write is RX's 1->2, done with an atomic CAS.
 * seq_at_slot is published by TX with a release store on state and read
 * by RX after an acquire load, giving correct visibility without a lock. */
#define SEQ_WINDOW (1u << 20)         /* 1,048,576 — vastly exceeds in-flight */
#define SEQ_MASK   (SEQ_WINDOW - 1)
typedef struct {
    uint8_t  state[SEQ_WINDOW];       /* 0=free 1=outstanding 2=returned */
    uint64_t seq_at_slot[SEQ_WINDOW];
} SeqTrack;
static SeqTrack g_seq;

/* TX thread: before reusing a slot, retire its previous occupant.
 * The slot that seq will occupy was last used by (seq - SEQ_WINDOW). If that
 * previous occupant is still 'outstanding' (state 1), it never came back =
 * genuine loss. This inline retirement guarantees retirement always stays
 * exactly in lock-step with reuse, closing the aliasing window that a
 * lazy/batched retire left open. */
static inline void seq_mark_sent(uint64_t seq) {
    uint32_t slot = seq & SEQ_MASK;

    /* Retire the previous occupant of this slot, if any. */
    if (seq >= SEQ_WINDOW) {
        uint64_t prev = seq - SEQ_WINDOW;
        /* seq_at_slot still holds 'prev' here (we haven't overwritten yet). */
        uint8_t st = __atomic_load_n(&g_seq.state[slot], __ATOMIC_ACQUIRE);
        if (st == 1)
            atomic_fetch_add_explicit(&g_stats.frames_lost, 1,
                                      memory_order_relaxed);
        (void)prev;
    }

    /* Publish new occupant: write seq_at_slot, then release-store state=1. */
    g_seq.seq_at_slot[slot] = seq;
    __atomic_store_n(&g_seq.state[slot], 1, __ATOMIC_RELEASE);
}

/* RX thread: acquire-load state; if outstanding and seq matches, CAS 1->2. */
static inline void seq_mark_returned(uint64_t seq) {
    uint32_t slot = seq & SEQ_MASK;
    uint8_t st = __atomic_load_n(&g_seq.state[slot], __ATOMIC_ACQUIRE);
    if (st == 1 && g_seq.seq_at_slot[slot] == seq) {
        uint8_t expected = 1;
        if (__atomic_compare_exchange_n(&g_seq.state[slot], &expected, 2,
                                        0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
            return;                    /* successfully marked returned */
    }
    atomic_fetch_add_explicit(&g_stats.seq_bad, 1, memory_order_relaxed);
}

/* TX thread, shutdown only: sweep the whole window and count any slot still
 * 'outstanding' as lost. Inline retirement in seq_mark_sent handles the
 * steady state; this catches the final in-flight tail that will never be
 * reused because sending has stopped. Idempotent. */
static inline void seq_retire_final(uint64_t next_seq) {
    uint64_t lo = (next_seq >= SEQ_WINDOW) ? (next_seq - SEQ_WINDOW) : 0;
    for (uint64_t s = lo; s < next_seq; s++) {
        uint32_t slot = s & SEQ_MASK;
        if (g_seq.seq_at_slot[slot] == s) {
            uint8_t st = __atomic_load_n(&g_seq.state[slot], __ATOMIC_ACQUIRE);
            if (st == 1) {
                atomic_fetch_add_explicit(&g_stats.frames_lost, 1,
                                          memory_order_relaxed);
                __atomic_store_n(&g_seq.state[slot], 0, __ATOMIC_RELEASE);
            }
        }
    }
}

/* ── Globals ────────────────────────────────────────────────────────────── */
static volatile int g_running   = 1;  /* master run flag (RX honours this)   */
static volatile int g_tx_running = 1;  /* TX-only stop flag for drain barrier */
static _Atomic uint64_t g_tx_final_seq = 0; /* seq count at TX stop */
static int          g_verbose   = 0;
static int          g_num_slaves = 0;
static int          g_loopback   = 0;

/* ── Signal handler ─────────────────────────────────────────────────────── */
/* Stop TX first; the main thread performs the drain barrier and then clears
 * g_running to stop RX. A second signal forces immediate exit. */
static void sig_handler(int sig) {
    (void)sig;
    if (g_tx_running) g_tx_running = 0;   /* first: stop sending, drain */
    else              g_running    = 0;   /* second: hard stop          */
}

/* ── Timing helpers ─────────────────────────────────────────────────────── */
static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static inline void sleep_ns(uint64_t ns) {
    struct timespec ts = { .tv_sec = ns / 1000000000ULL,
                           .tv_nsec = ns % 1000000000ULL };
    nanosleep(&ts, NULL);
}

/* ── NIC MAC address ────────────────────────────────────────────────────── */
static int get_mac(int sock, const char *iface, uint8_t *mac) {
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);
    if (ioctl(sock, SIOCGIFHWADDR, &ifr) < 0) return -1;
    memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);
    return 0;
}

/* ── Interface index ────────────────────────────────────────────────────── */
static int get_ifindex(int sock, const char *iface) {
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);
    if (ioctl(sock, SIOCGIFINDEX, &ifr) < 0) return -1;
    return ifr.ifr_ifindex;
}

/* ── ethtool counter reader ─────────────────────────────────────────────────
 * Generic helper: sum all ethtool -S counters whose name contains ANY of the
 * given substrings (case-insensitive on the needles as written). Used for both
 * CRC/FCS error counting and TX-packet (on-wire) counting across chipsets.
 *
 * exclude, if non-NULL, is a substring that disqualifies a match (e.g. exclude
 * "err" when summing tx_packets so we don't pick up tx_*_errors). */
static uint64_t ethtool_sum_counters(const char *iface,
                                     const char *const *needles, int n_needles,
                                     const char *exclude) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return 0;

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);

    struct ethtool_drvinfo drvinfo;
    drvinfo.cmd = ETHTOOL_GDRVINFO;
    ifr.ifr_data = (void *)&drvinfo;
    if (ioctl(fd, SIOCETHTOOL, &ifr) < 0) { close(fd); return 0; }

    uint32_t n_stats = drvinfo.n_stats;
    if (n_stats == 0) { close(fd); return 0; }

    size_t sset_size = sizeof(struct ethtool_gstrings) + n_stats * ETH_GSTRING_LEN;
    struct ethtool_gstrings *gstrings = calloc(1, sset_size);
    if (!gstrings) { close(fd); return 0; }
    gstrings->cmd = ETHTOOL_GSTRINGS;
    gstrings->string_set = ETH_SS_STATS;
    gstrings->len = n_stats;
    ifr.ifr_data = (void *)gstrings;
    if (ioctl(fd, SIOCETHTOOL, &ifr) < 0) { free(gstrings); close(fd); return 0; }

    size_t stats_size = sizeof(struct ethtool_stats) + n_stats * sizeof(uint64_t);
    struct ethtool_stats *stats = calloc(1, stats_size);
    if (!stats) { free(gstrings); close(fd); return 0; }
    stats->cmd = ETHTOOL_GSTATS;
    stats->n_stats = n_stats;
    ifr.ifr_data = (void *)stats;
    if (ioctl(fd, SIOCETHTOOL, &ifr) < 0) { free(gstrings); free(stats); close(fd); return 0; }

    uint64_t total = 0;
    for (uint32_t i = 0; i < n_stats; i++) {
        char name[ETH_GSTRING_LEN + 1];
        memcpy(name, gstrings->data + i * ETH_GSTRING_LEN, ETH_GSTRING_LEN);
        name[ETH_GSTRING_LEN] = '\0';
        if (exclude && strstr(name, exclude)) continue;
        for (int k = 0; k < n_needles; k++) {
            if (strstr(name, needles[k])) { total += stats->data[i]; break; }
        }
    }

    free(gstrings);
    free(stats);
    close(fd);
    return total;
}

/* CRC/FCS error counter (RX). */
static uint64_t read_nic_crc_errors(const char *iface) {
    static const char *const needles[] = { "crc", "CRC", "fcs" };
    return ethtool_sum_counters(iface, needles, 3, NULL);
}

/* On-wire TX packet counter. Sums the driver's transmitted-packet counters,
 * excluding any *error* counters. Names vary: r8169/RTL8125 exposes
 * "tx_packets"; some drivers use "tx_unicast"+"tx_multicast"+"tx_broadcast".
 * We prefer an exact "tx_packets" if present, else fall back to the sum of the
 * per-cast counters. Returns 0 if none found (caller notes unavailability). */
static uint64_t read_nic_tx_packets(const char *iface) {
    /* First try the single authoritative counter. */
    static const char *const exact[] = { "tx_packets" };
    uint64_t v = ethtool_sum_counters(iface, exact, 1, "err");
    if (v) return v;
    /* Fall back to per-cast counters. */
    static const char *const cast[] = { "tx_unicast", "tx_multicast", "tx_broadcast" };
    return ethtool_sum_counters(iface, cast, 3, "err");
}

/* ── Build EtherCAT frame ───────────────────────────────────────────────── */
/*
 * Frame layout:
 *   [Ethernet header 14B]
 *   [EtherCAT header 2B]
 *   [Datagram 0: NOP, carrying fill payload, more=1]
 *   [Datagram 1: BRD reg 0x0000, 1 byte, more=1 if slaves>0]
 *   [Datagram 2..N: APRD slave 0..N-1, reg 0x0300, 8 bytes (CRC+lostlnk)]
 *     last APRD has more=0
 *
 * In loopback mode: just NOP datagram, no BRD/APRD.
 */
static int build_frame(uint8_t *buf, int buflen,
                       const uint8_t *src_mac, int num_slaves,
                       uint64_t seq, int loopback)
{
    memset(buf, 0, buflen);

    /* Ethernet header */
    memset(buf, 0xff, 6);              /* dst: broadcast */
    memcpy(buf + 6, src_mac, 6);
    buf[12] = 0x88; buf[13] = 0xA4;   /* EtherType */

    /* Calculate payload sizes */
    int aprd_bytes   = loopback ? 0 : (num_slaves * (ECAT_DG_OVERHEAD + 8));
    int brd_bytes    = loopback ? 0 : (ECAT_DG_OVERHEAD + 1);
    int overhead     = ETH_HDR_LEN + ECAT_HDR_LEN + ECAT_DG_OVERHEAD
                       + brd_bytes + aprd_bytes;
    int nop_payload  = buflen - overhead;
    if (nop_payload < 8) nop_payload = 8;  /* minimum sensible payload */

    /* Cursor */
    int pos = ETH_HDR_LEN + ECAT_HDR_LEN;  /* skip EtherCAT header for now */

    /* Datagram 0: NOP carrying payload */
    int has_more_after_nop = (!loopback && num_slaves > 0) ? 1 : 0;
    uint16_t nop_len_flags = htons((uint16_t)(nop_payload & 0x07FF)
                                   | (has_more_after_nop ? 0x8000 : 0));
    buf[pos++] = ECAT_CMD_NOP;
    buf[pos++] = ECAT_IDX_NOP;
    buf[pos++] = 0; buf[pos++] = 0; buf[pos++] = 0; buf[pos++] = 0; /* addr */
    memcpy(buf + pos, &nop_len_flags, 2); pos += 2;
    buf[pos++] = 0; buf[pos++] = 0;  /* IRQ */
    /* Payload: [seq(8)][crc32c(4)][random(N)].
     * Ensure the payload is at least large enough for the header. */
    if (nop_payload < PL_HDR_LEN) nop_payload = PL_HDR_LEN;
    {
        uint8_t *pl = buf + pos;
        /* seq (LE) */
        memcpy(pl + PL_SEQ_OFF, &seq, PL_SEQ_LEN);
        /* pseudo-random payload after the 12-byte header */
        int rnd_len = nop_payload - PL_DATA_OFF;
        if (rnd_len < 0) rnd_len = 0;
        fill_random_payload(pl + PL_DATA_OFF, rnd_len, seq);
        /* CRC32C over seq bytes ++ random payload bytes, as one logical
         * stream (they are separated on the wire by the 4-byte CRC hole). */
        uint32_t crc = crc32c_chain(pl + PL_SEQ_OFF, PL_SEQ_LEN,
                                    pl + PL_DATA_OFF, (size_t)rnd_len);
        memcpy(pl + PL_CRC_OFF, &crc, PL_CRC_LEN);
    }
    pos += nop_payload;
    buf[pos++] = 0; buf[pos++] = 0;  /* WKC */

    if (!loopback && num_slaves > 0) {
        /* Datagram 1: BRD reading register 0x0000 (type/revision, safe) */
        int has_more_after_brd = (num_slaves > 0) ? 1 : 0;
        uint16_t brd_lf = htons(1 | (has_more_after_brd ? 0x8000 : 0));
        buf[pos++] = ECAT_CMD_BRD;
        buf[pos++] = ECAT_IDX_BRD;
        buf[pos++] = 0; buf[pos++] = 0; buf[pos++] = 0; buf[pos++] = 0;
        memcpy(buf + pos, &brd_lf, 2); pos += 2;
        buf[pos++] = 0; buf[pos++] = 0;  /* IRQ */
        buf[pos++] = 0;                   /* 1 byte data */
        buf[pos++] = 0; buf[pos++] = 0;  /* WKC */

        /* Datagrams 2..N+1: APRD per slave, reading 8 bytes at 0x0300
         * (CRC counters 0x0300-0x0307 and lost-link 0x0310-0x0313 don't
         *  fit in one contiguous read; we read 0x0300 block, 8 bytes,
         *  which covers 0x0300-0x0307. Lost-link at 0x0310 needs a
         *  second pass — we send a second set of APRD datagrams for that.
         *  For simplicity here we read 16 bytes starting at 0x0300 which
         *  covers both blocks since they're within the same 256-byte page.) */
        for (int s = 0; s < num_slaves; s++) {
            int is_last = (s == num_slaves - 1);
            uint16_t aprd_lf = htons(16 | (is_last ? 0 : 0x8000));
            /* APRD address: upper 16 bits = auto-increment position (negated),
             * lower 16 bits = register offset.
             * Auto-increment: slave 0 sees address 0, slave 1 sees -1, etc.
             * We encode as: addr[31:16] = (uint16_t)(-(s)), addr[15:0] = 0x0300 */
            /* APRD address: auto-increment node (negated s), register 0x0300 */
            uint16_t node_le = (uint16_t)(-(int16_t)s);
            buf[pos++] = ECAT_CMD_APRD;
            buf[pos++] = (uint8_t)s;   /* idx = slave number for easy demux */
            /* addr in little-endian: node_addr (16-bit LE), then reg (16-bit LE) */
            buf[pos++] = (node_le) & 0xFF;
            buf[pos++] = (node_le >> 8) & 0xFF;
            buf[pos++] = 0x00;   /* reg 0x0300 low byte */
            buf[pos++] = 0x03;   /* reg 0x0300 high byte */
            memcpy(buf + pos, &aprd_lf, 2); pos += 2;
            buf[pos++] = 0; buf[pos++] = 0;  /* IRQ */
            memset(buf + pos, 0, 16);  /* 16 bytes data (zeroed, slaves fill in) */
            pos += 16;
            buf[pos++] = 0; buf[pos++] = 0;  /* WKC */
        }
    }

    /* EtherCAT header: total datagram length, type=1 */
    int ecat_payload_len = pos - ETH_HDR_LEN - ECAT_HDR_LEN;
    uint16_t ecat_hdr = htons((uint16_t)(ecat_payload_len & 0x07FF) | (0x1 << 12));
    memcpy(buf + ETH_HDR_LEN, &ecat_hdr, 2);

    return pos;  /* actual frame length */
}

/* ── Parse returned frame, update stats ─────────────────────────────────────
 * Returns the sequence number extracted from the NOP payload, or UINT64_MAX
 * if the frame could not be parsed / is not one of ours. */
static uint64_t parse_return_frame(const uint8_t *buf, int len,
                                   int num_slaves, int loopback) {
    if (len < ETH_HDR_LEN + ECAT_HDR_LEN) return UINT64_MAX;

    int pos = ETH_HDR_LEN + ECAT_HDR_LEN;

    /* Datagram 0: NOP — extract sequence number from payload */
    if (pos + ECAT_DG_HDR_LEN > len) return UINT64_MAX;
    uint16_t lf;
    memcpy(&lf, buf + pos + 6, 2);
    lf = ntohs(lf);
    uint16_t dg_len = lf & 0x07FF;
    pos += ECAT_DG_HDR_LEN;
    if (pos + dg_len + ECAT_DG_WKC_LEN > len) return UINT64_MAX;

    uint64_t ret_seq = UINT64_MAX;
    if (dg_len >= PL_HDR_LEN) {
        const uint8_t *pl = buf + pos;
        memcpy(&ret_seq, pl + PL_SEQ_OFF, PL_SEQ_LEN);

        /* Independent payload integrity check: recompute CRC32C over
         * (seq ++ random payload) and compare to the embedded field. This is
         * independent of the Ethernet FCS — it catches corruption that a slave
         * regenerating a valid FCS would otherwise mask. */
        uint32_t embedded;
        memcpy(&embedded, pl + PL_CRC_OFF, PL_CRC_LEN);
        int rnd_len = (int)dg_len - PL_DATA_OFF;
        if (rnd_len < 0) rnd_len = 0;
        uint32_t calc = crc32c_chain(pl + PL_SEQ_OFF, PL_SEQ_LEN,
                                     pl + PL_DATA_OFF, (size_t)rnd_len);
        if (calc != embedded)
            atomic_fetch_add_explicit(&g_stats.payload_crc_errors, 1,
                                      memory_order_relaxed);
    }
    pos += dg_len + ECAT_DG_WKC_LEN;

    if (loopback || num_slaves == 0) return ret_seq;

    /* Datagram 1: BRD — read WKC */
    if (pos + ECAT_DG_HDR_LEN > len) return ret_seq;
    memcpy(&lf, buf + pos + 6, 2); lf = ntohs(lf);
    dg_len = lf & 0x07FF;
    pos += ECAT_DG_HDR_LEN + dg_len;
    if (pos + ECAT_DG_WKC_LEN > len) return ret_seq;
    uint16_t brd_wkc;
    memcpy(&brd_wkc, buf + pos, 2);
    brd_wkc = ntohs(brd_wkc);
    pos += ECAT_DG_WKC_LEN;

    if (brd_wkc != (uint16_t)num_slaves) {
        atomic_fetch_add_explicit(&g_stats.brd_wkc_mismatches, 1,
                                  memory_order_relaxed);
    }

    /* Datagrams 2..N+1: APRD per slave */
    for (int s = 0; s < num_slaves && s < MAX_SLAVES; s++) {
        if (pos + ECAT_DG_HDR_LEN > len) break;
        memcpy(&lf, buf + pos + 6, 2); lf = ntohs(lf);
        dg_len = lf & 0x07FF;
        pos += ECAT_DG_HDR_LEN;
        if (pos + dg_len + ECAT_DG_WKC_LEN > len) break;

        uint16_t aprd_wkc;
        memcpy(&aprd_wkc, buf + pos + dg_len, 2);
        aprd_wkc = ntohs(aprd_wkc);

        if (aprd_wkc == 1 && dg_len >= 16) {
            /* Bytes 0-7: CRC error registers 0x0300-0x0307
             * Layout per ETG.1000.6:
             *   0x0300: Port0 invalid frame counter
             *   0x0301: Port0 RX error counter
             *   0x0302: Port1 invalid frame counter
             *   0x0303: Port1 RX error counter
             *   0x0304: Port2 ...
             *   0x0305: Port2 ...
             *   0x0306: Port3 ...
             *   0x0307: Port3 ...
             * Bytes 8-15 cover 0x0308-0x030F (forwarded error counters etc.)
             * Lost link at 0x0310-0x0313 NOT in this read (we'd need another APRD)
             * For now we track the 4 invalid-frame counters */
            for (int p = 0; p < 4; p++) {
                uint8_t cur = buf[pos + p * 2];  /* invalid frame counter for port p */
                uint8_t prev = g_stats.esc_crc_prev[s][p];
                uint8_t delta = (uint8_t)(cur - prev);  /* handles 8-bit wrap */
                g_stats.esc_crc[s][p] += delta;
                g_stats.esc_crc_prev[s][p] = cur;
            }
            /* Lost-link counters at offset 0x10 from 0x0300 = bytes [16..19]
             * but our read is only 16 bytes (0x0300-0x030F). To get 0x0310
             * we'd need a second APRD set. Mark as not available this cycle. */
        }

        pos += dg_len + ECAT_DG_WKC_LEN;
    }

    return ret_seq;
}

/* ── Print stats ────────────────────────────────────────────────────────── */
static void print_stats(FILE *csv, uint64_t elapsed_ns, uint64_t new_nic_crc) {
    double elapsed_s  = elapsed_ns / 1e9;

    /* Snapshot atomics once for a consistent-ish view. */
    uint64_t sent     = atomic_load_explicit(&g_stats.frames_enqueued, memory_order_relaxed);
    uint64_t rcvd     = atomic_load_explicit(&g_stats.frames_received, memory_order_relaxed);
    uint64_t lost     = atomic_load_explicit(&g_stats.frames_lost, memory_order_relaxed);
    uint64_t bad      = atomic_load_explicit(&g_stats.seq_bad, memory_order_relaxed);
    uint64_t backp    = atomic_load_explicit(&g_stats.tx_backpressure, memory_order_relaxed);
    uint64_t wkcmm    = atomic_load_explicit(&g_stats.brd_wkc_mismatches, memory_order_relaxed);
    uint64_t kdrops   = atomic_load_explicit(&g_stats.kernel_drops, memory_order_relaxed);
    uint64_t plcrc    = atomic_load_explicit(&g_stats.payload_crc_errors, memory_order_relaxed);
    uint64_t tsc      = atomic_load_explicit(&g_stats.tx_ts_completions, memory_order_relaxed);
    uint64_t wire     = g_stats.tx_wire_packets;

    uint64_t total_bits = sent * 12000ULL;

    /* NIC CRC delta */
    uint64_t nic_crc_delta = new_nic_crc - g_stats.nic_crc_errors_prev;
    g_stats.nic_crc_errors += nic_crc_delta;
    g_stats.nic_crc_errors_prev = new_nic_crc;

    /* Frames still legitimately in flight — informational, NOT loss.
     * Clamp at 0: on some interfaces (notably loopback) a frame can be
     * delivered to the raw socket more than once, making received+lost
     * momentarily exceed sent. That is a property of the test interface,
     * not a real negative in-flight count. */
    int64_t in_flight = (int64_t)sent - (int64_t)rcvd - (int64_t)lost;
    if (in_flight < 0) in_flight = 0;

    /* Effective throughput since last call. */
    static uint64_t prev_sent = 0, prev_ns = 0;
    double fps = 0.0;
    if (prev_ns && elapsed_ns > prev_ns)
        fps = (double)(sent - prev_sent) / ((elapsed_ns - prev_ns) / 1e9);
    prev_sent = sent; prev_ns = elapsed_ns;

    /* TX pipeline gaps. Guard against transient negatives. */
    int64_t gap_ring_drv  = (int64_t)sent - (int64_t)tsc;   /* socket/qdisc backlog */
    int64_t gap_drv_wire  = (int64_t)tsc  - (int64_t)wire;  /* driver->hw backlog   */
    if (gap_ring_drv < 0) gap_ring_drv = 0;
    if (gap_drv_wire < 0) gap_drv_wire = 0;

    printf("\n── EtherCAT BER Test ─────────────────────────────────────\n");
    printf("  Elapsed:        %.1f s\n", elapsed_s);
    printf("  ── TX pipeline ─────────────────────────────────────────\n");
    printf("  TX enqueued (ring):      %lu\n", sent);
    if (g_stats.tx_ts_supported)
        printf("  TX driver-xmit (sw ts):  %lu   (gap vs ring: %ld)\n",
               tsc, (long)gap_ring_drv);
    else
        printf("  TX driver-xmit (sw ts):  n/a (unsupported)\n");
    printf("  TX on wire (ethtool):    %lu   (gap vs driver: %ld)\n",
           wire, (long)gap_drv_wire);
    printf("  ── RX / integrity ──────────────────────────────────────\n");
    printf("  Frames rcvd:    %lu\n",    rcvd);
    printf("  Throughput:     %.0f fps  (%.1f Mbit/s)\n",
           fps, fps * 12000.0 / 1e6);
    printf("  In flight:      %ld  (queue lag, not loss)\n", (long)in_flight);
    printf("  Lost frames:    %lu  (aged out, never returned)\n", lost);
    printf("  Unknown/dup seq:%lu\n",    bad);
    printf("  Payload CRC err:%lu  %s\n", plcrc,
           plcrc ? "*** PAYLOAD CORRUPTION (FCS-independent) ***" : "(clean)");
    printf("  TX backpressure:%lu  (EAGAIN/in-flight cap, normal)\n", backp);
    printf("  Kernel RX drops:%lu  %s\n", kdrops,
           kdrops ? "*** DRAIN SATURATED — BER INVALID ***" : "(none, drain healthy)");
    printf("  BRD WKC mismatches: %lu\n", wkcmm);
    printf("  NIC CRC errors: %lu (cumulative)\n", g_stats.nic_crc_errors);
    printf("  Total bits:     %.3e\n", (double)total_bits);
    if (total_bits > 0) {
        printf("  Est. BER (CRC): %.2e\n",
               (double)g_stats.nic_crc_errors / (double)total_bits);
        printf("  Payload BER:    %.2e\n",
               (double)plcrc / (double)total_bits);
        printf("  Frame loss rate:%.2e\n",
               sent ? (double)lost / (double)sent : 0.0);
    }

    if (g_verbose && !g_loopback) {
        printf("  ── Per-slave ESC CRC counters ──────────────────────────\n");
        for (int s = 0; s < g_num_slaves && s < MAX_SLAVES; s++) {
            printf("  Slave %2d: P0=%lu P1=%lu P2=%lu P3=%lu\n", s,
                   g_stats.esc_crc[s][0], g_stats.esc_crc[s][1],
                   g_stats.esc_crc[s][2], g_stats.esc_crc[s][3]);
        }
    }
    printf("──────────────────────────────────────────────────────────\n");

    /* CSV row */
    if (csv) {
        fprintf(csv, "%.3f,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu",
                elapsed_s, sent, wire, tsc, rcvd, lost,
                g_stats.nic_crc_errors, plcrc, wkcmm, kdrops, bad);
        for (int s = 0; s < g_num_slaves && s < MAX_SLAVES; s++)
            fprintf(csv, ",%lu,%lu,%lu,%lu",
                    g_stats.esc_crc[s][0], g_stats.esc_crc[s][1],
                    g_stats.esc_crc[s][2], g_stats.esc_crc[s][3]);
        fprintf(csv, "\n");
        fflush(csv);
    }
}

/* ── CSV header ─────────────────────────────────────────────────────────── */
static void write_csv_header(FILE *csv, int num_slaves) {
    fprintf(csv, "elapsed_s,tx_enqueued,tx_wire,tx_driver_xmit,frames_rcvd,"
                 "frames_lost,nic_crc_errors,payload_crc_errors,"
                 "brd_wkc_mismatches,kernel_rx_drops,unknown_dup_seq");
    for (int s = 0; s < num_slaves; s++)
        fprintf(csv, ",slave%d_p0_crc,slave%d_p1_crc,slave%d_p2_crc,slave%d_p3_crc",
                s, s, s, s);
    fprintf(csv, "\n");
    fflush(csv);
}

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

typedef struct {
    int             sock;
    int             ifindex;
    uint8_t         src_mac[6];
    int             num_slaves;
    int             loopback;
    long            rate_hz;      /* 0 = saturate */
    int             tx_core;      /* CPU to pin TX thread (-1 = no pin) */
    int             rx_core;      /* CPU to pin RX thread (-1 = no pin) */
    int             errq_core;    /* CPU to pin errqueue thread (-1 = none) */
} ThreadCtx;

/* Pin the calling thread to a single CPU core. Returns 0 on success. */
static int pin_to_core(int core) {
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

static void *tx_thread(void *arg) {
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

        /* Bound outstanding frames. If RX hasn't caught up to within
         * MAX_INFLIGHT, pause briefly — this is genuine wire backpressure,
         * not a buffering artifact. Guarded so a transient rcvd>sent (possible
         * on interfaces that double-deliver, e.g. lo) can't underflow. */
        uint64_t sent_now = atomic_load_explicit(&g_stats.frames_enqueued, memory_order_relaxed);
        uint64_t rcvd_now = atomic_load_explicit(&g_stats.frames_received, memory_order_relaxed);
        if (sent_now > rcvd_now && (sent_now - rcvd_now) >= MAX_INFLIGHT) {
            atomic_fetch_add_explicit(&g_stats.tx_backpressure, 1, memory_order_relaxed);
            sleep_ns(10000);   /* 10us; loop re-checks g_tx_running */
            continue;
        }

        int frame_len = build_frame(tx_buf, sizeof(tx_buf), ctx->src_mac,
                                    ctx->loopback ? 0 : ctx->num_slaves,
                                    seq, ctx->loopback);

        int sent = send(ctx->sock, tx_buf, frame_len, 0);
        if (sent > 0) {
            seq_mark_sent(seq);   /* inline retirement of the reused slot */
            atomic_fetch_add_explicit(&g_stats.frames_enqueued, 1, memory_order_relaxed);
            seq++;
            if (interval_ns) next_send_ns += interval_ns;

        } else if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS) {
            /* TX ring full = bus saturation. This IS the intended backpressure.
             * Sleep a very short, bounded time and retry the SAME seq —
             * nothing is lost. A real (not yielding) sleep is used rather than
             * sched_yield() so we never live-lock the supervisor/RX threads on
             * a CPU-constrained host; at ~10us it is far shorter than the
             * ~120us it takes the wire to drain one 1500-byte frame, so the
             * TX ring is refilled the instant space appears. */
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

    /* Publish the final sequence number so the main thread can run the loss
     * sweep AFTER the post-TX drain barrier — NOT here. Sweeping now would
     * count every still-in-flight frame as lost, which is wrong: those frames
     * are still legitimately returning and RX is still draining them. */
    atomic_store_explicit(&g_tx_final_seq, seq, memory_order_release);
    return NULL;
}

/* ── RX thread ──────────────────────────────────────────────────────────── */
#define RX_BATCH 256
static void *rx_thread(void *arg) {
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

    static uint8_t bufs[RX_BATCH][MAX_FRAME];
    struct mmsghdr msgs[RX_BATCH];
    struct iovec   iov[RX_BATCH];

    memset(msgs, 0, sizeof(msgs));
    for (int i = 0; i < RX_BATCH; i++) {
        iov[i].iov_base = bufs[i];
        iov[i].iov_len  = MAX_FRAME;
        msgs[i].msg_hdr.msg_iov    = &iov[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
    }

    struct pollfd pfd = { .fd = ctx->sock, .events = POLLIN };

    /* Drain helper: pull every available batch, non-blocking. Returns frames
     * processed. */
    #define DRAIN_ONCE() do {                                                  \
        for (;;) {                                                             \
            int n = recvmmsg(ctx->sock, msgs, RX_BATCH, MSG_DONTWAIT, NULL);   \
            if (n <= 0) break;                                                 \
            for (int i = 0; i < n; i++) {                                      \
                atomic_fetch_add_explicit(&g_stats.frames_received, 1,         \
                                          memory_order_relaxed);              \
                uint64_t rseq = parse_return_frame(bufs[i], msgs[i].msg_len,   \
                                                   ctx->num_slaves,            \
                                                   ctx->loopback);             \
                if (rseq != UINT64_MAX) seq_mark_returned(rseq);              \
                iov[i].iov_len = MAX_FRAME;                                    \
            }                                                                 \
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
    return NULL;
}

/* Poll kernel RX-drop statistics (queue overflow). Accumulates into
 * g_stats.kernel_drops. Reading PACKET_STATISTICS resets the kernel's
 * internal counters, so each read returns the delta since the last. */
static void poll_kernel_drops(int sock) {
    struct tpacket_stats st;
    socklen_t len = sizeof(st);
    if (getsockopt(sock, SOL_PACKET, PACKET_STATISTICS, &st, &len) == 0) {
        if (st.tp_drops)
            atomic_fetch_add_explicit(&g_stats.kernel_drops, st.tp_drops,
                                      memory_order_relaxed);
    }
}

/* ── Software TX timestamping ────────────────────────────────────────────────
 * This NIC (RTL8125) has no PTP hardware clock, so only SOFTWARE TX
 * timestamping is available. The timestamp is taken in the kernel driver's
 * xmit path — strictly UPSTREAM of the wire — so this counter measures
 * "frames the driver pushed toward hardware", not true on-wire time. The
 * ethtool tx_packets counter remains the authoritative on-wire figure.
 *
 * Enable and return 1 on success, 0 if unsupported. */
static int enable_tx_timestamping(int sock) {
    int flags = SOF_TIMESTAMPING_TX_SOFTWARE   /* sw timestamp on TX */
              | SOF_TIMESTAMPING_SOFTWARE       /* report sw timestamps */
              | SOF_TIMESTAMPING_OPT_ID         /* per-frame id */
              | SOF_TIMESTAMPING_OPT_TSONLY;    /* don't copy frame back */
    if (setsockopt(sock, SOL_SOCKET, SO_TIMESTAMPING, &flags, sizeof(flags)) < 0)
        return 0;
    return 1;
}

/* ── Error-queue reader thread ──────────────────────────────────────────────
 * Drains MSG_ERRQUEUE, counting SCM_TIMESTAMPING completions — one per
 * transmitted frame. This is TX counter (c): "driver-xmit (sw ts)". Pinned to
 * a spare core; the completion rate equals the TX rate (~8k/s) so overhead is
 * negligible. */
static void *errq_thread(void *arg) {
    ThreadCtx *ctx = (ThreadCtx *)arg;
    pin_to_core(ctx->errq_core);

    char ctrl[512];
    struct msghdr msg;
    struct pollfd pfd = { .fd = ctx->sock, .events = POLLERR };

    while (g_running) {
        /* POLLERR signals the error queue has data. Short timeout so we
         * re-check g_running for prompt shutdown. */
        int pr = poll(&pfd, 1, 100);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr == 0) continue;

        /* Drain all pending completions. */
        for (;;) {
            memset(&msg, 0, sizeof(msg));
            msg.msg_control    = ctrl;
            msg.msg_controllen = sizeof(ctrl);
            int n = recvmsg(ctx->sock, &msg, MSG_ERRQUEUE | MSG_DONTWAIT);
            if (n < 0) break;   /* EAGAIN — queue empty */

            /* Count each SCM_TIMESTAMPING control message as one TX
             * completion. */
            for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg); cm;
                 cm = CMSG_NXTHDR(&msg, cm)) {
                if (cm->cmsg_level == SOL_SOCKET &&
                    cm->cmsg_type  == SCM_TIMESTAMPING) {
                    atomic_fetch_add_explicit(&g_stats.tx_ts_completions, 1,
                                              memory_order_relaxed);
                }
            }
        }
    }

    /* Final drain. */
    for (int i = 0; i < 1000; i++) {
        memset(&msg, 0, sizeof(msg));
        msg.msg_control = ctrl; msg.msg_controllen = sizeof(ctrl);
        int n = recvmsg(ctx->sock, &msg, MSG_ERRQUEUE | MSG_DONTWAIT);
        if (n < 0) break;
        for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg); cm;
             cm = CMSG_NXTHDR(&msg, cm)) {
            if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_TIMESTAMPING)
                atomic_fetch_add_explicit(&g_stats.tx_ts_completions, 1,
                                          memory_order_relaxed);
        }
    }
    return NULL;
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

    /* On-wire TX baseline (ethtool tx_packets). */
    g_stats.tx_wire_base = read_nic_tx_packets(iface);
    if (g_stats.tx_wire_base == 0)
        printf("NOTE: could not read ethtool tx_packets — wire count "
               "may be unavailable on this driver.\n");

    /* Open CSV */
    FILE *csv = fopen(csv_path, "w");
    if (!csv) { perror("fopen csv"); return 1; }
    write_csv_header(csv, num_slaves);

    /* Signal handlers */
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    /* Initial NIC CRC baseline */
    g_stats.nic_crc_errors_prev = read_nic_crc_errors(iface);

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
    memcpy(ctx.src_mac, src_mac, 6);
    ctx.num_slaves = num_slaves;
    ctx.loopback   = g_loopback;
    ctx.rate_hz    = rate_hz;
    ctx.tx_core    = tx_core;
    ctx.rx_core    = rx_core;
    ctx.errq_core  = errq_core;

    uint64_t start_ns    = now_ns();
    uint64_t last_stat_ns = start_ns;

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

        poll_kernel_drops(sock);
        g_stats.tx_wire_packets =
            read_nic_tx_packets(iface) - g_stats.tx_wire_base;

        if (now - last_stat_ns >= 5000000000ULL) {
            uint64_t nic_crc = read_nic_crc_errors(iface);
            print_stats(csv, now - start_ns, nic_crc);
            last_stat_ns = now;
        }
    }

    /* ── Drain barrier ──────────────────────────────────────────────────────
     * TX has stopped (or is about to). Wait for the TX thread to finish, then
     * let RX keep draining so every frame still legitimately in flight has
     * time to return. Only after this quiet period do we sweep the sequence
     * window for genuine losses. This is what prevents the ~60k in-flight
     * frames at shutdown from being miscounted as lost. */
    g_tx_running = 0;
    pthread_join(tx_tid, NULL);

    uint64_t tx_final = atomic_load_explicit(&g_tx_final_seq, memory_order_acquire);

    /* Wait until the in-flight count stops falling (RX has caught up) or a
     * hard timeout elapses. In-flight = sent - received - lost(=0 so far). */
    printf("\nTX stopped at seq %lu. Draining in-flight frames...\n", tx_final);
    uint64_t drain_deadline = now_ns() + 2000ULL * 1000000ULL;  /* 2s max */
    uint64_t prev_rcvd = 0, stable_ticks = 0;
    while (now_ns() < drain_deadline) {
        sleep_ns(50 * 1000000);   /* 50 ms */
        uint64_t rcvd = atomic_load_explicit(&g_stats.frames_received, memory_order_relaxed);
        uint64_t sent = atomic_load_explicit(&g_stats.frames_enqueued, memory_order_relaxed);
        if (rcvd >= sent) break;             /* everything came back */
        if (rcvd == prev_rcvd) {
            if (++stable_ticks >= 4) break;  /* 200ms with no new returns */
        } else {
            stable_ticks = 0;
        }
        prev_rcvd = rcvd;
    }

    /* Now stop RX (and errq) and sweep the window for anything that never
     * returned. */
    g_running = 0;
    pthread_join(rx_tid, NULL);
    if (have_errq) pthread_join(errq_tid, NULL);
    seq_retire_final(tx_final);   /* count genuine losses only */

    /* Final kernel-drop poll, wire count, and stats. */
    poll_kernel_drops(sock);
    g_stats.tx_wire_packets = read_nic_tx_packets(iface) - g_stats.tx_wire_base;
    uint64_t end_ns  = now_ns();
    uint64_t nic_crc = read_nic_crc_errors(iface);
    print_stats(csv, end_ns - start_ns, nic_crc);

    fclose(csv);
    close(sock);

    printf("\nDone. Results written to %s\n", csv_path);
    return 0;
}
