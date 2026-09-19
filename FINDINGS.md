# EVE-NET frame corruption — investigation findings

Working record of the EtherCAT frame-corruption investigation. Written to be
read by someone who has not followed the day-to-day work. Every number here
comes from a run on the rig; where something is inferred rather than measured,
it says so.

Status: **mechanism characterised, origin attributed, root cause not yet found.**

---

## 1. Summary

Frames returning to the master are occasionally delivered with their **leading
bytes missing**. The remainder of the frame is byte-for-byte correct — the
payload is never damaged — and the frame carries an invalid FCS plus a short
trailer that marks it as bad.

The damage originates at **EVE-NET modules** (ESC type `0x90`). Seven
independent origin inferences across five runs all land on an EVE-NET; no
EVS-XCR, EVS-NET or third-party slave has ever been the origin of a step
increase in the error gradient.

The emission rate is **not a fixed property of the device**: the same module has
measured 0/s, 0.011/s and 0.11/s in different chain arrangements. But **no model
of that variation currently survives the data** — a downstream-depth hypothesis
was proposed and then falsified by a controlled test (§5). Every rate
measurement so far is underpowered.

**No link has ever dropped.** Across 8.25 hours of continuous measurement:
Fast Link Down never fired, every ESC lost-link counter stayed at zero, and the
host NIC logged no carrier transitions. The reported field symptom of
intermittent *link drops* has not been reproduced and may be a different fault.

---

## 2. The fault signature

### 2.1 Data is never corrupted

Over 238.7 million frames (2.9 × 10¹² bits) in the longest run:

| measure | value |
|---|---|
| payload CRC32C errors | **0** |
| payload BER | ≤ 1.72 × 10⁻¹³ |
| damaged frames | 3,221 |
| BER (whole frame, FCS) | ≤ 1.11 × 10⁻⁹ |
| frame loss rate | 1.39 × 10⁻⁵ |

Not one byte of payload was ever wrong. **Every failure is a framing failure.**

### 2.2 Three signatures

From 3,221 damaged frames captured in one run:

| class | n | description | paired? |
|---|---|---|---|
| **prefix loss** | 3,068 (95.2%) | first K bytes missing; ends `…+4 bytes+0x50` | — |
| **+4, ends `0x55`** | 28 (0.9%) | 1522 bytes | **24 of 28** followed by a prefix-loss frame within 200 µs |
| **+1, ends `0x00`/`0x01`** | 125 (3.9%) | 1519 bytes | always isolated |

The trailing `0x50` is **not applied by the device that caused the damage** —
it is the EVS-XCR's bad-frame marking, added in transit (§7.1).

`0x55` is the Ethernet preamble byte. The pairing is not coincidence: at 0.103
events/s the expected number of chance coincidences within 200 µs across 153
trials is ~0.003; 24 were observed. In a later run **every** 1522-byte frame was
followed by a truncated frame at the same timestamp, 17 pairs with no
exceptions.

So classes 1 and 2 are **one disturbance straddling a frame boundary**: the tail
of frame N runs into the preamble of frame N+1, and frame N+1 loses its head.

### 2.3 Recovering K

The number of missing bytes is recoverable from the damaged frame alone, two
independent ways, which agree exactly on every frame measured:

- **from length**: `K = 1519 − len`
- **from structure**: APRD datagram for slave *s* begins at absolute offset
  `994 + 52·s` in an intact frame, giving 10 concurrent votes per frame

---

## 3. K is exponentially distributed

3,068 prefix-loss frames, 100-byte bins:

```
   0- 99 bytes  912  ########################################
 100-199        722  ###############################
 200-299        496  #####################
 300-399        313  #############
 400-499        234  ##########
 500-599        144  ######
 600-699        107  ####
 700-799         65  ##
 800-899         42  #
 900-999         33  #
```

