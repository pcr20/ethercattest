# EtherCAT BER Test Tool

Tests EtherCAT physical layer bit error rate by saturating a slave chain
with NOP frames and logging CRC errors from both the host NIC PHY and
ESC slave registers.

## Receiving corrupt frames (rx-all + rx-fcs)

By default the NIC checks the Ethernet FCS in hardware and **drops** any frame
that fails — so a frame corrupted mid-flight (e.g. by a cable fault) never
reaches the host and silently becomes a "lost" frame rather than a visible
corruption. To receive corrupt frames instead, `setup.sh` enables two ethtool
features on the RTL8125 (both supported):

- **`rx-all on`** — deliver frames that fail the FCS check instead of dropping.
- **`rx-fcs on`** — deliver the 4-byte FCS trailer so it can be verified in
  software.

The tool detects these at startup (via `ETHTOOL_GFEATURES`) and reports their
state. With them on, corrupt frames are received and flagged two independent
ways:

- **Bad FCS (computed)** — the tool recomputes the Ethernet FCS (standard
  CRC32, poly 0xEDB88320 — distinct from the CRC32C payload check) over each
  frame and compares to the delivered trailer. Authoritative.
- **Bad FCS (kernel)** — from the `PACKET_AUXDATA` `tp_status` the kernel
  attaches to each frame. On the first few self-detected bad frames the tool
  prints the raw `tp_status` so the exact FCS-fail bit for this driver can be
  confirmed.

Both are shown side by side (`Bad FCS (computed/kernel): N / M`) to cross-check,
and appear in the CSV as `rx_bad_fcs_computed`, `rx_bad_fcs_auxdata`, plus
`rx_truncated` for frames too short to parse (e.g. cut off by a mid-frame
fault).

A **corrupt-but-returned** frame whose sequence number still matches an
outstanding frame counts as *returned* (not lost) and separately as bad-FCS —
so a mid-frame cable short now surfaces as corruption rather than inflating the
loss count. This directly addresses the case where a pin-short during a
saturated run produced zero visible CRC errors: the corrupt frames were being
dropped by the NIC before the socket; now they are delivered and counted.

**Self-check:** if the tool ever flags >50% of frames as bad-FCS early in a
run, it prints a warning — that indicates the FCS trailer offset/byte-order
assumption needs adjustment for the driver, not that the link is bad. (The
standard-CRC32 algorithm itself is verified against the `0xCBF43926` check
value at build/test time.)

## Link-loss monitoring (host NIC)


Link-loss (carrier down/up) events on the host interface are counted two
independent ways, which cross-check each other:

**sysfs counters** (polled each supervisor tick) — three cumulative counters
read from `/sys/class/net/<if>/`:
- `carrier_down_count` — link-down events
- `carrier_up_count` — link-up events
- `carrier_changes` — total transitions (should equal down + up)

**netlink events** (event-driven, timestamped) — a thread subscribed to
`RTNLGRP_LINK` watches `IFF_LOWER_UP` transitions on the interface, counting
down-events (up→down) and up-events (down→up) separately. Each transition is
printed inline to stderr the instant it happens, with the recovery duration:

```
[+14.203s] LINK DOWN (host NIC enp2s0)
[+14.251s] LINK UP (down 48ms)
```

The netlink down-count and the sysfs `carrier_down_count` delta measure the
same physical events by different mechanisms and should agree; a divergence is
itself diagnostic (e.g. a flap too brief for one path to observe).

Output:
- The running panel gains a **Link (host NIC)** section with all three sysfs
  counters, the netlink down/up counts, and current link state.
- The main CSV gains columns `carrier_down,carrier_up,carrier_changes,
  link_down_nl,link_up_nl`.
- A separate **`<output>_linkevents.csv`** logs one row per transition:
  `t_rel_s,direction,down_duration_ms`. This is the drop timeline for
  correlating link losses against, e.g., when you flexed a cable.

This monitors the **host NIC's** link only (the PC↔slave-0 segment). For
per-slave, per-port link loss deeper in the chain, the ESC lost-link register
0x0310 would be read per slave — not yet wired into the APRD read.

## BER reporting


BER is reported as an **upper-bound estimator**:

```
BER ≤ (errors + 0.5) / N
```

