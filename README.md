# EtherCAT BER Test Tool

Tests EtherCAT physical layer bit error rate by saturating a slave chain
with NOP frames and logging CRC errors from both the host NIC PHY and
ESC slave registers.

## Accounting model (TxOk wire-truth boundary)

The measurement boundary is **the r8169 and everything wire-side of it** — the
MAC's transmit path, the PHY, the cable, the slaves, and the return path. The
kernel's software stack *above* the r8169 (the qdisc, which drops frames on
carrier-down) is outside the boundary: a frame the kernel discards before it
ever reaches the r8169 is not a physical-layer loss and must not pollute the BER.

The boundary is read directly from the r8169's **hardware tally counters** (the
Dump Tally Counter block, exposed via `ethtool -S` / `ETHTOOL_GSTATS`):

- **TxOk** (`tx_packets`) — frames the MAC actually clocked onto the wire. This
  is the wire-truth signal and the BER denominator.
- **TxER** (`tx_errors`) — frames the MAC rejected, *including carrier-lost*.
  These reached the r8169 but the physical layer refused them: link-down
  collateral, **not** a bit error. Reported separately, excluded from BER.
- **qdisc tx_dropped** (kernel software counter, sysfs) — frames the kernel
  discarded *above* the r8169. Never reached the MAC. Excluded entirely.

