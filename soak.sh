#!/bin/bash
# Long-soak orchestration: run ecat_ber continuously, and every INTERVAL
# seconds pause it, probe both PHYs, and resume.
#
# ecat_ber and the probe share one interface, so the probe's frames land in
# the NIC's interface-wide tx_packets and its replies reach ecat_ber's
# promiscuous ETH_P_ECAT socket. Running them concurrently would create
# phantom loss AND phantom payload-CRC errors. The pause protocol exists to
# make that safe:
#   SIGUSR1 -> ecat_ber stops TX, lets in-flight returns settle, then mutes
#              RX counting and snapshots the raw hardware TxOk
#   SIGUSR2 -> it reads TxOk again; since it sent nothing in between, the
#              delta is foreign by definition and is subtracted exactly
# A watchdog inside ecat_ber force-resumes after 30 s, so if this script dies
# mid-probe the soak continues rather than stalling until morning.
#
# Run as root (raw sockets). Usage: sudo ./soak.sh [interval_s] [tag]
set -u

IFACE=${IFACE:-enp2s0}
SLAVES=${SLAVES:-1}
INTERVAL=${1:-60}
TAG=${2:-soak_$(date +%Y%m%d_%H%M%S)}
OUT=$TAG; mkdir -p "$OUT"

[ "$(id -u)" -eq 0 ] || { echo "must run as root"; exit 1; }

BER_PID=""
cleanup() {
    echo "[soak] stopping"
    if [ -n "$BER_PID" ] && kill -0 "$BER_PID" 2>/dev/null; then
        kill -USR2 "$BER_PID" 2>/dev/null      # never leave it paused
        sleep 0.3
        kill -INT "$BER_PID" 2>/dev/null       # graceful: drain + settle
        wait "$BER_PID" 2>/dev/null
    fi
}
trap cleanup INT TERM EXIT

echo "[soak] interface=$IFACE slaves=$SLAVES interval=${INTERVAL}s out=$OUT/"

# Baseline BEFORE any traffic: full register state, and plant the canary.
./ecat_regdump -i "$IFACE"                    > "$OUT/regdump_start.txt" 2>&1
./ecat_phy -i "$IFACE" --ext --allow-phy-write \
           --csv "$OUT/phy_start.csv"         > "$OUT/phy_start.txt"     2>&1
./ecat_phy -i "$IFACE" --canary-write --allow-phy-write \
                                              > "$OUT/canary_write.txt"  2>&1
echo "[soak] baseline captured, canary planted"

./ecat_ber -i "$IFACE" -s "$SLAVES" -v -o "$OUT/ber.csv" \
           > "$OUT/ber.log" 2>&1 &
BER_PID=$!
sleep 5
kill -0 "$BER_PID" 2>/dev/null || { echo "[soak] ecat_ber died at startup"; exit 1; }

N=0
while kill -0 "$BER_PID" 2>/dev/null; do
    sleep "$INTERVAL"
    kill -0 "$BER_PID" 2>/dev/null || break
    N=$((N+1))
    TS=$(date +%Y%m%d_%H%M%S)

    kill -USR1 "$BER_PID"
    sleep 0.3                                  # let the pause take effect
    # Direct registers only: no --ext, so the probe does NOT write the PHY.
    # The canary check is read-only too.
    ./ecat_phy -i "$IFACE" --csv "$OUT/phy_${TS}.csv" --canary-check \
               > "$OUT/phy_${TS}.txt" 2>&1
    kill -USR2 "$BER_PID"

    if grep -qE "GONE|SATURATED|WRITE FAILED|READ FAILED" "$OUT/phy_${TS}.txt"; then
        echo "[soak] !!! probe $N ($TS) flagged something — see $OUT/phy_${TS}.txt"
    fi
    echo "[soak] probe $N done ($TS)"
done

echo "[soak] ecat_ber exited; final register state"
./ecat_regdump -i "$IFACE" > "$OUT/regdump_end.txt" 2>&1