where N is the total number of **wire bits** (frames confirmed transmitted ×
~12000 bits/frame). The `+0.5` gives a conservative bound even when zero errors
have been observed — with no errors it reports `BER ≤ 0.5/N`, the smallest
bound the data can support — and adds the same half-count margin once errors
appear. It is applied identically to both BER lines:

- **BER (CRC)** — from the NIC's FCS error count (link-layer).
- **BER (payload)** — from the independent CRC32C payload check
  (FCS-independent; catches corruption a hop-by-hop FCS would mask).

Note this is the "add-half" rule of thumb, not a specific confidence level. For
reference, a 95%-confidence zero-error bound would use `2.996/N` and 90% would
use `2.303/N`; `0.5/N` corresponds to roughly 39% confidence. Quote it as an
order-of-magnitude bound, and run long enough that N pushes the bound below
your target (e.g. N ≥ 5×10¹² bits to claim BER ≲ 10⁻¹³).

## Wire-exact loss accounting (model B)


A frame is marked "outstanding" (eligible to be counted lost) only when its
**software TX timestamp completion** confirms it left the driver — not when
`send()` accepts it into the ring. The error-queue reader thread reads each
completion's `SOF_TIMESTAMPING_OPT_ID` value (verified to equal the send-order
sequence number, one completion per send) and marks that sequence transmitted.

Consequences:

- **`Lost frames` is wire-exact**: it counts only frames confirmed transmitted
  that never returned. A frame enqueued but never transmitted (the socket/qdisc
  tail discarded at shutdown) is never marked outstanding and so can never be
  miscounted as loss.
- A new line, **`TX confirmed (loss base)`**, shows how many frames were
  confirmed transmitted — this is the denominator for the loss rate.
- **`Enqueued, never tx`** appears at shutdown showing the discarded tail
  (`enqueued − transmitted`), explicitly labelled *NOT loss*. This is where the
  frames that older builds reported as phantom "lost" now correctly land.

If TX timestamping is unavailable, the tool falls back to marking frames
outstanding at enqueue (the frame is still counted, but the loss figure is then
enqueue-based rather than wire-exact — flagged in the header line).

### Completion-accounting guard

Model B's loss figure depends on the TX completion stream being complete. At
shutdown the tool cross-checks the number of counted completions against the
driver's own `tx_packets`; if they diverge by more than ~0.1%, it prints a
warning that completions were dropped and the loss figure may be overcounted,
directing you to cross-check against the (independent) NIC CRC error count.

## TX pipeline: three counters


The tool measures the transmit path at three stages so you can see exactly
where frames accumulate:

- **TX enqueued (ring)** — `send()` calls the kernel accepted into the
  socket/qdisc/ring. This is "handed to the kernel", not "on the wire".
- **TX driver-xmit (sw ts)** — counted from software TX timestamp
  completions drained off the socket error queue (`MSG_ERRQUEUE`). The
  timestamp is taken in the driver's transmit path, just before the frame is
  handed to hardware. The RTL8125 has **no PTP hardware clock**
  (`ethtool -T` shows `PTP Hardware Clock: none`), so only *software* TX
  timestamping is available — this stage is strictly upstream of the wire.
- **TX on wire (ethtool)** — the driver's `tx_packets` hardware counter, read
  via `ethtool -S`. On this NIC it is the **authoritative on-wire figure**
  (closer to the wire than the software timestamp).

Two gaps are derived and printed:

- `enqueued − driver-xmit` = socket/qdisc backlog
- `driver-xmit − wire` = driver→hardware backlog

On a passive loopback plug you'll see the driver→wire gap grow (the plug's
latency backs frames up in the ring); on a real slave chain both gaps stay
near zero. This is what quantifies the TX buffering that older builds
mistook for loss.

## Independent payload integrity check (CRC32C)

Each frame carries, in its NOP payload:

```
[ 8 bytes ] sequence number
[ 4 bytes ] CRC32C over (sequence number ++ payload)
[ N bytes ] pseudo-random payload (xorshift64 seeded from the sequence number)
```

On receive the tool recomputes CRC32C over the received sequence bytes and
payload and compares to the embedded value. A mismatch increments
**Payload CRC err** and is flagged `*** PAYLOAD CORRUPTION (FCS-independent)
***`.

