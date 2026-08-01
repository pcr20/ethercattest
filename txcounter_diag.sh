#!/bin/bash
# ── Characterize the r8169 HARDWARE tally counters (from the Dump Tally Counter
# ── Command, exposed via ethtool -S) as a measurement instrument for the
# ── "r8169-and-downstream" boundary.
#
# Establishes:
#   1. Boundary identity during an outage: enqueued - tx_packets ~= tx_dropped,
#      and tx_errors (TxER, incl. carrier-lost) climbs. Frames not reaching the
#      wire split cleanly into kernel-drops (above r8169) vs TxER (at r8169/PHY).
#   2. Update granularity: how coarsely hardware tx_packets steps (the driver
#      rate-limits the DTC dump), i.e. the boundary's precision in frames.
#   3. Monotonicity across outage/recovery.
#
# Usage: sudo ./txcounter_diag.sh <iface>
#   Induce a brief link drop (short pins / unplug) during the 20s window.
set -u
IFACE="${1:-enp2s0}"

# Pull a single ethtool -S field.
es() { ethtool -S "$IFACE" 2>/dev/null | awk -v k="$1:" '$1==k{print $2}'; }
# Kernel-side qdisc drop counter (software, above the r8169).
kdrop() { cat "/sys/class/net/$IFACE/statistics/tx_dropped" 2>/dev/null || echo 0; }

echo "=== r8169 hardware tally-counter diagnostic on $IFACE ==="
echo "Fields: tx_packets=TxOk(hw)  tx_errors=TxER(hw, incl carrier-lost)"
echo "        tx_dropped=qdisc drop(sw, ABOVE the r8169)"
echo

# ── Phase A: granularity under steady saturation (no outage). ───────────────
# Sample hardware tx_packets every 20ms for 2s while generating traffic; the
# step pattern reveals the DTC dump rate-limit (how many frames per update).
echo "[A] Granularity probe: sampling hw tx_packets every 20ms under load (2s)."
echo "    Generating background traffic..."
python3 - "$IFACE" << 'PYEOF' &
import socket,sys,time
s=socket.socket(socket.AF_PACKET,socket.SOCK_RAW); s.bind((sys.argv[1],0))
f=b'\xff'*6+b'\x02\x00\x00\x00\x00\x01'+b'\x88\xb5'+bytes(1000)
end=time.time()+25
while time.time()<end:
    try: s.send(f)
    except BlockingIOError: time.sleep(0.0001)
    except OSError: time.sleep(0.001)
PYEOF
GEN=$!
sleep 1
echo "    t(ms)  tx_packets   delta"
PREV=$(es tx_packets); T0=$(date +%s%N)
for i in $(seq 1 100); do
    sleep 0.02
    P=$(es tx_packets); NOWMS=$(( ($(date +%s%N)-T0)/1000000 ))
    D=$((P-PREV)); PREV=$P
    # Only print when it steps, to reveal the update cadence.
    if [ "$D" -ne 0 ]; then printf "    %5d  %-11s  +%d\n" "$NOWMS" "$P" "$D"; fi
done
echo "    (rows appear only when the counter steps; gaps between rows = dump"
echo "     rate-limit period; delta = frames per hardware update = precision)"
echo

# ── Phase B: boundary identity across an induced outage. ────────────────────
echo "[B] Boundary identity across an outage — INDUCE A LINK DROP NOW (15s)."
echo "    Checking:  enqueued(~tx_packets+dropped) split, and TxER climb."
P0=$(es tx_packets); E0=$(es tx_errors); D0=$(kdrop); C0=$(cat /sys/class/net/$IFACE/carrier_changes)
echo "    baseline: TxOk=$P0  TxER=$E0  qdisc_drop=$D0  carrier_changes=$C0"
echo "    time  dTxOk    dTxER   d_qdrop   carrier_chg"
PP=$P0; PE=$E0; PD=$D0
for i in $(seq 1 15); do
    sleep 1
    P=$(es tx_packets); E=$(es tx_errors); D=$(kdrop); C=$(cat /sys/class/net/$IFACE/carrier_changes)
    printf "    %3ds  %-7s  %-6s  %-8s  %s\n" "$i" "$((P-PP))" "$((E-PE))" "$((D-PD))" "$((C-C0))"
    PP=$P; PE=$E; PD=$D
done
echo

wait $GEN 2>/dev/null

P1=$(es tx_packets); E1=$(es tx_errors); D1=$(kdrop); C1=$(cat /sys/class/net/$IFACE/carrier_changes)
echo "[C] Totals over phase B:"
echo "    TxOk (hw, on wire):        +$((P1-P0))"
echo "    TxER (hw, incl carrier):   +$((E1-E0))"
echo "    qdisc drop (sw, above HW): +$((D1-D0))"
echo "    carrier_changes:            $((C1-C0))"
echo
echo "    INTERPRETATION:"
echo "    - TxER climb during outage = frames the r8169 MAC rejected at the PHY"
echo "      (carrier lost). These ARE within your boundary (r8169+downstream)."
echo "    - qdisc drop climb = frames the KERNEL discarded above the r8169."
echo "      These are OUTSIDE your boundary — never reached the MAC."
echo "    - If carrier_changes==0: no outage was induced; re-run and drop link."
echo
echo "    => For BER: count only frames that reached the wire (TxOk) and did not"
echo "       return good = physical-layer loss. Exclude qdisc drops entirely."
echo "       TxER tells you how many the r8169 itself rejected (carrier), which"
echo "       you may report separately as link-down collateral vs true BER loss."
