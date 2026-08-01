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

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <net/ethernet.h>
#include <netpacket/packet.h>
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
    uint64_t frames_sent;
    uint64_t frames_lost;       /* no reply within timeout */
    uint64_t frames_received;
    uint64_t nic_crc_errors;    /* from ethtool */
    uint64_t nic_crc_errors_prev;
    /* Per-slave ESC counters (accumulated from 8-bit wrap-around registers) */
    uint64_t esc_crc[MAX_SLAVES][4];      /* ports 0-3 */
    uint64_t esc_lostlnk[MAX_SLAVES][4]; /* ports 0-3 */
    uint8_t  esc_crc_prev[MAX_SLAVES][4];
    uint8_t  esc_lostlnk_prev[MAX_SLAVES][4];
    uint32_t brd_wkc_expected;   /* = num_slaves */
    uint32_t brd_wkc_mismatches; /* BRD returned wrong WKC */
    uint64_t seq_reordered;      /* frames returned out of order */
    uint64_t seq_bad;            /* returned seq we never sent / duplicate */
    uint64_t tx_backpressure;    /* EAGAIN on send — normal, not a loss */
} Stats;

/* g_stats is defined here (ahead of the SeqTrack helpers that update it). */
static Stats g_stats;

/* ── Outstanding-frame tracking window ──────────────────────────────────────
 * Ring of in-flight sequence numbers. A frame is "outstanding" from the moment
 * it is sent until its seq comes back on the wire. If a seq is still
 * outstanding when it falls off the back of the window (older than WINDOW
 * frames), it is counted as genuinely lost — this distinguishes true loss
 * from mere RX-queue lag. */
#define SEQ_WINDOW 65536              /* power of 2, must exceed max in-flight */
#define SEQ_MASK   (SEQ_WINDOW - 1)
typedef struct {
    uint8_t  state[SEQ_WINDOW];   /* 0=free, 1=outstanding, 2=returned */
    uint64_t seq_at_slot[SEQ_WINDOW]; /* which seq currently occupies the slot */
    uint64_t oldest_unretired;    /* lowest seq not yet reconciled */
} SeqTrack;
static SeqTrack g_seq;

static inline void seq_mark_sent(uint64_t seq) {
    uint32_t slot = seq & SEQ_MASK;
    g_seq.state[slot]       = 1;   /* outstanding */
    g_seq.seq_at_slot[slot] = seq;
}

static inline void seq_mark_returned(uint64_t seq) {
    uint32_t slot = seq & SEQ_MASK;
    if (g_seq.seq_at_slot[slot] == seq && g_seq.state[slot] == 1) {
        g_seq.state[slot] = 2;     /* returned OK */
    } else {
        g_stats.seq_bad++;         /* seq we don't recognise, or duplicate */
    }
}

/* Retire all sequences that have aged out of the window (older than
 * next_seq - SEQ_WINDOW). Any still 'outstanding' at retirement = lost. */
static inline void seq_retire(uint64_t next_seq) {
    if (next_seq < SEQ_WINDOW) return;
    uint64_t retire_upto = next_seq - SEQ_WINDOW;
    while (g_seq.oldest_unretired < retire_upto) {
        uint32_t slot = g_seq.oldest_unretired & SEQ_MASK;
        if (g_seq.seq_at_slot[slot] == g_seq.oldest_unretired) {
            if (g_seq.state[slot] == 1)
                g_stats.frames_lost++;   /* aged out still outstanding */
        }
        g_seq.state[slot] = 0;           /* free the slot */
        g_seq.oldest_unretired++;
    }
}

/* ── Globals ────────────────────────────────────────────────────────────── */
static volatile int g_running = 1;
static int          g_verbose  = 0;
static int          g_num_slaves = 0;
static int          g_loopback   = 0;

/* ── Signal handler ─────────────────────────────────────────────────────── */
static void sig_handler(int sig) { (void)sig; g_running = 0; }

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

/* ── ethtool CRC / error counter read ──────────────────────────────────── */
/*
 * RTL8125 exposes "rx_crc_errors" in ethtool -S.
 * RTL8111 exposes "rx_crc_errors" similarly.
 * We iterate gstrings to find it by name — this works across chipsets.
 */
