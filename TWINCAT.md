# TwinCAT reference behaviour, and the link drops it recorded

Analysis of `TwinCAT - Wireshark Test_01 - 20260923.pcapng`: 42.64 s of a
TwinCAT master running two M400+EVE-NET drives, captured 2026-09-23 on a
separate Windows PC. A screenshot of the TwinCAT online view taken just after
the capture accompanies it.

The capture matters for two reasons. It is the only recording we have of the
**link-drop symptom** — the field fault, which our own rig has never
reproduced. And it is a byte-exact specification of how a real master drives
these drives, which is what `ecat_op` replays.

Written to be read by someone who has not followed the work. Every number
comes from the file; where something is inferred rather than measured, it
says so.

---

## 1. Summary

Two Everest NET drives, both in OP, exchanging process data on a 2.016 ms
cycle. **The link between the two drives dropped three times in 25.4 s of
running** — one every 8.5 s — and recovered identically each time after
exactly 3.00 s.

The drops have no precursor of any kind. Not one corrupted frame occurs
anywhere in the capture: every invalid-frame and RX-error counter on both
drives reads zero throughout. The drive is fully present in one 2 ms cycle
and completely absent in the next.

This is **not** the frame-truncation fault recorded in `FINDINGS.md`. That
fault always produces invalid-frame counts and has never dropped a link; this
one drops links and never corrupts a frame. They are independent.

---

## 2. Method — how the behaviour was established

The capture cannot be taken at face value, and three of the conclusions below
were reached only after an earlier reading of the same file turned out to be
wrong. The techniques that settled each question are worth recording.

### 2.1 What the file is

pcapng, nanosecond timestamps, 58,854 packets, no snap length, strict time
order. Parsed directly (a small Python reader) and cross-checked with
`tshark`. Both directions of the link are present:

| source MAC | meaning |
|---|---|
| `c8:f7:50:50:a0:ff` | master → slaves (29,658 frames) |
| `01:01:05:01:00:00` | returning (29,196 frames) |

### 2.2 The sequence counter — proof the capture is complete

**Byte 30 of every cyclic frame is the LRW datagram's index, and TwinCAT
increments it by one per frame.** It is the single most useful field in the
file.

Across all 25,191 captured cyclic frames the index steps by exactly `+1`
**25,189 times** out of 25,190. In the 12,193 frames before the operator stop
(§5.4) the index advances by 12,192 — **not one frame is missing**.

This was found late. Before it, completeness was estimated from average frame
rates, which gave 30–40% loss; a second attempt gave ~99%. Both were wrong,
because the averages spanned an 8.5 s period when the master was stopped and
8.7 s before it started. *If a protocol carries a per-frame counter, read it
before reasoning about rates.*

### 2.3 The capture is two streams, merged — and what that invalidates

An earlier draft argued that the two cyclic frames must be back-to-back
because the capture shows both going out before either response returns
(`TX TX RX RX`), and a capture may mis-timestamp but does not reorder.

**That reasoning is wrong.** Grouping frames into batches and looking at the
direction pattern:

```
   TTRR          10882
   TTTTRRRR       1085
   TTRRR           215
   ...
   12921 of 12922 batches (100.0%) direction-grouped, never interleaved
```

On the wire TX and RX must alternate — send, ~14 µs later it returns, send
the next: `T R T R`. The capture shows `T T R R` in **100%** of batches. That
is impossible as wire order, and proves the file is **two separate streams
merged**: the tap has one port per direction, the capture host reads them as
independent sources and timestamps them at read time in per-direction blocks.

Consequences, all of them limiting:

- **The apparent index "lag" is this artefact, not protocol.** `IDX` is a
  master-only field — ETG.1000.4 says slaves shall not change it, and the
  capture confirms it: 25,177 returning frames, every one carrying an index
  the master sent, zero unmatched. A response with index 185 *is* the
  response to request 185. The TX block of a batch simply holds frames
  186-187 while its RX block holds the returns for 185-186.