This is **independent of the Ethernet FCS**. The link FCS is checked and
regenerated at every hop, so a slave that corrupts data internally and then
emits a valid FCS would show **zero** NIC CRC errors but a **non-zero**
payload CRC error. That fault class is invisible to the FCS-based BER and is
exactly what this check exists to catch. The payload is genuinely
pseudo-random (not a constant), which also exercises the 100BASE-TX MLT-3
scrambler far more realistically than a fixed fill.

CRC32C uses the SSE4.2 hardware instruction on the Ryzen (falls back to a
software table on CPUs without it). Build with `-msse4.2` (the Makefile does
this).

## Architecture


The tool runs three threads so the receive drain can never become the
bottleneck and the send side is limited only by bus saturation:

- **TX thread** (pinned to core 1) — builds and sends frames flat-out. Its
  only backpressure is `EAGAIN`/`ENOBUFS` from a full TX ring, which on a
  saturated 100BASE-TX link means "the wire is busy". It never waits on the
  receive side. Reported as `TX backpressure` — this is the *intended*
  signal that you are saturating the bus.

- **RX thread** (pinned to core 2) — batch-drains the socket with
  `recvmmsg()` (256 frames per syscall) into a 64 MB buffer, running on its
  own core so the kernel RX queue is kept empty. This is what stops the
  drain from ever saturating.

- **Supervisor** (main thread) — prints stats every 5 s, polls kernel drop
  counters, enforces the run duration. Touches no hot-path state.

Frame accounting uses a 1 M-entry sequence-number window shared lock-free
between TX and RX (atomic slot states; TX allocates and retires, RX marks
returns via CAS). A frame counts as *lost* only when its slot is reused
1,048,576 frames later while still outstanding — never merely because it is
briefly in flight.

### Drain saturation is detected, not guessed

The kernel's own RX-queue-overflow counter is polled via
`PACKET_STATISTICS` (`tp_drops`). If it is ever non-zero the drain fell
behind and the line is flagged:

```
  Kernel RX drops:0  (none, drain healthy)
```

vs.

```
  Kernel RX drops:1234  *** DRAIN SATURATED — BER INVALID ***
```

A kernel-dropped frame is **not** a wire loss, so this distinction is
essential: without it, a scheduling hiccup would masquerade as a bit error
and corrupt your BER figure. If you ever see drops, raise `net.core.rmem_max`
further, confirm IRQ steering, or reduce competing load on the RX core.

## Hardware


- **Host PC**: Bosgame E5 (Ryzen 5300U, dual RTL8125 2.5GbE NICs)
- **EtherCAT slaves**: AX58100-based development boards (4×)
- **Test cables**: standard CAT5e/6 initially; degraded cables to induce errors

## Quick Start

```bash
# 1. Install build dependencies
sudo apt install build-essential ethtool iperf3

# 2. Find your EtherCAT interface name
ip link show
# e.g. enp3s0, enp4s0 — the one connected to the slave chain

# 3. Build
make

# 4. Prepare interface
sudo ./setup.sh enp3s0

# 5. Run loopback test first (sanity check, no slaves needed)
sudo ./ecat_ber -i enp3s0 -l -d 30

# 6. Run 4-slave chain BER test
sudo ./ecat_ber -i enp3s0 -s 4 -v -o results.csv
```

## NIC Notes (RTL8125 / RTL8111)

The Bosgame E5 has dual Realtek NICs. After boot, identify which port
is which and find the interface names:

```bash
lspci | grep Ethernet
ip link show
# Match by MAC address on the physical port label if present
```

The NIC reports CRC errors via `ethtool -S <iface>`. The tool reads
`rx_crc_errors` and related counters automatically.

```bash
# Check NIC counters manually at any time:
ethtool -S enp3s0 | grep -iE "crc|error|drop"
```

## Loopback Cable Wiring

For the no-slave loopback test, wire an RJ45 plug as follows:

```
Pin 1 (TX+) ──→ Pin 3 (RX+)
Pin 2 (TX-) ──→ Pin 6 (RX-)
Pins 4,5,7,8: leave unconnected
```