static uint64_t read_nic_crc_errors(const char *iface) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return 0;

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);

    /* Get number of stats */
    struct ethtool_drvinfo drvinfo;
    drvinfo.cmd = ETHTOOL_GDRVINFO;
    ifr.ifr_data = (void *)&drvinfo;
    if (ioctl(fd, SIOCETHTOOL, &ifr) < 0) { close(fd); return 0; }

    uint32_t n_stats = drvinfo.n_stats;
    if (n_stats == 0) { close(fd); return 0; }

    /* Get stat strings */
    size_t sset_size = sizeof(struct ethtool_gstrings) + n_stats * ETH_GSTRING_LEN;
    struct ethtool_gstrings *gstrings = calloc(1, sset_size);
    if (!gstrings) { close(fd); return 0; }
    gstrings->cmd = ETHTOOL_GSTRINGS;
    gstrings->string_set = ETH_SS_STATS;
    gstrings->len = n_stats;
    ifr.ifr_data = (void *)gstrings;
    if (ioctl(fd, SIOCETHTOOL, &ifr) < 0) { free(gstrings); close(fd); return 0; }

    /* Get stat values */
    size_t stats_size = sizeof(struct ethtool_stats) + n_stats * sizeof(uint64_t);
    struct ethtool_stats *stats = calloc(1, stats_size);
    if (!stats) { free(gstrings); close(fd); return 0; }
    stats->cmd = ETHTOOL_GSTATS;
    stats->n_stats = n_stats;
    ifr.ifr_data = (void *)stats;
    if (ioctl(fd, SIOCETHTOOL, &ifr) < 0) { free(gstrings); free(stats); close(fd); return 0; }

    uint64_t crc_total = 0;
    for (uint32_t i = 0; i < n_stats; i++) {
        char name[ETH_GSTRING_LEN + 1];
        memcpy(name, gstrings->data + i * ETH_GSTRING_LEN, ETH_GSTRING_LEN);
        name[ETH_GSTRING_LEN] = '\0';
        /* Match common CRC counter names across Realtek and Intel drivers */
        if (strstr(name, "crc") || strstr(name, "CRC") || strstr(name, "fcs")) {
            crc_total += stats->data[i];
        }
    }

    free(gstrings);
    free(stats);
    close(fd);
    return crc_total;
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
    /* Fill payload with sequence number + pattern */
    memcpy(buf + pos, &seq, sizeof(seq));
    memset(buf + pos + sizeof(seq), 0xA5, nop_payload - sizeof(seq));
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
    if (dg_len >= 8) memcpy(&ret_seq, buf + pos, 8);
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
        g_stats.brd_wkc_mismatches++;
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
    uint64_t total_bits = g_stats.frames_sent * 12000ULL;

    /* NIC CRC delta */
    uint64_t nic_crc_delta = new_nic_crc - g_stats.nic_crc_errors_prev;
    g_stats.nic_crc_errors += nic_crc_delta;
    g_stats.nic_crc_errors_prev = new_nic_crc;

    /* Frames still legitimately in flight (sent, not yet returned, not yet
     * aged out). This is the queue-lag figure — informational, NOT loss. */
    uint64_t in_flight = g_stats.frames_sent
                       - g_stats.frames_received
                       - g_stats.frames_lost;

    printf("\n── EtherCAT BER Test ─────────────────────────────────────\n");
    printf("  Elapsed:        %.1f s\n", elapsed_s);
    printf("  Frames sent:    %lu\n",    g_stats.frames_sent);
    printf("  Frames rcvd:    %lu\n",    g_stats.frames_received);
    printf("  In flight:      %ld  (queue lag, not loss)\n", (int64_t)in_flight);
    printf("  Lost frames:    %lu  (aged out, never returned)\n",
           g_stats.frames_lost);
    printf("  Unknown/dup seq:%lu\n",    g_stats.seq_bad);
    printf("  TX backpressure:%lu  (EAGAIN, normal)\n", g_stats.tx_backpressure);
    printf("  BRD WKC mismatches: %u\n", g_stats.brd_wkc_mismatches);
    printf("  NIC CRC errors: %lu (cumulative)\n", g_stats.nic_crc_errors);
    printf("  Total bits:     %.3e\n", (double)total_bits);
    if (total_bits > 0) {
        /* Report BER against the direct physical error signal (NIC CRC),
         * and separately a frame-loss rate. CRC is the more sensitive
         * BER estimator; a lost frame usually implies >=1 bit error too. */
        printf("  Est. BER (CRC): %.2e\n",
               (double)g_stats.nic_crc_errors / (double)total_bits);
        printf("  Frame loss rate:%.2e\n",
               g_stats.frames_sent ?
               (double)g_stats.frames_lost / (double)g_stats.frames_sent : 0.0);
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
        fprintf(csv, "%.3f,%lu,%lu,%lu,%lu,%u",
                elapsed_s, g_stats.frames_sent, g_stats.frames_received,
                g_stats.frames_lost, g_stats.nic_crc_errors,
                g_stats.brd_wkc_mismatches);
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
    fprintf(csv, "elapsed_s,frames_sent,frames_rcvd,frames_lost,"
                 "nic_crc_errors,brd_wkc_mismatches");
    for (int s = 0; s < num_slaves; s++)
        fprintf(csv, ",slave%d_p0_crc,slave%d_p1_crc,slave%d_p2_crc,slave%d_p3_crc",
                s, s, s, s);
    fprintf(csv, "\n");
    fflush(csv);
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

    /* Set socket to non-blocking for receive */
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    /* Increase socket buffer */
    int bufsize = 4 * 1024 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));

    /* Open CSV */
    FILE *csv = fopen(csv_path, "w");
    if (!csv) { perror("fopen csv"); return 1; }
    write_csv_header(csv, num_slaves);

    /* Signal handlers */
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    /* Initial NIC CRC baseline */
    g_stats.nic_crc_errors_prev = read_nic_crc_errors(iface);

    /* Timing */
    uint64_t interval_ns = (rate_hz > 0) ? (1000000000ULL / rate_hz) : 0;
    uint64_t start_ns    = now_ns();
    uint64_t last_stat_ns = start_ns;
    uint64_t next_send_ns = start_ns;
    uint64_t seq          = 0;

    uint8_t tx_buf[MAX_FRAME];
    uint8_t rx_buf[MAX_FRAME];

    printf("Running... (Ctrl-C to stop)\n");

    while (g_running) {
        uint64_t now = now_ns();

        /* Check duration */
        if (duration_s > 0 && (now - start_ns) >= (uint64_t)duration_s * 1000000000ULL)
            break;

        /* Send a frame if due */
        if (interval_ns == 0 || now >= next_send_ns) {
            int frame_len = build_frame(tx_buf, sizeof(tx_buf),
                                        src_mac, g_loopback ? 0 : num_slaves,
                                        seq, g_loopback);
            int sent = send(sock, tx_buf, frame_len, 0);
            if (sent > 0) {
                seq_mark_sent(seq);
                g_stats.frames_sent++;
                seq++;
                if (interval_ns > 0)
                    next_send_ns += interval_ns;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS) {
                /* TX ring full — normal backpressure in saturate mode.
                 * Do NOT advance seq or next_send_ns; retry same frame next
                 * iteration after draining RX. This is not an error and is
                 * not counted as a loss. */
                g_stats.tx_backpressure++;
            } else {
                /* A genuine, unexpected send error. */
                static uint64_t send_errors = 0;
                if (send_errors < 5) {
                    fprintf(stderr, "send() failed (frame_len=%d): %s\n",
                            frame_len, strerror(errno));
                    if (errno == EMSGSIZE)
                        fprintf(stderr,
                            "  -> frame too large for interface MTU.\n");
                }
                send_errors++;
                if (send_errors == 5)
                    fprintf(stderr, "(further send errors suppressed)\n");
                if (interval_ns > 0)
                    next_send_ns += interval_ns;
            }
        }

        /* Drain the ENTIRE receive queue this iteration, not just one frame.
         * This is what prevents received frames piling up in the socket
         * buffer and being misread as losses. */
        for (;;) {
            int n = recv(sock, rx_buf, sizeof(rx_buf), 0);
            if (n <= 0) break;   /* EAGAIN when queue empty -> done draining */
            g_stats.frames_received++;
            uint64_t rseq = parse_return_frame(rx_buf, n, num_slaves, g_loopback);
            if (rseq != UINT64_MAX)
                seq_mark_returned(rseq);
        }

        /* Retire aged-out sequences; genuinely-lost frames are counted here. */
        seq_retire(seq);

        /* Periodic stats print (every 5 seconds) */
        if (now - last_stat_ns >= 5000000000ULL) {
            uint64_t nic_crc = read_nic_crc_errors(iface);
            print_stats(csv, now - start_ns, nic_crc);
            last_stat_ns = now;
        }
    }

    /* Drain any final in-flight frames and retire everything before summary. */
    for (int flush = 0; flush < 1000; flush++) {
        int n = recv(sock, rx_buf, sizeof(rx_buf), 0);
        if (n <= 0) { sleep_ns(1000000); continue; }
        g_stats.frames_received++;
        uint64_t rseq = parse_return_frame(rx_buf, n, num_slaves, g_loopback);
        if (rseq != UINT64_MAX)
            seq_mark_returned(rseq);
    }
    seq_retire(seq + SEQ_WINDOW);  /* force-retire all remaining slots */

    /* Final stats */
    uint64_t end_ns  = now_ns();
    uint64_t nic_crc = read_nic_crc_errors(iface);
    print_stats(csv, end_ns - start_ns, nic_crc);

    fclose(csv);
    close(sock);

    printf("\nDone. Results written to %s\n", csv_path);
    return 0;
}