- **Round-trip times are unusable.** Exactly 1,991 frames show a sensible
  13.5 µs and exactly 1,991 are inflated by precisely one cycle (2012.6 µs),
  a perfect 50/50 split by index parity. The same frame's `LRD` and `BRD`
  datagrams show 14.6 µs while its `LRW` shows 1014 µs — one frame cannot
  have two round trips.
- **The relative order of any TX and any RX frame is meaningless.** Every TX
  in a batch precedes every RX in the file regardless of what happened on the
  wire.
- **Intra-cycle structure is unresolvable.** 992 cyclic frames/s is the solid
  number, from counting. Whether that is two frames back-to-back every
  2.016 ms or one frame every ~1.008 ms **cannot be determined from this
  file**, and TwinCAT's own "973 frames/s" is consistent with either.

### 2.4 The response-latency test — withdrawn

An earlier draft used response latency to argue that acyclic frames appearing
at the tail of a burst were really transmitted in the next cycle, and used it
to dismiss an association between the drops and a link-status poll (§6.7).
Given that half of all round-trip times are inflated by exactly one cycle as
an artefact of §2.3, the test has no force. The association it dismissed is
addressed properly in §6.7.

### 2.5 What the capture cannot tell us

Within a burst the median frame spacing is **0.6 µs**, but a 77-byte frame
occupies **7.8 µs** on a 100 Mbit link. The capture path is gigabit: 77 bytes
at 1 Gbit is 0.62 µs plus inter-packet gap ≈ 0.71 µs, which matches. The
mirror forwards frames at gigabit, compressing transmission time while
leaving the long idle gaps intact.

Consequences:

- **Inter-burst timing is reliable** — 2 ms gaps are far larger than any
  re-timing error.
- **Intra-burst timing and ordering are not.** Whether an acyclic frame sits
  between the two cyclic frames or after them cannot be determined from this
  file. An earlier draft asserted "between"; it is unsupported.
- **True inter-packet gap is unmeasurable.** That frames are back-to-back
  follows from §2.3, not from the timestamps.

---

## 3. The system under test

Two drives, both ESC type `0x90` (EVE-NET), station addresses 1001 and 1002,
positions 0 and 1, directly on the master — no other devices in the chain.

From the SII reads TwinCAT performs at startup (words 8 and 10):

```
vendor ID     0x0000029C
product code  0x02C10001
```

Process data, 11 bytes each way per drive, from the PDO mapping TwinCAT
downloads (§5.2):

| direction | object | contents |
|---|---|---|
| inputs `0x1A00` | statusword `0x6041` (16b), position actual `0x6064` (32b), velocity actual `0x606C` (32b), modes display `0x6061` (8b) | 88 bits = 11 B |
| outputs `0x1600` | controlword `0x6040` (16b), target position `0x607A` (32b), target velocity `0x60FF` (32b), modes of operation `0x6060` (8b) | 88 bits = 11 B |

Every returning frame in steady state carries the same input data:

```
5002 aa0a0000 00000000 ff        per drive
 |     |        |         `- modes display 0xFF
 |     |        `----------- velocity actual 0
 |     `-------------------- position actual 0x0AAA = 2730
 `-------------------------- statusword 0x0250 = 592
```

Statusword `0x0250` decodes as "switch on disabled" — the drives are powered
and operational at the fieldbus level but not enabled. **No motors are
connected on this rig.** The value 592 appears in the TwinCAT screenshot,
which independently confirms the mapping decode.

---

## 4. Steady-state traffic

### 4.1 The cycle

**992 cyclic frames/s**, matching the 973/s TwinCAT reports in its own
counters. Frames appear in the capture as pairs 2.016 ms apart, but §2.3
shows the capture cannot resolve the spacing *within* a pair: this is equally
consistent with two frames back-to-back every 2.016 ms or one frame every
~1.008 ms. The frame rate is solid; the cycle time is not.