**Critical**: do NOT connect pins 4,5,7,8. If all 8 pins are connected,
the NIC will attempt to negotiate 1000BASE-T, which the EtherCAT slaves
cannot support. With only the 100BASE-TX pair connected, the PHY falls
back to 100Mbps.

Alternatively, force 100Mbps via ethtool (setup.sh does this):
```bash
sudo ethtool -s enp3s0 speed 100 duplex full autoneg off
```

## BER Calculation

To confirm BER of 10⁻¹³:

```
Required bits = 10^13 = 10,000,000,000,000

At 100BASE-TX saturation:
  Max frame rate ≈ 8,100 frames/s  (1518-byte frames)
  Bits per frame ≈ 12,144
  Bit rate       ≈ 98,000,000 bits/s

Time to accumulate 10^13 bits:
  10^13 / 9.8×10^7 ≈ 102,040 seconds ≈ 28 hours

For zero observed errors in that period:
  Upper 95% confidence BER < 3 / 10^13 = 3×10^-13  (Poisson, 0 events)
```

The tool reports estimated BER in the periodic status output.

## Understanding the Output

```
── EtherCAT BER Test ────────────────────────────
  Elapsed:            3600.0 s
  Frames sent:        29,160,000
  Frames rcvd:        29,160,000
  Lost frames:        0
  BRD WKC mismatches: 0        ← if non-zero, a slave dropped out
  NIC CRC errors:     0        ← from RTL8125 PHY registers
  Total bits:         3.50e+11
  Est. BER:           0.00e+00
  ── Per-slave ESC CRC counters ──────────────────
  Slave  0: P0=0 P1=0 P2=0 P3=0
  Slave  1: P0=0 P1=0 P2=0 P3=0
  Slave  2: P0=0 P1=0 P2=0 P3=0
  Slave  3: P0=0 P1=0 P2=0 P3=0
─────────────────────────────────────────────────
```

**BRD WKC mismatches**: the broadcast Working Counter returned a value
different from the expected slave count. Indicates a slave temporarily
dropped from the ring (link loss, power issue).

**NIC CRC errors**: frames received by the host NIC that failed FCS check.
This is the primary BER measurement for the cable return path.

**Per-slave ESC CRC**: each slave's own invalid-frame counter for each
port, read via APRD datagrams. Non-zero values localise which segment
has errors (see earlier discussion of CRC asymmetry).

## Introducing Errors

To degrade the link and verify the test detects errors:

1. **Long cable**: use a 90–100m run of CAT5e (at the limit of 100BASE-TX)
2. **Poor quality cable**: unshielded patch cable near EMI sources
3. **Damaged cable**: deliberately crimp or partially crush a cable
4. **Loose connector**: partially unseated RJ45 plug

Observe which counter increments first:
- NIC CRC errors only → problem on return path (last slave back to PC)
- Slave 0 P0 CRC only → problem between PC and slave 0
- Slave N P0 CRC = Slave N-1 P1 CRC → problem between slave N-1 and N

## Troubleshooting

### frames_sent stays at 0

Every `send()` is failing. The rebuilt binary now prints the error to
stderr. The most common cause was an oversized frame: `MAX_FRAME` must be
**1514**, not 1518 — the 4-byte FCS is appended by the NIC hardware, so a
raw `AF_PACKET` socket rejects anything over 1514 bytes with `EMSGSIZE`.
This is fixed in the current version.

### frames_sent increments but frames_rcvd stays at 0 (passive loopback cable)

A passive TX→RX loopback plug (pins 1→3, 2→6) does **not** reliably work
on 100BASE-TX. Unlike 10BASE-T, 100BASE-TX uses MLT-3 line coding with a
side-stream scrambler, and the receiving PHY must recover its clock and
descrambler state from the incoming stream. When a PHY receives its own
scrambled transmission, the descrambler frequently fails to lock —
the link reports "up" but received frames are corrupted or dropped.

The `align_errors` climbing during `setup.sh` (before the tool even runs)
is the signature of this problem.

**Reliable alternatives to a passive loopback plug:**

1. **Loop between the two NICs** — the Bosgame E5 has two ports. Connect
   `enp1s0` ↔ `enp2s0` with a normal patch cable, send on one and receive
   on the other. Each PHY recovers clock from the *other* PHY's clean
   transmission, exactly as in normal operation. This is the recommended
   sanity-test topology.