mean 247, median 184, sd 207, **mean/sd = 1.20**. A uniform distribution over
this range would give sd ≈ 279; an exponential gives sd = mean. Decay constant
**λ ≈ 280 bytes ≈ 22 µs**. Range 27–995 bytes (2.2–79.6 µs).

This is a **memoryless recovery process** — constant hazard per unit time. It
rules out any fixed timing constant (EEE wake, PLL relock, a watchdog).

Interpreting it: for the host NIC to accept these frames at all, something must
have transmitted a fresh preamble and SFD followed by the tail of our frame. A
device is resynchronising mid-stream and starting a new frame from wherever it
regained lock. *(Model consistent with the data; not confirmed against any
forwarding specification.)*

### 3.1 Timing is Poisson

Inter-event intervals: mean/sd = **0.95**, median 6.12 s, rate 0.108/s. Random
and memoryless — **not** the regular intervals described in the field report.

---

## 4. Where the damage originates

The ESC counts **receives only**. Per slave there are two counter sets, but
they cover all four paths once neighbours are included:

| path | counted at |
|---|---|
| prev → p0 (outbound arriving) | **this** slave, p0 |
| next → p1 (return arriving) | **this** slave, p1 |
| p1 → next (outbound leaving) | **next** slave, p0 |
| p0 → prev (return leaving) | **previous** slave, p1 |

A slave never reports its own emissions. "Slave N created the damage" is always
an inference from slave N−1's counter.

### 4.1 Origin table, all runs

| run | chain | RETURN (p1) | OUTBOUND (p0) | inferred origin |
|---|---|---|---|---|
| **A** 10-slave, 8.25 h | 0–3 XCR, 4 CN, 5–6 EVE-NET, 7–9 CN | s0–s5 all **255 saturated** | s7,s8,s9 = 14 | outbound **s6 (EVE-NET)**; return unreadable |
| **B** 10-slave, 98 s | same | s0–s4 = 4; s5 fwderr 2, invalid 0 | none | **s5 (EVE-NET)** |
| **C** 12-slave, 191.9 s | 0–2 XCR, 3 EVE-NET/XCR, 4 XCR, 5 CN, 6 EVS-NET, 7–8 EVE-NET, 9–11 CN | s9,s10=7; s8=0; s7=13; s6=63; s5=66; s4=63; s3=63; s2–s0 **255 sat** | s9,s10,s11 = 7 | **s8 (+13)**, **s7 (+50)**, **s3 (≥192)** |
| **D** 5-slave truncated | 0–2 XCR, 3 EVE-NET/XCR, 4 XCR | **all zero** | **all zero** | none |
| **E** 5-slave reordered | 0 XCR, 1 EVE-NET/XCR, 2 XCR, 3 CN, 4 EVS-NET | s0 = 10 **only** | none | **s1 (EVE-NET)** |
| **F** 5-slave, EVE-NET first | 0 EVE-NET/XCR, 1–2 XCR, 3 CN, 4 EVS-NET | **all zero** | **all zero** | **s0 (EVE-NET)** — see below |

Run F is the strongest single attribution. Every ESC counter in the chain read
zero, yet the host logged one damaged frame. With the EVE-NET at position 0
there is no ESC between it and the master, so nothing existed to count the
frame: it went straight from the EVE-NET's port 0 to the NIC. The origin is
identified by the *absence* of any intervening counter rather than by a
gradient.

**Every traced origin is an EVE-NET.** Run E is the cleanest configuration
built: a single non-zero counter in the entire chain, one origin, one hop, no
propagation, no saturation. Ten frames emitted, ten counted, ten received, ten
captured.

### 4.2 Both directions, return far worse

Outbound damage is real but appears only in the longest chains, only at the tail
slaves, and at roughly **20× lower** counts than the return path. Its origin
also traces to an EVE-NET (s6 in run A, s8 in run C).

At the tail, the same frames are counted twice — on `p0` outbound and `p1` on
the way back (run C: slaves 9 and 10 both show 7 on both ports). This is a
useful sanity check on the port mapping.

---

## 5. Rate varies with chain arrangement — mechanism unknown

