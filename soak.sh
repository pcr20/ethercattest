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
# Run as root (raw sockets).
#   sudo ./soak.sh <slaves> [interval_s] [tag]
# e.g. a 7-slave chain probed every minute:
#   sudo ./soak.sh 7 60 mixed_chain
#
# SLAVES is a positional argument, not an environment variable, because sudo
# resets the environment — "sudo SLAVES=7 ./soak.sh" does not do what it looks
# like it does. Getting it wrong is not harmless: ecat_ber compares the BRD
# working counter against the slave count, so an incorrect value reports a
# permanent WKC mismatch on every frame.
set -u

IFACE=${IFACE:-enp2s0}
SLAVES=${1:?usage: sudo ./soak.sh <slaves> [interval_s] [tag]}
INTERVAL=${2:-60}
TAG=${3:-soak_$(date +%Y%m%d_%H%M%S)}
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

# Wait for a quiet wire. In the first overnight run the baseline regdump ran
# while a previous instance was still shutting down and ALL 30 reads failed.
echo "[soak] waiting for a quiet wire..."
for _ in $(seq 1 30); do
    pgrep -x ecat_ber >/dev/null 2>&1 || break
    sleep 1
done
sleep 2

# Baseline BEFORE any traffic: per-position identity and register state for
# EVERY slave. With a mixed chain (EVS-XCR-E + unknown silicon) the log must
# record what was present and in what order, since register semantics beyond
# the ETG-standard set cannot be assumed for unknown ESCs.
for p in $(seq 0 $((SLAVES-1))); do
    ./ecat_regdump -i "$IFACE" -p "$p" > "$OUT/regdump_start_pos${p}.txt" 2>&1
    TYPE=$(awk '/0x0000 Type/{print $3}' "$OUT/regdump_start_pos${p}.txt")
    PORTS=$(awk '/0x0007 Port descriptor/{print $4}' "$OUT/regdump_start_pos${p}.txt")
    echo "[soak]   position $p: ESC type=0x${TYPE:-??} portdesc=0x${PORTS:-??}"
done
./ecat_regdump -i "$IFACE"                    > "$OUT/regdump_start.txt" 2>&1
# PHY baseline and canary, per position. Only slaves whose ESC implements MII
# management can be probed; the rest are recorded as such and skipped rather
# than being decoded against the TI register map, which would report confident
# nonsense for another vendor's PHY.
MII_POS=""
for p in $(seq 0 $((SLAVES-1))); do
    SW="$OUT/phy_start_pos${p}.txt"
    ./ecat_phy -i "$IFACE" -p "$p" --ext --allow-phy-write \
               --csv "$OUT/phy_start_pos${p}.csv" > "$SW" 2>&1
    rc=$?

    # Report what actually happened, not what was attempted. An earlier version
    # printed "canary planted" for every position whose sweep merely exited 0 —
    # including four slaves where discovery found no PHY, nothing was probed and
    # no canary was written. A log that overstates coverage is worse than no log.
    NPHY=$(grep -c "identity:" "$SW" 2>/dev/null); NPHY=${NPHY:-0}
    if grep -q "PDI HAS ACCESS" "$SW" 2>/dev/null; then
        echo "[soak]   position $p: SKIPPED — the slave's own firmware (PDI) owns"
        echo "[soak]                the MII interface; the master cannot reach its PHYs"
        continue
    fi
    if [ "$rc" -ne 0 ] || [ "$NPHY" -eq 0 ]; then
        echo "[soak]   position $p: SKIPPED — no PHY reachable (ecat_phy rc=$rc)"
        continue
    fi

    CW="$OUT/canary_write_pos${p}.txt"
    ./ecat_phy -i "$IFACE" -p "$p" --canary-write --allow-phy-write > "$CW" 2>&1
    PLANTED=$(grep -c "read-back" "$CW" 2>/dev/null); PLANTED=${PLANTED:-0}
    REFUSED=$(grep -c "REFUSED" "$CW" 2>/dev/null); REFUSED=${REFUSED:-0}
    MII_POS="$MII_POS $p"
    if [ "$REFUSED" -gt 0 ]; then
        echo "[soak]   position $p: $NPHY PHY(s) probed; canary REFUSED on $REFUSED"
        echo "[soak]                (not a confirmed DP83822 — reset detection"
        echo "[soak]                 is NOT available for that PHY)"
    else
        echo "[soak]   position $p: $NPHY PHY(s) probed, canary planted ($PLANTED writes)"
    fi
done
echo "$MII_POS" > "$OUT/mii_positions.txt"
echo "[soak] baseline captured; MII positions:${MII_POS:- none}"

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
    # ecat_ber must stop TX, drain the qdisc backlog (~300 ms, polled
    # adaptively) and mute RX before the wire is quiet enough to probe. With
    # only 0.3 s here, 43% of probes in the first overnight run failed with
    # "cannot read the ESC MII block" — they were started while the wire was
    # still saturated and burned their 10 ms timeouts on BER frames.
    sleep 1.0
    # Direct registers only: no --ext, so the probe does NOT write the PHY.
    # The canary check is read-only too.
    for p in $MII_POS; do
        ./ecat_phy -i "$IFACE" -p "$p" -d --csv "$OUT/phy_${TS}_pos${p}.csv" \
                   --canary-check >> "$OUT/phy_${TS}.txt" 2>&1
    done
    kill -USR2 "$BER_PID"

    if grep -qE "GONE|SATURATED|WRITE FAILED|READ FAILED" "$OUT/phy_${TS}.txt"; then
        echo "[soak] !!! probe $N ($TS) flagged something — see $OUT/phy_${TS}.txt"
    fi
    echo "[soak] probe $N done ($TS)"
done

echo "[soak] ecat_ber exited; final register state"
for p in $(seq 0 $((SLAVES-1))); do
    ./ecat_regdump -i "$IFACE" -p "$p" > "$OUT/regdump_end_pos${p}.txt" 2>&1
done
./ecat_regdump -i "$IFACE" > "$OUT/regdump_end.txt" 2>&1
