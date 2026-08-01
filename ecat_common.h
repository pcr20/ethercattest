#ifndef ECAT_COMMON_H
#define ECAT_COMMON_H
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
 *   Bits per frame = (frame+FCS)×8, measured at runtime (1518×8 = 12144 for a
 *   full-size frame). To accumulate 10^13 bits: ~10^13/12144 ≈ 8.2×10^8
 *   frames ≈ 28 hours at saturation. Plan accordingly.
 *
 * Loopback cable wiring (100BASE-TX only, prevents 1000BASE-T negotiation):
 *   Pin 1 (TX+) → Pin 3 (RX+)
 *   Pin 2 (TX-) → Pin 6 (RX-)
 *   Leave pins 4,5,7,8 unconnected.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE   /* also passed as -D_GNU_SOURCE by the Makefile so it
                         precedes ANY libc header in every TU (incl. tests) */
#endif
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
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
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

/* ── EtherCAT wire byte order: LITTLE-ENDIAN ────────────────────────────────
 * Per ETG.1000.4, ALL EtherCAT fields are little-endian on the wire: the frame
 * header (11-bit length + 4-bit type), every datagram len/flags field
 * (including the 'more' bit = bit 15 of the LE value), and the WKC. Using
 * htons/ntohs (big-endian) here is a protocol violation that a real ESC
 * detects instantly: it reads a byte-swapped frame header (type=13, garbage
 * length), declares the frame invalid, and cuts it off mid-forwarding —
 * returning ~16-byte runts for every frame. (A passive loopback plug never
 * exposes this because nothing on the wire interprets the bytes.) */
static inline void le16put(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}
static inline uint16_t le16get(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
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


/* ── Shared run-state globals (defined in stats.c) ────────────────────────── */
extern volatile sig_atomic_t g_running;
extern volatile sig_atomic_t g_tx_running;
extern uint64_t     g_start_ns;
extern int          g_verbose;
extern int          g_loopback;
extern int          g_num_slaves;

#endif /* ECAT_COMMON_H */