The same physical EVE-NET module, moved within the chain:

| run | EVE-NET at | devices downstream | events / 91 s | rate |
|---|---|---|---|---|
| D | pos 3 | 1 | 0 | 0 /s |
| E | pos 1 | 3 | 10 | 0.11 /s |
| **F** | pos 0 | **4** | **1** | **0.011 /s** |

**A downstream-depth hypothesis was proposed and then falsified.** Earlier
readings from the 12-slave run appeared to show emission scaling with the number
of slaves downstream (~0.07 /s at 3, ~0.26 /s at 4, ≥1.0 /s at 8). Run F was the
controlled test: four devices downstream, predicted 0.3–0.5 /s, **measured
0.011 /s** — an order of magnitude the wrong way, and non-monotonic against D
and E.

Two reasons the earlier trend should not have been trusted:

1. **Those points came from saturated counters.** In the 12-slave run the ESC
   counters nearest the master had pegged at 255, so three of the five points
   were floors, not measurements (§10.2).
2. **Every measurement is underpowered.** Poisson confidence intervals on the
   two controlled runs *overlap*:

   ```
   run F:  1 event  / 90.9 s -> 0.0110/s   95% CI [0.0003, 0.0613]
   run E: 10 events / 91.0 s -> 0.1099/s   95% CI [0.0527, 0.2021]
   ```

   One event is not a rate. Even the E-versus-F difference is only marginally
   supported, and D, E and F cannot be ordered at this power.

**What is established** is only that the rate varies by more than an order of
magnitude with chain arrangement, and that the variation is not explained by
downstream device count. What varies alongside position — upstream neighbour,
which devices sit downstream, cable identity, round-trip time — has not been
separated.

To settle it, each configuration needs **~20 events at the lowest rate**, i.e.
**30-minute runs**:

```bash
for p in 0 1 2 3 4; do sudo ./ecat_escreset -i enp2s0 -p $p --all --yes-write-to-slave; done
sudo ./ecat_ber -i enp2s0 -s 5 -d 1800 -o posX.csv -F posX
```

At the high-rate end 30 minutes is ~200 events, close to the 255 ESC ceiling.
The host-side count is what the rate comparison needs and it does not saturate;
keep the ESC counters for attribution on short runs.

## 6. Two device families

ESC identity registers separate the modules cleanly:

```
pos  device                   Type   Rev   Build   RAM    Features  PDI
0    EVS-XCR                  0x91   0x00  0x0000  16KB   0x01CC    0x08
1    EVE-NET on XCR carrier   0x90   0x01  0x04EE   8KB   0x008C    0x80
6    M400 + EVS-NET           0x91   0x00  0x0000  16KB   0x01CC    0x08
7    M400 + EVE-NET           0x90   0x01  0x04EE   8KB   0x008C    0x80
```

Type `0x91` devices additionally carry `"EtherCAT"` / `"TB-TR-EV"` ASCII at
`0x0900`/`0x0918`, SyncManager status bytes `0x30`, `0x0502 = 0x0080`,
`0x0150 = 0x6600`, `0x0110 = 0x5A37`, and populated user RAM at `0x0F80`.
Type `0x90` devices have none of those and half the process RAM.

**The EVS-NET is byte-identical to the EVS-XCR** across every identity
register — it is the XCR internals without the carrier.

`0x030D` (PDI error counter) write returns `wkc=0` on type-`0x90` devices only:
a reliable one-command family fingerprint.

### 6.1 What PDI type does and does not tell us

`0x0140` PDI type is `0x08` (16-bit asynchronous microcontroller interface) on
type-`0x91` and `0x80` (on-chip bus) on type-`0x90`. This describes the **style
of host interface**, not packaging: a single-die SoC with a hardware ESC block
on an internal µC-style bus reports `0x08` exactly as an external ASIC would.
Visual inspection of an EVS-XCR shows a single BGA, which is consistent.