Each cyclic frame is **77 bytes** and carries three datagrams:

| datagram | address | length | purpose |
|---|---|---|---|
| `LRD` | `0x09000000` | 1 B | mailbox state, one bit per slave via FMMU 2 |
| `LRW` | `0x01000000` | 22 B | process data, 11 B per drive |
| `BRD` | `0x0130` | 2 B | AL status of all slaves |

The `BRD` is useful beyond its data: **its returning ADP field equals the
number of slaves that processed it**, which is an immediate slave count. It
reads 2 before each drop and 1 after.

### 4.2 Acyclic traffic

Over 18.8 s of healthy operation — outages and the operator stop excluded —
TwinCAT sends only **three** kinds of acyclic frame:

| frame | rate | period |
|---|---|---|
| `FPRD 0x0300` ×2 — read error counters, both drives | 7.87/s | 127 ms |
| `BWR 0x0300` — clear error counters, broadcast | 7.87/s | 127 ms |
| `APRD 0x0310` ×2 — lost-link counters | 4.04/s | 247 ms |

**Total 19.8 acyclic frames/s against 992 cyclic.** So roughly 98% of cycles
are exactly two frames, and a burst of four requires the read-and-clear pair
to coincide — about 1.6% of cycles.

Notably absent: **TwinCAT does not poll link status in steady state at all.**
`FPRD 0x0111` appears only while a link is down, when it runs every cycle.
Zero healthy bursts contain one.

It also clears the error counters eight times a second, which is why its CRC
column reads zero — any count is erased within 127 ms.

---

## 5. Bring-up

### 5.1 Shape

About 40 register writes plus 8 CoE downloads, ~90 ms from reset to OP. The
full sequence is in `tests/twincat_seq.h`, extracted datagram by datagram;
`ecat_op` replays it and `t_opseq` asserts byte equality against it.

Phases: clear FMMUs/SyncManagers/DC → INIT with error acknowledge → SII
identity read → station addresses → mailbox SyncManagers → PREOP → PDO
mapping by SDO → process-data SyncManagers → FMMUs → SAFEOP → OP.

### 5.2 Mailbox details that matter

- The mailbox counter in the type byte **increments per message** (0…7).
- A payload of 4 bytes or fewer goes **expedited** (command specifier `0x33`,
  data in the size field); longer payloads go normal (`0x31`).
- The request is written to `0x1000`, then the **last byte of the buffer**
  (`0x107F`) is written separately, which is what marks the mailbox full.
- The response is read as the **full 128-byte buffer** from `0x1400`. A short
  read never reaches the buffer's last byte, so the mailbox is never released
  and the drive stops accepting requests.

Each of these was learned by getting it wrong first; see §8.

### 5.3 Distributed clocks

TwinCAT writes `0x0910`, `0x0930`, `0x0934` and sets `0x0981 = 0`, which
**disables SYNC signal generation**. There is no DC synchronisation in this
configuration.

### 5.4 An operator stop, not a fault

Between t=21.079 and t=29.619 the cyclic traffic stops for 8.54 s while
acyclic frames continue, so the capture is alive. Both drives fall out of OP
on their own — the SM2 watchdog expires without process data — and at
t=29.615 TwinCAT drives both back through PREOP → SAFEOP → OP.

Drive 1's **port 0** lost-link counter also advances by 2 in this window,
which an earlier draft reported as a fourth link drop. It is almost certainly
the master's NIC bouncing when the configuration was stopped and restarted.
**Three drops, not four**, all on the drive-to-drive link.

---

## 6. The link drops

### 6.1 Rate

Three drops in **25.39 s** of at-risk time (33.93 s of cyclic traffic minus
the 8.54 s stop): **0.118/s, one every 8.5 s.**

An earlier figure of one per 14.2 s divided by the full 42.64 s of the file,
including time when nothing was running. The corrected rate is 1.7× higher.