2. **Use one real AX58100 slave** — a single powered slave in the chain
   provides a proper PHY that regenerates the signal, and its auto-loopback
   on the open port returns the frame cleanly.

3. **A managed switch** between the ports (if forcing 100Mbps) also works.

Passive loopback plugs are mainly reliable at 10BASE-T (no scrambler).
For 100BASE-TX testing, use the two-NIC or one-slave approach instead.

### Two-NIC loopback quick test

```bash
# Connect enp1s0 <-> enp2s0 with a patch cable, then:
sudo ./setup.sh enp1s0
sudo ./setup.sh enp2s0
# Send on enp2s0; frames physically arrive at enp1s0 and loop back
# through the NIC's own MAC only if both share the wire — for a true
# send/receive split, run the tool on enp2s0 and sniff enp1s0:
sudo tcpdump -i enp1s0 -e ether proto 0x88a4 -c 10
# In another terminal:
sudo ./ecat_ber -i enp2s0 -l -d 30
# tcpdump should show your 0x88A4 frames arriving on enp1s0.
```



### High "In flight" and a loss spike at shutdown on the passive loopback plug

On a passive TX→RX loopback plug you will see two things that are **not**
real losses:

1. **"In flight" grows steadily** (e.g. 15k → 60k over 30s) and the sent
   rate reads ~10,600 fps / 127 Mbit/s — above the 100BASE-TX wire limit.
   The passive plug adds latency, so the NIC's TX ring buffers frames ahead
   of the wire and the send counter runs ahead of returns. On a real
   EtherCAT chain the round-trip is microseconds and in-flight stays tiny
   (dozens), so this inflation does not occur.

2. **A loss spike only appeared in older builds.** The tool now runs a
   **drain barrier** at shutdown: it stops TX, waits (up to 2s, or until
   returns catch up) for in-flight frames to come back, and only then sweeps
   the sequence window for genuine losses. In-flight frames at stop time are
   no longer miscounted as lost. In addition, TX now **bounds outstanding
   frames to `MAX_INFLIGHT` (4000)** so the sent/received gap can never grow
   unboundedly on a high-latency link; this keeps backpressure tied to the
   wire rather than to buffering, and leaves almost nothing stuck at
   shutdown. Any residual shutdown "loss" on a passive plug is TX-side ring
   buffering, not wire loss.

**Bottom line on the passive plug:** it validates the tooling (bus
saturation, zero-drop drain, clean CRC baseline) but its latency inflation
and — on `lo` — double delivery make its loss/in-flight figures unreliable.
For trustworthy end-to-end numbers, test with a real slave (below).

### Validate with one real slave, not the plug

The passive loopback plug cannot exercise the per-slave CRC/BRD/APRD path and
distorts in-flight accounting. Insert a single AX58100 slave and run:

```bash
sudo ./ecat_ber -i enp2s0 -s 1 -v
```

A real slave returns each frame exactly once and regenerates the signal with
microsecond latency, so in-flight stays near zero, shutdown is clean, and the
BRD working-counter / per-slave CRC registers become meaningful. Scale to
`-s 4` once the single-slave case looks right.


### Testing against the `lo` interface shows inflated lost/dup counts


Don't validate against `lo`. The Linux loopback interface can deliver a raw
frame to the socket more than once and reflects sent frames back
immediately, so the sequence tracker sees duplicate returns and the lost /
`Unknown-dup seq` counters climb. This is a property of `lo`, not the tool —
on a real EtherCAT chain each slave forwards each frame exactly once around
the ring, so each frame returns exactly once. Validate with the two-NIC
loopback or a real slave instead.

Each row (written every 5 seconds):

```
elapsed_s, frames_sent, frames_rcvd, frames_lost, nic_crc_errors,
brd_wkc_mismatches, kernel_rx_drops, slave0_p0_crc, slave0_p1_crc,
slave0_p2_crc, slave0_p3_crc, slave1_p0_crc, ...
```

Import into Excel or Python/pandas for post-analysis:

```python
import pandas as pd
df = pd.read_csv('results.csv')
df['ber_estimate'] = df['nic_crc_errors'] / (df['frames_sent'] * 12000)
print(df.tail())
```
