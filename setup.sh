#!/bin/bash
# setup.sh — Prepare Linux interface for EtherCAT BER testing
# Run as root or with sudo.
# Usage: sudo ./setup.sh <interface>  e.g.  sudo ./setup.sh enp3s0

set -euo pipefail

IFACE="${1:-}"
if [[ -z "$IFACE" ]]; then
    echo "Usage: $0 <interface>"
    echo "Find your interface name with: ip link show"
    exit 1
fi

echo "=== EtherCAT BER test setup for interface: $IFACE ==="

# 1. Flush any IP addresses (we use raw sockets, not IP)
echo "[1] Flushing IP addresses from $IFACE..."
ip addr flush dev "$IFACE" 2>/dev/null || true

# 2. Bring up in promiscuous mode
echo "[2] Setting $IFACE up (promisc)..."
ip link set "$IFACE" promisc on
ip link set "$IFACE" up

# 3. Force 100BASE-TX (prevents gigabit negotiation, matching EtherCAT slaves)
echo "[3] Forcing 100BASE-TX full-duplex on $IFACE..."
ethtool -s "$IFACE" speed 100 duplex full autoneg off 2>/dev/null || {
    echo "    WARNING: ethtool speed force failed — your NIC may not support"
    echo "    forced speed. This is OK if the slave forces 100Mbps anyway."
    echo "    For the loopback cable test, add a 100BASE-TX-only SFP or"
    echo "    ensure autoneg settles at 100Mbps."
}

# 4. Disable NIC offloads that can interfere with raw frame handling
echo "[4] Disabling NIC offloads..."
for feature in rx tx gso gro tso ufo lro; do
    ethtool -K "$IFACE" "$feature" off 2>/dev/null || true
done

# 4a. Enable reception of ALL frames including bad-FCS, and keep the FCS
#     trailer, so corrupt frames (e.g. from a mid-frame cable fault) are
#     DELIVERED and can be counted rather than silently dropped by the NIC.
#     rx-fcs: frames arrive with their 4-byte FCS trailer (verified in sw).
#     rx-all: frames failing the FCS check are still delivered.
echo "[4a] Enabling rx-all + rx-fcs (deliver corrupt frames)..."
if ethtool -K "$IFACE" rx-all on rx-fcs on 2>/dev/null; then
    echo "    rx-all=on rx-fcs=on"
else
    echo "    WARNING: this NIC does not support rx-all/rx-fcs; corrupt frames"
    echo "    will be dropped by hardware and counted only via ethtool error"
    echo "    counters, not delivered to the tool."
fi

# 4b. Raise txqueuelen so the qdisc can buffer the whole credit window during a
#     brief link outage (frames are held, not dropped, until the link returns).
#     Run qdisc_buffer_test.sh to confirm this NIC buffers rather than drops.
echo "[4b] Raising txqueuelen to 16000 (qdisc buffer depth for outages)..."
ip link set "$IFACE" txqueuelen 16000 2>/dev/null && \
    echo "    txqueuelen=$(cat /sys/class/net/$IFACE/tx_queue_len)" || \
    echo "    WARNING: could not set txqueuelen"

# 4b. Raise kernel socket buffer ceilings so the tool's 64MB RX buffer request
#     is honoured (the drain thread relies on this headroom to never overflow).
echo "[4b] Raising kernel socket buffer limits..."
sysctl -w net.core.rmem_max=134217728 >/dev/null    # 128 MB
sysctl -w net.core.wmem_max=33554432  >/dev/null    # 32 MB
sysctl -w net.core.netdev_max_backlog=250000 >/dev/null

# 4c. Maximise the NIC RX ring so bursts are absorbed in hardware before the
#     kernel queue is even involved. Ask for the hardware maximum.
echo "[4c] Maximising NIC RX/TX ring sizes..."
MAXRX=$(ethtool -g "$IFACE" 2>/dev/null | awk '/^RX:/{print $2; exit}')
MAXTX=$(ethtool -g "$IFACE" 2>/dev/null | awk '/^TX:/{print $2; exit}')
if [[ -n "${MAXRX:-}" && -n "${MAXTX:-}" ]]; then
    ethtool -G "$IFACE" rx "$MAXRX" tx "$MAXTX" 2>/dev/null && \
        echo "    RX ring -> $MAXRX, TX ring -> $MAXTX" || \
        echo "    (ring resize not supported by driver — OK)"
else
    echo "    (driver does not report ring sizes — skipping)"
fi

# 4d. Steer this NIC's IRQs to core 3 (0x8), away from the pinned TX (core 1)
#     and RX (core 2) worker threads, so IRQ handling doesn't contend.
echo "[4d] Steering NIC IRQs to core 3 (best effort)..."
for irq in $(grep "$IFACE" /proc/interrupts | awk -F: '{print $1}' | tr -d ' '); do
    echo 8 > "/proc/irq/$irq/smp_affinity" 2>/dev/null || true
done

# 5. Show current ethtool stats baseline
echo "[5] NIC stats baseline:"
ethtool -S "$IFACE" 2>/dev/null | grep -iE "crc|error|drop|miss|lost" || \
    echo "    (no matching stats — check 'ethtool -S $IFACE' manually)"

# 6. Show link status
echo "[6] Link status:"
ethtool "$IFACE" 2>/dev/null | grep -E "Speed|Duplex|Link"

echo ""
echo "=== Setup complete. ==="
echo ""
echo "Build the tester:  make"
echo ""
echo "--- Test scenarios ---"
echo ""
echo "Loopback cable (no slaves, basic sanity):"
echo "  sudo ./ecat_ber -i $IFACE -l -d 30 -o loopback.csv"
echo ""
echo "4-slave chain, saturate, run until Ctrl-C:"
echo "  sudo ./ecat_ber -i $IFACE -s 4 -o chain_4slave.csv"
echo ""
echo "4-slave chain, 1000 Hz rate, 3600 seconds (1 hour):"
echo "  sudo ./ecat_ber -i $IFACE -s 4 -r 1000 -d 3600 -o chain_1hr.csv"
echo ""
echo "4-slave chain, saturate, verbose ESC register output:"
echo "  sudo ./ecat_ber -i $IFACE -s 4 -v -o chain_verbose.csv"
echo ""
echo "--- BER estimation ---"
echo "At 100BASE-TX saturation (~8100 fps, ~1500 byte frames):"
echo "  Bits/frame:    ~12000"
echo "  Bits/second:   ~97,200,000"
echo "  For 10^13 bits: ~115 days at saturation"
echo "  For 10^13 bits: ~28 hours at rate_hz=10000 (if cable allows)"
echo ""
echo "To check NIC CRC counters manually at any time:"
echo "  ethtool -S $IFACE | grep -i crc"
echo ""
echo "To restore normal NIC settings after testing:"
echo "  ethtool -s $IFACE autoneg on"
echo "  ethtool -K $IFACE rx on tx on gso on gro on"
echo "  ethtool -K $IFACE rx-all off rx-fcs off"