| drop | t (s) | interval |
|---|---|---|
| 1 | 16.057005 | — |
| 2 | 31.401068 | 6.80 s at risk |
| 3 | 39.573875 | 8.17 s |

### 6.2 The packets before drop 1

Everything on the wire from 16.0400 to 16.0600 s. `wkc` is the working
counter on the returning frame; `slaves` is the returning `BRD` ADP.

```
16.040902 TX 77B  LRD 0x09000000 | LRW 0x01000000 | BRD 0x0130
16.040902 TX 60B  FPRD 1001/0x0300 | FPRD 1002/0x0300
16.040903 TX 77B  LRD | LRW | BRD
16.040903 TX 60B  BWR 0/0x0300
16.040914 RX 77B  wkc 2/6/2  slaves=2  AL=0x0800
16.040914 RX 77B  wkc 2/6/2  slaves=2  AL=0x0800
16.040914 RX 60B  FPRD 0x0300 wkc=1|1      <- both drives, counters all zero
16.042959 TX 77B  LRD | LRW | BRD
16.042959 TX 77B  LRD | LRW | BRD
16.042966 RX 77B  wkc 2/6/2  slaves=2  AL=0x0800
16.042966 RX 60B  BWR 0x0300 wkc=2         <- counters cleared
16.042966 RX 77B  wkc 2/6/2  slaves=2  AL=0x0800
16.044858 TX 77B  LRD | LRW | BRD
16.044858 TX 77B  LRD | LRW | BRD
16.044859 TX 60B  APRD 0/0x0310 | APRD 65535/0x0310
16.044872 RX 77B  wkc 2/6/2  slaves=2  AL=0x0800
16.044872 RX 77B  wkc 2/6/2  slaves=2  AL=0x0800
16.046879 TX 77B  LRD | LRW | BRD
16.046880 TX 77B  LRD | LRW | BRD
16.046893 RX 77B  wkc 2/6/2  slaves=2  AL=0x0800
16.046893 RX 60B  APRD 0x0310 wkc=1|1  [0000]|[0000]   <- lost-link still 0
16.046894 RX 77B  wkc 2/6/2  slaves=2  AL=0x0800
16.048973 TX 77B  LRD | LRW | BRD          } four further cycles,
16.048973 TX 77B  LRD | LRW | BRD          } every one identical:
16.048998 RX 77B  wkc 2/6/2  slaves=2      } wkc 2/6/2, AL 0x0800,
16.048998 RX 77B  wkc 2/6/2  slaves=2      } PD 5002aa0a...
16.050885 TX 77B  ...
16.050886 TX 77B  ...
16.050897 RX 77B  wkc 2/6/2  slaves=2
16.050897 RX 77B  wkc 2/6/2  slaves=2
16.053100 TX 77B  ...
16.053100 TX 77B  ...
16.053113 RX 77B  wkc 2/6/2  slaves=2
16.053113 RX 77B  wkc 2/6/2  slaves=2
16.054861 TX 77B  ...                      <- last normal cycle
16.054862 TX 77B  ...
16.054877 RX 77B  wkc 2/6/2  slaves=2  AL=0x0800  PD=5002aa0a...5002aa0a...
16.054878 RX 77B  wkc 2/6/2  slaves=2  AL=0x0800  PD=5002aa0a...5002aa0a...

16.056990 TX 77B  LRD | LRW | BRD          <- next cycle, sent normally
16.056990 TX 77B  LRD | LRW | BRD

16.057005 RX 77B  IRQ=0x0004  wkc 1/3/1  slaves=1  AL=0x0800
                  PD = 5002aa0a000000000000ff 0000000000000000000000
16.057005 RX 77B  IRQ=0x0004  wkc 1/3/1  slaves=1
```

Read that last frame carefully:

- Working counters fall **2→1** (LRD), **6→3** (LRW), **2→1** (BRD) — drive 2
  contributed nothing.
