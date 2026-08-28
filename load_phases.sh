#!/bin/bash
# Controlled host-load phases for correlating CPU/IO contention against
# ecat_ber frame loss. Run this in a SECOND terminal while ecat_ber is running.
# Needs no root.
#
# Why these targets: ecat_ber pins TX to core 1 and RX to core 2, both at
# SCHED_FIFO priority 80, so ordinary SCHED_OTHER load cannot preempt them.
# The vulnerable path is the NIC's softirq/NAPI processing, which runs at
# normal priority — and IRQ 66 (enp2s0) is pinned to CPU 3. So core 3 is the
# stressor and cores 4-7 are the control: if loss tracks core 3 but not
# cores 4-7, the mechanism is NIC RX servicing, not general CPU pressure.

MARK=load_markers.txt
PHASE=${PHASE_SECS:-120}
IOFILE=/tmp/ecat_loadtest.bin
PIDS=()

cleanup() {
    for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done
    PIDS=()
    rm -f "$IOFILE"
}
trap 'echo; echo "interrupted"; cleanup; exit 130' INT TERM
trap cleanup EXIT

mark() {
    printf '%s  %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$1" | tee -a "$MARK"
}

spin_on() {                       # spin_on <cpu-list> <n>
    for _ in $(seq 1 "$2"); do
        taskset -c "$1" sh -c 'while :; do :; done' &
        PIDS+=($!)
    done
}

io_load() {
    ( while :; do
        dd if=/dev/zero of="$IOFILE" bs=1M count=512 conv=fsync 2>/dev/null
        rm -f "$IOFILE"
      done ) &
    PIDS+=($!)
}

: > "$MARK"
mark "START  phase length ${PHASE}s  (total $((PHASE*7))s)"
mark "PHASE 1 idle baseline";                      sleep "$PHASE"
mark "PHASE 2 CPU LOAD on core 3 (NIC IRQ core)";  spin_on 3 1;    sleep "$PHASE"; cleanup
mark "PHASE 3 idle recovery";                      sleep "$PHASE"
mark "PHASE 4 CPU LOAD on cores 4-7 (CONTROL)";    spin_on 4-7 4;  sleep "$PHASE"; cleanup
mark "PHASE 5 idle recovery";                      sleep "$PHASE"
mark "PHASE 6 DISK I/O LOAD (fsync writes)";       io_load;        sleep "$PHASE"; cleanup
mark "PHASE 7 idle recovery";                      sleep "$PHASE"
mark "END — stop ecat_ber with Ctrl-C now"