What the registers do establish is that the two module types run **different
ESC implementations**. Whether that is different silicon, different IP, or
hard-IP-versus-firmware on the same family, these registers cannot say.

---

## 7. RX_ER is a marker, not the damage

`rxerr` appears only where the *transmitting* neighbour is a type-`0x91` device:

- run C: at s0←s1 (XCR), s1←s2 (XCR), s3←s4 (XCR), s5←s6 (EVS-NET)
- but **not** at s2←s3 (EVE-NET), s4←s5 (CN), s6←s7 (EVE-NET)
- run E: **no `rxerr` anywhere** — slave 0's only upstream neighbour is the EVE-NET

**Type-`0x91` devices assert RX_ER when forwarding a frame they have found bad;
EVE-NETs and third-party slaves do not.** This fully explains the `RECR`
gradient chased earlier in the investigation: RX_ER was never the damage, it
was the marker being regenerated by the XCRs, which is why it appeared near the
master and vanished mid-chain.

### 7.1 The trailing `0x50` is the same marking

```
run E  EVE-NET -> one EVS-XCR -> master     10 frames, ALL end 0x50
run F  EVE-NET -> master directly            1 frame,     ends 0x79
```

Remove the intervening type-`0x91` device and the trailing `0x50` disappears.
The marker byte tracked since the start of the investigation is **applied by the
EVS-XCR when it forwards a frame it has found bad**, not by the device that
caused the damage — the same story as `RX_ER`.

This is n=1 for the direct case, so it is suggestive rather than settled. It
makes a sharp prediction: with the EVE-NET last before the master, damaged
frames should not end in `0x50`.

---

## 8. What has been ruled out

Each of these was a working hypothesis that the data killed.

| hypothesis | how it died |
|---|---|
| **Duplex mismatch** | Controlled A-test with mismatch verified present (PHYSTS bit2 = 0) produced zero loss in 5.37 M frames. |
| **Frame duplication** | Across 393,971 RX and 393,942 TX frames in a full `ETH_P_ALL` capture, every sequence number appears exactly once. No frame ever returned twice. |
| **EEE / LPI wake, or any fixed timing constant** | K is exponentially distributed, not clustered. Events landed 6.3–18.7 s after the nearest idle gap, with the previous frame only 20–134 µs earlier — the wire was saturated. |
| **Fast Link Down firing** | `FLDS = 0x0000` on all 8,676 probe reads over 8.25 h. Every ESC lost-link counter zero. Host carrier transitions zero. |
| **Signal integrity at the faulting hop** | `RECR = 0` on both EVE-NET PHYs across 723 probes / 8.25 h, while slaves 0/1/2 recorded thousands. The counter was proven working on that exact PHY by a deliberate shorted-pair test (`RECR = 67`, `FLDS = 0x08`). The PHYs see clean symbols; the damage is downstream of the PHY, inside the device. |
| **Emission scales with downstream device count** | Run F: four devices downstream, predicted 0.3–0.5 /s, measured 0.011 /s. Non-monotonic against runs D and E. The earlier trend came from saturated counters (§5). |
| **The custom M400 carrier is required** | Run E's sole emitter is an EVE-NET on **Novanta's own XCR carrier**. The M400 carrier is not necessary for the fault. Whether it makes it worse is still open. |

---

## 9. The `0x0E00` vendor register block

Present **only** on type-`0x90` devices, absent on type-`0x91`:

```
0x0E00 : 32-bit frame counter   <- confirmed
0x0E04 : 32-bit counter, tracks 0x0E00 exactly under clean traffic
0x0E08 : 0x00800301
0x0E10 : 0x0098 / 0x0098
0x0E18 : 0x20000201
0x0E1C : 0x000000FF
0x0E20 : 117,184,368
```

`0x0E00` is **confirmed to the single frame**, three times. In one test it
advanced by 421,595 on all three devices against a run of 421,592 frames plus
exactly 3 single-frame register reads. In another, increments between dumps
matched the 6-frame cost of each intervening `ecat_regdump` exactly.