- The returning `BRD` ADP is **1**: only one slave saw the frame.
- Drive 2's 11 process-data bytes come back **all zero** — untouched, as the
  master sent them.
- Drive 1's data is unchanged, and its AL status is still `0x0800` (OP).
- **`IRQ = 0x0004`** is set in every datagram, the first non-zero IRQ in the
  capture.

**The elapsed time from the last wholly normal frame to the drop frame is
2.127 ms — one cycle.** Drops 2 and 3 give 2.003 ms and 2.000 ms.

### 6.3 What is *not* there

The ten frames before a drop are not merely *similar* to ordinary traffic.
**They are the only frame TwinCAT ever sends.** Masking the one index byte,
all **15,424** healthy returning cyclic frames in the capture reduce to
**exactly one distinct 77-byte frame** — same working counters 2/6/2, same AL
status `0x0800`, same process data `5002aa0a000000000000ff` twice over. The
transmitted frames are likewise identical apart from the counter: controlword
0, target position and velocity 0, every cycle.

So the answer to "how often does that ten-frame sequence occur?" is: **every
ten-frame window in the healthy capture**, about 15,400 of them.

That converts a weak claim into a strong one. It is not that we looked for a
precursor and found none — **the traffic contains one distinct frame, so a
precursor cannot exist in it.** There is no information in the EtherCAT
stream, outside a counter, that differs before a drop from any other moment.

The index also confirms nothing was lost at the drop itself. Indices step by
+1 across all three, with no gap:

```
drop 1:  185, 186, 187 -> 188, 189, 190
drop 2:   94,  95,  96 ->  97,  98,  99
drop 3:   18,  19,  20 ->  21,  22,  23
```

Every frame went out and came back; drive 2 stopped contributing to them
mid-cycle. The index values at the drops show no pattern — no wrap boundary.

No degraded frame, no working-counter blip, no status bit, no counter
movement. And in the whole capture:

- invalid-frame counters (`0x0300`): **zero**
- RX-error counters (`0x0301`): **zero**
- forwarded-error counters (`0x0308`): **zero**

The link goes away without a single corrupted frame.

### 6.4 The IRQ flag

`IRQ = 0x0004` appears in 132 returning datagrams, in exactly **10 windows**,
and nowhere else:

| when | duration |
|---|---|
| each of the 3 drops, at t+0 | ~2 ms |
| each drop +2.0 s — the physical link returning | ~2 ms |
| each drop +3.0 s — TwinCAT reopening the port | 2–4 ms |
| the operator stop | 2.5 s |

Zero false positives in 42 s. It is the ESC's AL-event flag ORed into the
datagram as the frame passes, and it tracks **DL-status transitions exactly**.
It is how TwinCAT learns of the drop in the same frame that shows the reduced
working counter, and why it reads `0x0111` on the next cycle.

The specific bit is read as the DL-status-change bit of AL Event Request
(`0x0220`). *That bit assignment is not verified against the ESC datasheet.*
What is measured is that the flag tracks link transitions perfectly.

### 6.5 Which link, and who removed the drive

The lost-link counters settle it. After each event **drive 1 port 1 and drive
2 port 0 both advance by 2**, symmetrically:

```
t=19.10   drive 1: p0=0 p1=2      drive 2: p0=2 p1=0
t=34.54   drive 1: p0=2 p1=4      drive 2: p0=4 p1=0
t=39.61   drive 1: p0=2 p1=6
```

That is the **EVE-NET ↔ EVE-NET link**, seen from both ends. (Drive 1's port 0
advance is the operator stop, §5.4.)

Drive 1's ESC removes drive 2 **by itself**. Port loop control was set to
`0xF4` (auto-close), so the ESC closes port 1 the moment the physical link
fails — which is why the very next frame returns with one slave, 2 ms before
TwinCAT writes anything.

The screenshot's `Reg:0310 = 0x0A02` reads as port 0 = 2, port 1 = **10**. The
capture accounts for 6 of those 10, so about two further drops happened after
the capture stopped — one of which left Drive 2 in `INIT NO_COMM`.

