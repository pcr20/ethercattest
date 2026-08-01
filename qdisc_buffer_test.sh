#!/bin/bash
# ── Issue 2 diagnostic: does the NIC/qdisc BUFFER or DROP frames during a
# ── carrier-down, and does a large txqueuelen let buffered frames survive?
#
# Run this, then short the pins (or unplug) briefly during the 20s window.
# It reports whether tx_dropped climbed (drop) or stayed flat (buffer), and
# whether frames resumed cleanly on recovery.
#
# Usage: sudo ./qdisc_buffer_test.sh <iface>
set -u
IFACE="${1:-enp2s0}"
QLEN=16000

echo "=== qdisc buffering diagnostic on $IFACE ==="
echo

# 1. Raise txqueuelen so the qdisc can hold the whole credit window.
echo "[1] Setting txqueuelen=$QLEN (was $(cat /sys/class/net/$IFACE/tx_queue_len))"
ip link set "$IFACE" txqueuelen $QLEN
echo "    now: $(cat /sys/class/net/$IFACE/tx_queue_len)"
echo

# 2. Identify the tx_dropped / tx_errors counters. Try ip -s link (kernel view)
#    and ethtool -S (driver view) — report both, since drivers vary.
read_dropped() {
    # kernel netdev tx_dropped from /sys statistics
    cat "/sys/class/net/$IFACE/statistics/tx_dropped" 2>/dev/null || echo 0
}
read_carrier_changes() {
    cat "/sys/class/net/$IFACE/carrier_changes" 2>/dev/null || echo 0
}
read_txpkts() {
    cat "/sys/class/net/$IFACE/statistics/tx_packets" 2>/dev/null || echo 0
}

echo "[2] Baseline counters:"
D0=$(read_dropped); C0=$(read_carrier_changes); P0=$(read_txpkts)
echo "    tx_dropped=$D0  carrier_changes=$C0  tx_packets=$P0"
echo

# 3. Generate steady traffic in the background so the qdisc is active. We use
#    ping flood if available (needs an IP/peer) OR just rely on the user running
#    the ber tool. Here we emit a large number of broadcast frames via a tiny
#    python raw-socket sender to keep the queue busy without needing a peer.
echo "[3] Starting background frame generation (raw broadcast, EtherType 0x88B5)..."
python3 - "$IFACE" << 'PYEOF' &
import socket, struct, sys, time
ifn=sys.argv[1]
s=socket.socket(socket.AF_PACKET, socket.SOCK_RAW)
s.bind((ifn,0))
mac=s.getsockname()[4] if False else b'\x00\x00\x00\x00\x00\x00'
# build a 1000-byte frame, experimental ethertype 0x88B5
frame=b'\xff'*6 + b'\x02\x00\x00\x00\x00\x01' + b'\x88\xb5' + bytes(1000)
end=time.time()+20
n=0
while time.time()<end:
    try:
        s.send(frame); n+=1
    except BlockingIOError:
        time.sleep(0.0001)
    except OSError:
        time.sleep(0.001)   # no carrier: keep trying
print(f"    [gen] sent {n} frames", file=sys.stderr)
PYEOF
GENPID=$!
echo "    generator PID $GENPID"
echo

# 4. Monitor for 20s. User should induce a link drop during this window.
echo "[4] Monitoring 20s — INDUCE A BRIEF LINK DROP NOW (short pins / unplug)..."
echo "    time  tx_dropped  carrier_changes  tx_packets   delta_pkts"
PREV_P=$P0
for i in $(seq 1 20); do
    sleep 1
    D=$(read_dropped); C=$(read_carrier_changes); P=$(read_txpkts)
    DP=$((P-PREV_P)); PREV_P=$P
    printf "    %3ds  %-10s  %-15s  %-11s  %s\n" "$i" "$D" "$C" "$P" "$DP"
done
echo

wait $GENPID 2>/dev/null

# 5. Verdict.
D1=$(read_dropped); C1=$(read_carrier_changes)
echo "[5] Result:"
echo "    carrier_changes during test: $((C1-C0))  (0 = no drop induced; re-run and drop the link)"
echo "    tx_dropped delta:            $((D1-D0))"
echo
if [ $((C1-C0)) -eq 0 ]; then
    echo "    INCONCLUSIVE: no link transition detected. Re-run and physically"
    echo "    drop the link during the 20s window."
elif [ $((D1-D0)) -eq 0 ]; then
    echo "    => BUFFERED. tx_dropped stayed flat across a real carrier drop:"
    echo "       the qdisc held frames until the link returned. Set txqueuelen"
    echo "       >= credit window (done: $QLEN) and outages cause near-zero loss."
else
    echo "    => DROPPED. tx_dropped climbed by $((D1-D0)) during the outage:"
    echo "       the kernel/driver discards frames when carrier is down. Frames"
    echo "       offered during an outage are lost (handled by RX accounting)."
fi
echo
echo "To restore: ip link set $IFACE txqueuelen 1000"
