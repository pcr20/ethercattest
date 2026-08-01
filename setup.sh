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
for feature in rx tx gso gro tso ufo; do
    ethtool -K "$IFACE" "$feature" off 2>/dev/null || true
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