### 6.6 Recovery

Identical to the millisecond on every drop:

| step | drop 1 | drop 2 | drop 3 |
|---|---|---|---|
| TwinCAT forces port 1 closed (`0x0101 = 0xFC`) | +2.0 ms | +2.0 ms | +2.0 ms |
| physical link returns (`0x0111` bit 3 set) | **+2.0012 s** | **+2.0008 s** | **+1.9842 s** |
| TwinCAT reopens the port (`0x0101 = 0xF4`) | **+3.0011 s** | **+3.0010 s** | **+2.9841 s** |
| drive 2 reads AL status INIT, re-initialised to OP | ~+40 ms | ~+40 ms | ~+40 ms |

Two constants, both to the millisecond:

- **2.000 s** from drop to physical link return. That is the PHY
  re-establishing the link — auto-negotiation.
- **1.000 s** from link return to port reopen. A TwinCAT debounce timer.

Total outage **3.00 s** every time. During it, TwinCAT polls `0x0111` every
cycle and drives the remaining slave normally.

### 6.7 The link-status poll adjacent to every drop

In all three drops the last frame the master sends before the drop frame is
`FPRD 1001/0x0111`, a single-datagram 60-byte read of drive 1's DL status —
at −0.014, −0.020 and −0.014 ms. Nothing else in the preceding 20 frames is
anything but the cyclic frame.

It is a real association and it deserves care, because TwinCAT sends
`0x0111` **only** when a link is down: 3,052 of them in the capture, all
inside the three outage windows, **none at all in steady state**. The first
poll of each outage therefore sits adjacent to its drop.

**The capture cannot order the poll against the drop.** They are 14–20 µs
apart, and §2.3 shows that every TX frame in a batch is listed before every
RX frame regardless of wire order. The poll is a TX frame, the drop is an RX
frame, they share a batch — so the poll would appear first *even if it had
been sent afterwards*. No re-analysis of this file can resolve it.

What can be said:

- **The poll alone does not cause drops.** `ecat_op` sent 178,560 of them to
  station 1001 — the first EVE-NET — in the 1,800 s run, with zero drops.
- **A reaction explains it economically**: the IRQ flag arrives with the
  cyclic response (§6.4), TwinCAT reads `0x0111` on learning of it, and that
  is why the poll rate is zero except during outages.

Settling it needs a capture with **wire-accurate timestamps and a single
merged stream** — a passive tap into a NIC with hardware timestamping, or a
logic analyser on a PHY's MII pins. The mirror used here cannot.


---

## 7. What it means, and what it rules out

**The signature fits Fast Link Drop.** `FINDINGS.md` §6.2 records that the
EVE-NET is the only device in our chain with FLD armed (`CR3 = 0x1009`),
triggering on 32 RX_ER in a 10 µs window, or on signal/energy loss. **Neither
requires a frame to be in flight.** RX_ER bursts during the IDLE stream
between frames would trip FLD without incrementing any ESC counter — which is
exactly what is observed: a link that vanishes with every error counter at
zero.

**No further analysis of this capture, or any capture like it, can find the
trigger** (§6.3): the traffic holds a single repeated frame. Any precursor
lies outside the EtherCAT layer — in the PHY's own state, in symbol errors
during the IDLE stream between frames, or in the electrical domain.

**The evidence that would identify the cause is not visible at the EtherCAT
layer at all.** It is in `FLDS` (`0x000F`) on the PHY, which latches which
criterion fired. TwinCAT never reads that register. `ecat_op` probes it within
~200 ms of a lost-link increment, which is the entire reason that tool exists.

**What our own rig has ruled out.** At 0.118 drops/s, our OP runs expected
**726** drops and saw **zero** (P = 1×10⁻³¹⁵ over 6,144.7 s pooled; exact
one-sided 95% upper bound 4.9×10⁻⁴/s, a 242× separation). That covers: both
drives in OP, exchanging process data, at TwinCAT's cadence, with bursts of 1,
4 and 8. So neither the operational state nor the traffic shape provokes it.

