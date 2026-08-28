#ifndef ECAT_ESCREG_H
#define ECAT_ESCREG_H
/* ── ESC register access (READ-ONLY) ────────────────────────────────────────
 * Single-threaded request/response transaction layer for dumping EtherCAT
 * Slave Controller registers, built on the same little-endian wire helpers as
 * the BER tester (ETG.1000.4: all EtherCAT fields are little-endian).
 *
 * READ-ONLY BY CONSTRUCTION: this module implements APRD (auto-increment
 * physical read) only. There is deliberately NO APWR/write code path anywhere
 * in the binary, so no sequence of arguments can cause a write to the slave.
 * Reading PHY registers over MII management would require writing ESC
 * registers and is therefore NOT part of this module (see README/discussion).
 *
 * Addressing: position (auto-increment) addressing. The slave at chain
 * position N is addressed with ADP = -N (two's complement, little-endian);
 * each slave increments the field as the frame passes, so the target sees 0.
 * WKC == 1 on a datagram means exactly one slave processed it. */
#include "ecat_common.h"

#define ESC_MAX_DGRAMS   12     /* datagrams per frame (keeps frames small)  */
#define ESC_MAX_DATA     64     /* bytes read per datagram                   */

typedef struct {
    uint16_t addr;                    /* ESC register offset (ADO)           */
    uint16_t len;                     /* bytes to read (<= ESC_MAX_DATA)     */
    uint16_t wkc;                     /* working counter returned            */
    uint8_t  data[ESC_MAX_DATA];      /* register content                    */
    int      ok;                      /* 1 if this datagram returned         */
} EscRead;

typedef struct {
    int      sock;
    int      ifindex;
    uint8_t  src_mac[6];
    uint16_t position;                /* chain position of the target slave  */
    uint8_t  idx_seq;                 /* rolling datagram index for matching  */
    int      timeout_ms;
    uint64_t frames_sent;
    uint64_t frames_matched;
    uint64_t retries;
    uint64_t timeouts;
} EscCtx;

/* Open a raw socket bound to iface and fill ctx. Returns 0 on success. */
int  esc_open(EscCtx *ctx, const char *iface, uint16_t position, int timeout_ms);
void esc_close(EscCtx *ctx);

/* Read up to n register ranges in ONE frame (n <= ESC_MAX_DGRAMS). Each
 * reads[i].addr/.len are inputs; .data/.wkc/.ok are outputs. Returns 0 if a
 * matching response frame was received (check per-datagram .ok/.wkc), or -1
 * on timeout after retries. */
int  esc_read_multi(EscCtx *ctx, EscRead *reads, int n);

/* Convenience: read one range, chunking internally across frames as needed.
 * buf must hold len bytes. Returns the WKC of the last datagram, or -1. */
int  esc_read_range(EscCtx *ctx, uint16_t addr, uint16_t len, uint8_t *buf);

/* Exposed for unit tests: build a read frame; returns frame length.
 * Aborts if the datagram budget disagrees with what was written. */
int  esc_build_read_frame(uint8_t *buf, int buflen, const uint8_t *src_mac,
                          uint16_t position, uint8_t idx_base,
                          const EscRead *reads, int n);

/* Exposed for unit tests: parse a response frame into reads[]. Returns the
 * number of datagrams matched (by idx), or -1 if the frame is not ours. */
int  esc_parse_read_frame(const uint8_t *buf, int len, uint8_t idx_base,
                          EscRead *reads, int n);

#endif /* ECAT_ESCREG_H */
