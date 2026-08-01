#ifndef ECAT_NIC_H
#define ECAT_NIC_H
/* Kernel/driver interface: MAC & ifindex lookup, ethtool tally counters
 * (TxOk/TxER wire truth), sysfs counters, RX-drop polling. */
#include "ecat_common.h"

int get_mac(int sock, const char *iface, uint8_t *mac);
int get_ifindex(int sock, const char *iface);
uint64_t read_nic_tx_packets(const char *iface);   /* hardware TxOk */
uint64_t read_nic_tx_errors(const char *iface);    /* hardware TxER */
uint64_t read_sysfs_u64(const char *iface, const char *name, int *ok);
void poll_kernel_drops(int sock);                  /* PACKET_STATISTICS */
int ethtool_feature_on(const char *iface, const char *feat, int *known);
int enable_tx_timestamping(int sock);              /* sw TX ts; 0 if unsupported */

#endif /* ECAT_NIC_H */