At the *faithful* traffic shape specifically — 1 kHz, one frame per cycle, the
three acyclic jobs at their measured periods with ±5 ms jitter — we now have
**744.7 s**, which expected 88 drops and saw zero (P = 1×10⁻³⁸). The full
accounting, and the two experiments still outstanding, are in `FINDINGS.md`
§10.

**What is left.** The hardware is stated to be identical and from the same
batch. The remaining differences between the two rigs are:

1. **Topology.** TwinCAT's two drives sat directly on the master. Ours are at
   positions 5 and 6 of an 11-slave chain, so traffic reaches them after five
   hops through EVS-XCRs, each re-timing it with its own PHY.
2. **The cable** between the two drives.
3. The physical installation and its electrical environment.

---

## 8. Corrections log

Recorded because several of them were caught only by a second measurement,
and the same traps will recur.

| claim | how it failed |
|---|---|
| "The capture is 30–40% incomplete" | Inferred from average rates spanning an 8.5 s operator stop. The LRW index proves **zero** frames missing. |
| "…~99% complete" | Second attempt, same method, still wrong. |
| "Acyclic frames sit between the cyclic pair" | Read from intra-burst ordering the gigabit mirror destroys. Unsupported. |
| "Link status is polled at 101/s" | Entirely outage traffic. Steady state has **none**. |
| "All 3 drops coincide with a link-status poll, p ≈ 7×10⁻⁸" | The poll's response returns 2 ms late, proving it was sent *after* the drop. Artefact. |
| "A fourth drop on the master link" | The operator stop, §5.4. |
| "One drop per 14.2 s" | Divided by 42.64 s including 8.5 s when nothing ran. Correct rate **one per 8.5 s**. |
| "Bursts of 4 match TwinCAT" | 98% of cycles are 2 frames. A burst of 4 needs two acyclic jobs to coincide, ~1.6% of cycles. |
| "Both cyclic frames go out before either response returns, so they are back to back" | The capture is two streams merged: 100% of batches are direction-grouped `TTRR`, never interleaved. TX-versus-RX order carries no information, and the cycle time is unresolvable. |
| "The 2 ms response latency proves the poll was sent a cycle later" | Half of all round trips are inflated by exactly one cycle as the same artefact. Test withdrawn; see §6.7. |
| "Our OP runs expected 449 drops (P = 6×10⁻¹⁹⁶)" | Counted before `opdrop.log` and not reproducible from the logs on disk. Re-derived from the three `Elapsed:` lines: 6,144.7 s, **726** expected. |
| "At the faithful traffic shape we have only 114 s" | That run sent *two* cyclic frames per cycle — the then-current reading of the capture. After the 1 ms / one-frame correction the faithful figure is `opdrop.log`'s **744.7 s**; the 114 s is a different shape and is not pooled with it. |
| "63.6% of returning frames match the pre-drop pattern" | Diluted by outage frames, and "byte-identical" ignored the index byte. Within healthy operation it is **100%** — one distinct frame, 15,424 times. |

---

## 9. Reproducing the analysis

The capture is at `~/Downloads/Wireshark Captures/`. Ubuntu's AppArmor
profile blocks `tshark` from reading `$HOME`; either copy the file to `/tmp`
or add `owner @{HOME}/**.pcap{,ng}{,.gz} r,` to `/etc/apparmor.d/local/tshark`
and reload with `apparmor_parser -r /etc/apparmor.d/tshark`.

Useful starting points:

- direction: `eth.src == 01:01:05:01:00:00` is the returning frame
- the sequence counter: byte 30 of a 77-byte frame
- working counter: `ecat.cnt` (there is no `ecat.wkc` field)
- drops: the first returning `LRD`/`LRW` whose working counter falls below 2