It is 32-bit (will not saturate for months), readable with plain APRD, and
costs one frame to sample.

`0x0E04` tracks `0x0E00` one-for-one while nothing goes wrong; the gap grows
only during damage (one run: +1,416 / +748 / +0 across three devices, ordered
the same way as the ESC gradient). **Its exact definition is not established** —
the growth is 20–70× larger than the invalid-frame counts, so it is not simply
a bad-frame counter.

---

## 10. Instrumentation corrections

Two defects in the measurement chain invalidated earlier data. Both are fixed;
results from before them are not comparable with results after.

### 10.1 The RX socket discarded 95% of the evidence

The RX socket was bound to `ETH_P_ECAT`, so the kernel only delivered frames
whose bytes 12–13 held `0x88A4`. A prefix-chopped frame has *payload* there, so
protocol dispatch dropped it before delivery — counted in `netdev rx_dropped`
and never seen.

Measured before the fix: three prefix-chopped frames on the wire (confirmed by
a concurrent `ETH_P_ALL` tcpdump), `rx_dropped` = 3, delivered = 0. After:
`rx_crc_errors` = 4, delivered = 4, `rx_dropped` = 0.

This also explains the long-standing "the NIC drops ~94% of corrupt frames"
puzzle. It never did. Frames marked bad but left full-length keep their
EtherType and were always delivered — that was the visible ~5%. `rx-all` was
working the whole time; our own socket filter was the blind spot.

**Consequence: corruption counts from before this fix undercount by ~20×.**

### 10.2 ESC error counters saturate at 0xFF

The 8-bit counters peg at 255 and stop. In the 8.25 h run most saturated within
the first 45 minutes, so **every ESC event count in that dataset is a floor,
not a measurement**, and the device nearest the origin was invisible because its
neighbour's counter was full.

Clear them before every run:

```bash
for p in $(seq 0 11); do sudo ./ecat_escreset -i enp2s0 -p $p --all --yes-write-to-slave; done
```

At ~1.4 events/s a 90-second run stays comfortably under the ceiling.

---

## 11. Open questions

1. **What sets the emission rate?** The same module measures 0/s, 0.011/s and
   0.11/s in different chain arrangements, and the obvious candidate —
   downstream device count — is falsified (§5). Upstream neighbour, downstream
   device *types*, cable identity and round-trip time are all still confounded
   with position. Needs 30-minute runs per configuration before any model is
   worth proposing.
2. **What is `0x0E04`?** Behaves like a damage-related counter but the
   magnitudes do not match any known quantity.
3. **What is the trailing `0x50` byte?** Constant across every damaged frame
   ever captured. Presumed part of the bad-frame marking; not confirmed.
4. **Is the field link-drop symptom the same fault?** This corruption is Poisson
   and has never once dropped a link. The field report describes regular-interval
   link drops. They may be unrelated.
5. **Does the M400 carrier make it worse?** The fault occurs without it. A
   controlled A/B at matched downstream depth has not been run.

---

## 12. How to measure it

Standard configuration — run E, the cleanest rig built:

```
0: EVS-XCR        1: EVE-NET (XCR carrier)      2: EVS-XCR
3: third-party    4: EVS-NET (M400 carrier)
```

```bash
for p in 0 1 2 3 4; do sudo ./ecat_escreset -i enp2s0 -p $p --all --yes-write-to-slave; done
sudo ./ecat_ber -i enp2s0 -s 5 -d 300 -o run.csv -F run
```

Read afterwards:

- `run.csv` last row — per-slave, per-port ESC counters; the step increase names
  the origin
- `run/frames.pcap` — every damaged frame, full bytes; `K = 1519 − len`
- `run/events.csv` — one row per ESC counter change, timestamped
- `run/probes.txt` — DP83822 registers captured within ~200 ms of each burst

Use 300 s rather than 90 at low event rates: ~30 events gives a comparable rate
while staying far below the 255 ceiling.
