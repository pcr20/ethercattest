#!/bin/bash
# iperf3_reference.sh
#
# NOTE: iperf3 operates at the IP/UDP layer and cannot send EtherCAT frames.
# It is useful as a REFERENCE / COMPARISON measurement alongside the EtherCAT
# BER test — you run ecat_ber on one interface and iperf3 on a separate
# normal Ethernet link to cross-check whether errors are cable-related
# (affect both) or EtherCAT-specific.
#
# If you want to saturate the EtherCAT chain with payload data,
# use ecat_ber itself at rate_hz=0 (saturate mode).
#
# ────────────────────────────────────────────────────────────────
# IPERF3 SETUP (on a separate normal Ethernet segment for reference)
# ────────────────────────────────────────────────────────────────
#
# Server side (receiving end):
#   iperf3 -s
#
# Client side — UDP saturation at 100Mbps:
#   iperf3 -c <server_ip> -u -b 100M -t 3600 -l 1400 --get-server-output
#
#   -u          UDP mode
#   -b 100M     target bitrate (matches 100BASE-TX)
#   -t 3600     run for 1 hour
#   -l 1400     packet length (close to EtherCAT max payload)
#
# For very long BER runs (days):
#   iperf3 -c <server_ip> -u -b 95M -t 86400 -l 1400 --logfile iperf_24hr.log
#
# ────────────────────────────────────────────────────────────────
# MONITORING CRC ERRORS DURING IPERF3 RUN
# ────────────────────────────────────────────────────────────────

IFACE="${1:-enp3s0}"
INTERVAL="${2:-10}"

echo "Monitoring CRC errors on $IFACE every ${INTERVAL}s"
echo "Press Ctrl-C to stop"
echo ""
echo "Timestamp,rx_crc_errors,rx_missed_errors,rx_frame_errors"

prev_crc=0
while true; do
    crc=$(ethtool -S "$IFACE" 2>/dev/null \
          | awk '/crc|fcs/{sum+=$2} END{print sum+0}')
    ts=$(date +%H:%M:%S)
    delta=$((crc - prev_crc))
    echo "$ts,$crc (delta: +$delta)"
    prev_crc=$crc
    sleep "$INTERVAL"
done
