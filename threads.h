#ifndef ECAT_THREADS_H
#define ECAT_THREADS_H
/* All worker threads: TX (never halts), RX (frame accounting + detectors),
 * error-queue (sw TX timestamps), wire-truth sampler (TxOk/TxER/qdisc-drop),
 * POLLPRI carrier monitor, and netlink cross-check. */
#include "ecat_common.h"

typedef struct {
    int             sock;
    int             ifindex;
    const char     *iface_name;   /* for sysfs/netlink lookups */
    uint8_t         src_mac[6];
    int             num_slaves;
    int             loopback;
    int             rx_fcs_on;    /* rx-fcs enabled: frames carry 4B FCS trailer */
    int             rx_all_on;    /* rx-all enabled: bad-FCS frames delivered   */
    long            rate_hz;      /* 0 = saturate */
    int             tx_core;      /* CPU to pin TX thread (-1 = no pin) */
    int             rx_core;      /* CPU to pin RX thread (-1 = no pin) */
    int             errq_core;    /* CPU to pin errqueue thread (-1 = none) */
} ThreadCtx;

void *tx_thread(void *arg);
void *rx_thread(void *arg);
void *errq_thread(void *arg);
void *sampler_thread(void *arg);
void *carrier_thread(void *arg);
void *netlink_thread(void *arg);
int   pin_to_core(int core);

#endif /* ECAT_THREADS_H */