A sampler thread reads all three every 20ms (the diagnostic confirmed TxOk
updates smoothly, ~per-frame under saturation, so the boundary is sharp). The RX
thread counts **distinct returned frames** (deduplicated by sequence number, so
a double-delivering interface like `lo` can't inflate the count). Loss is then a
pure count:

> **loss = TxOk − distinct returns**  (frames that reached the wire but never
> came back)

and **BER denominator = TxOk**. Because qdisc-dropped and TxER frames are never
in TxOk, they can never be counted as loss — the kernel artifacts you don't care
about are excluded by construction. Corruption (bad FCS / bad payload CRC) is
counted separately by the detectors; a corrupt frame *returned*, so it is not
loss. All sequence values are uint64 (a 32-bit counter would wrap in ~6 days at
8127 fps).

At termination the tool waits for TxOk and the distinct-return count to both go
quiet (settle), at which point `TxOk − distinct` is exact. If the link is down
and returns cannot drain, it warns once per second ("reestablish link to
complete test (N/10)") up to 10 times, then exits non-zero and marks the run
INCOMPLETE rather than reporting a figure that counts still-in-flight frames as
false loss.

Note: `lo` (loopback) has no r8169 and therefore no hardware tally counters, so
TxOk reads 0 there and loss/BER are unavailable — `lo` is a plumbing smoke test
only. The real measurement requires the physical `enp2s0` interface.

## TX throughput (TxOk-anchored backlog cap)

TX is bounded by a single cap on the offered-but-not-yet-on-wire backlog:

> **enqueued − TxOk ≥ 8000  →  pause**

This is deadlock-free by construction, which earlier returns-based schemes were
not:

- **Normal:** TxOk keeps pace with enqueued (backlog ≈ ring depth); the TX
  ring's own EAGAIN paces at line rate.
- **Outage:** TxOk freezes (frames don't reach the wire), the backlog grows to
  the cap, and TX pauses — bounded, no explosion.
- **Recovery:** TxOk advances the instant the wire works again — crucially, this
  does **not** depend on any frame *returning*. So the backlog drains and TX
  resumes immediately. On a NIC that drops during carrier-down (confirmed for
  the r8169/RTL8125), the dropped frames are simply never counted in TxOk; there
  is no stuck window to deadlock on, because the pacing anchor is "reached the
  wire", not "came back". No credit valve or write-off is needed.

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

- **Bad FCS (computed)** — the tool validates each frame's Ethernet FCS using
  the **CRC32 residual method**: it computes CRC32 (standard, poly 0xEDB88320 —
  distinct from the CRC32C payload check) over the *entire delivered frame
  including its 4-byte FCS trailer*. A valid frame yields a fixed magic residual
  constant; any other value means the frame is corrupt. This is how hardware
  validates, and is robust to FCS byte-order conventions (the tool accepts
  either of the two standard residuals). Authoritative.
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

**Self-check:** the tool prints the observed good-frame residual once at
startup (`[FCS] good-frame residual = 0x...`). It should match one of the two
accepted constants (0x2144DF1C or 0xDEBB20E3). If instead >50% of frames flag
as bad-FCS, the tool warns that the residual constant differs for this driver
and prints the observed value — which is then the correct constant to compile
in (a one-line change). The standard-CRC32 algorithm itself is verified against
the 0xCBF43926 check value, and the residual detection against 100 good + 100
corrupted synthetic frames, at test time.

## Link-loss monitoring (host NIC) — three independent sources

Carrier (link) transitions on the host interface are tracked three ways, each
with a different strength, so they cross-check:

**1. sysfs counters — authoritative counts.** `carrier_down_count`,
`carrier_up_count`, `carrier_changes` are incremented by the kernel on *every*
transition with no coalescing, so they are the ground-truth totals. Polled each
supervisor tick. Reported as "Transitions (sysfs, authoritative)".

**2. POLLPRI carrier thread — current state + timed events.** A thread opens
`/sys/class/net/<if>/carrier` and blocks on `poll(POLLPRI)`; the kernel's
`sysfs_notify()` on carrier wakes it at each transition — event-driven, minimal
latency. It owns `link_state_up` (the tool's live view of the link) and logs
timestamped transitions. `sysfs_notify` coalesces, so this can merge very fast
flaps — but it always reports the correct *current* state on wake, which is what
coalescing preserves. If a driver/kernel does not support POLLPRI on carrier
(some virtual interfaces), it automatically falls back to a 10ms timed re-read
and says so.

**3. netlink — independent cross-check.** An `RTMGRP_LINK` listener counts
`IFF_LOWER_UP` transitions. netlink's linkwatch source is itself rate-limited/
coalescing, so netlink is *not* authoritative for counts during a fast flap — it
is a third independent measurement. Hardened against socket overflow (4MB
receive buffer, tight drain); any residual `ENOBUFS`/`NLMSG_OVERRUN` is counted
and surfaced ("netlink overflows: K") rather than silently lost.

The design principle: **the sysfs counter is the true total; POLLPRI gives live
state and timing; netlink is an independent cross-check.** During a fast flap
storm the three diverge — e.g. sysfs `down 47`, carrier-thread `down 12`,
netlink `down 7` — and that divergence quantifies how much faster the link
thrashed than the notification layers could individually resolve. All appear in
the CSV (`carrier_down/up/changes`, `link_down_cp/up_cp`, `link_down_nl/up_nl`,
`netlink_overflows`), plus a per-event timeline in `<output>_linkevents.csv`
(`t_rel_s,direction,down_duration_ms`).

This monitors the **host NIC's** link only (the PC-to-slave-0 segment). For
per-slave, per-port link loss deeper in the chain, the ESC lost-link register
0x0310 would be read per slave — not yet wired into the APRD read.

## BER reporting


BER is reported as an **upper-bound estimator**:

```
BER ≤ (errors + 0.5) / N
```

where N is the total number of **wire bits** (TxOk × ~12000 bits/frame — frames
the r8169 actually clocked onto the wire). The `+0.5` gives a conservative bound even when zero errors
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

## TX pipeline diagnostics

The tool still shows the transmit-path stages as a **diagnostic** (they no longer
drive accounting — loss is TxOk-based):

- **TX enqueued (ring)** — `send()` calls the kernel accepted. "Handed to the
  kernel", not "on the wire". Used only for the backlog cap and display.
- **TX driver-xmit (sw ts)** — counted from software TX timestamp completions off
  the socket error queue, if TX timestamping is available. A pipeline
  diagnostic only; the RTL8125 has no PTP hardware clock, so this is software
  timing, strictly upstream of the wire.
- **TxOk (hardware)** — the r8169's own tally counter of frames clocked onto the
  wire. This is the authoritative wire-truth figure and the BER denominator (see
  the accounting section above).

The gap `enqueued − TxOk` is the offered-but-not-yet-on-wire backlog; the TX cap
holds it at ≤ 8000. During an outage this gap grows (TxOk frozen) and TX pauses;
on recovery TxOk advances and it drains.

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
── EtherCAT BER Test ─────────────────────────────────────
  Elapsed:        3600.0 s
  ── TX pipeline (diagnostic) ────────────────────────────
  TX enqueued (ring):      29,160,000
  ── Wire boundary (r8169 hardware tally) ────────────────
  TxOk  (on wire, BER denom): 29,160,000
  TxER  (r8169 rejected, carrier-lost etc.): 0  (excluded from BER)
  qdisc drop (kernel, above r8169): 0  (excluded — never on wire)
  ── Accounting (wire-side) ──────────────────────────────
  Frames rcvd (raw):      29,160,000
  Distinct returns:       29,160,000
  Lost frames:    0  (reached wire, never returned — PHY loss)
  Payload CRC err:0  (clean)
  Bad FCS (computed/kernel): 0 / 0  (none)
  ── Link (host NIC) ─────────────────────────────────────
  Now: UP
  Transitions (sysfs, authoritative): down 0 / up 0 / changes 0
  BRD WKC mismatches: 0        ← if non-zero, a slave dropped out
  NIC CRC errors:     0        ← from RTL8125 PHY registers
  Total wire bits:3.50e+11
  BER (CRC) <=:   ...
──────────────────────────────────────────────────────────
```

**TxOk / TxER / qdisc drop**: the wire-truth boundary. TxOk is frames the r8169
put on the wire (BER denominator). TxER (carrier-lost) and qdisc drop (kernel,
above the r8169) are shown but **excluded from BER** — see the accounting model.

**Lost frames**: TxOk − distinct returns = frames that reached the wire but
never came back. Physical-layer loss only; kernel drops and carrier-lost frames
are never included.

**BRD WKC mismatches**: the broadcast Working Counter returned a value
different from the expected slave count. Indicates a slave temporarily
dropped from the ring (link loss, power issue).

**NIC CRC errors**: frames received by the host NIC that failed FCS check.
This is the primary BER measurement for the cable return path.

**Per-slave ESC CRC** (with `-v`): each slave's own invalid-frame counter for
each port, read via APRD datagrams. Non-zero values localise which segment
has errors.

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

### TX enqueued stays at 0

Every `send()` is failing. The rebuilt binary now prints the error to
stderr. The most common cause was an oversized frame: `MAX_FRAME` must be
**1514**, not 1518 — the 4-byte FCS is appended by the NIC hardware, so a
raw `AF_PACKET` socket rejects anything over 1514 bytes with `EMSGSIZE`.
This is fixed in the current version.

### TX enqueued increments but frames_rcvd stays at 0 (passive loopback cable)

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



### Passive loopback plug: what to expect

On a passive TX→RX loopback plug the plug's latency lets the NIC's TX ring
buffer frames ahead of the wire, so `TX enqueued` runs ahead of TxOk (the
`enqueued − TxOk` backlog grows). This is **not** loss — it is buffering, and
the backlog cap holds it at ≤ 8000. Because loss is now `TxOk − distinct
returns` (both hardware/wire-side quantities), the ring buffering cannot inflate
the loss figure the way older enqueue-based builds did.

**Bottom line on the passive plug:** it validates the tooling (bus saturation,
zero-drop drain, clean CRC baseline). For trustworthy end-to-end numbers, test
with a real slave (below).

### Validate with one real slave, not the plug

The passive loopback plug cannot exercise the per-slave CRC/BRD/APRD path.
Insert a single AX58100 slave and run:

```bash
sudo ./ecat_ber -i enp2s0 -s 1 -v
```

A real slave returns each frame exactly once and regenerates the signal with
microsecond latency, so the `enqueued − TxOk` backlog stays tiny, shutdown is
clean, and the BRD working-counter / per-slave CRC registers become meaningful.
Scale to `-s 4` once the single-slave case looks right.

### Testing against the `lo` interface

`lo` is a software interface with **no r8169 and no hardware tally counters**, so
TxOk reads 0 there and loss/BER are unavailable (the tool says so explicitly).
`lo` also double-delivers frames; the deduplicating return counter keeps
`distinct returns` honest, but without TxOk there is no wire-truth denominator.
Use `lo` only as a plumbing smoke test — the real measurement requires the
physical `enp2s0` interface.

Each row (written every 5 seconds). The columns are:

```
elapsed_s, tx_enqueued, tx_wire, txok, txer, qdisc_drop,
distinct_returns, frames_rcvd, frames_lost, nic_crc_errors,
payload_crc_errors, brd_wkc_mismatches, kernel_rx_drops,
carrier_down, carrier_up, carrier_changes,
link_down_cp, link_up_cp, link_down_nl, link_up_nl, netlink_overflows,
rx_bad_fcs_computed, rx_bad_fcs_auxdata, rx_truncated,
[slave0_p0_crc, slave0_p1_crc, slave0_p2_crc, slave0_p3_crc, slave1_...]
```

Key columns: `txok` is the wire-truth denominator; `frames_lost = txok −
distinct_returns`; `txer` and `qdisc_drop` are the excluded boundary artifacts
(carrier-lost and kernel drops respectively).

Import into Excel or Python/pandas for post-analysis:

```python
import pandas as pd
df = pd.read_csv('results.csv')
# BER against the wire-truth denominator (TxOk), not enqueued frames:
df['loss_rate'] = df['frames_lost'] / df['txok']
df['ber_crc']   = (df['nic_crc_errors'] + 0.5) / (df['txok'] * 12000)
print(df[['elapsed_s','txok','frames_lost','txer','qdisc_drop','ber_crc']].tail())
```
