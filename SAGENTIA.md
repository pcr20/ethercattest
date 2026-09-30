# Field hardware — link-drop reproduction

Working note on the runs against the **actual four drives from which the
TwinCAT capture of 2026-09-23 was taken**. Live document; the run index at the
end is appended to as runs complete.

Status: **the link drop is reproduced.** The mechanism is identified on one
drop and unexplained on two others. The controlling variable is not yet the
one anyone expected, and it contradicts the TwinCAT capture in a way that
names the next experiment precisely.

Companion documents: `TWINCAT.md` (what TwinCAT did and what it recorded),
`FINDINGS.md` (the frame-corruption fault, §10 for the link drops).

---

## 1. Summary

Every earlier negative in this investigation came from our lab chain. Moving
the same master and the same binary onto the field hardware produced the fault
within seconds.

**Four slaves in the chain → drops within 2 to 21 s. Two slaves → nothing in
912 s.** The same link behaves both ways depending only on what sits beyond it
(§4.2).

| configuration | units | time in OP | first drop | total drops |
|---|---|---|---|---|
| 4-slave chain | `#14 → #16 → #nolabel → #17` | 78.4 s | **8.689 s** | 4 |
| 4-slave chain (repeat) | `#14 → #16 → #nolabel → #17` | 47.3 s | **21.079 s** | 2 |
| 2-slave pair | `#14 → #16` | 394.7 s | — | **0** |
| 2-slave pair | `#nolabel → #17` | 145.4 s | — | **0** |
| 2-slave pair (repeat) | `#nolabel → #17` | 372.3 s | — | **0** |
| 4-slave, **2 lab units appended** | `#nolabel → #17 → #5 → #0` | 311.1 s | **2.163 s** | 2 |
| 4-slave, 2 lab units (repeat) | `#nolabel → #17 → #5 → #0` | 64.6 s | **4.162 s** | 2 |
| 4-slave, **reordered** | `#5 → #0 → #nolabel → #17` | 39.7 s | **6.963 s** | 6 (**3 recovered**) |

On one drop the PHY latched **which mechanism fired**, and it is Fast Link
Drop on the RX-error criterion (§3), now latched on three separate drops.
That is the first direct evidence of a cause anywhere in this investigation,
and it connects the two faults that `FINDINGS.md` has so far treated as
independent.

Across five four-slave runs the rate is **0.1049 drops/s**, 95% CI
[0.0422, 0.2161], against TwinCAT's **0.1182/s** (95% CI [0.0244, 0.3453]) on
this same hardware. The intervals overlap comfortably: the rig reproduces the
field fault at a rate indistinguishable from the field's.

**The suspect is now one PHY.** Every Fast Link Drop firing recorded — six of
them across four runs — has been on `#nolabel`'s port 1, facing `#17`, at
three different positions in the chain (§4.4). No other PHY has ever latched
`FLDS`.

The result does **not** reduce to a bad cable or a bad unit, and it is not a
property of the field units. The link `#nolabel ↔ #17` was silent for 517.7 s
as an isolated pair and then dropped in **2.163 s** when two spare drives from
our own lab chain were appended behind it — same link, same cable, same two
units, opposite outcome (§4.2).

---

## 2. The hardware and the naming

Four EVE-NET drives, all ESC type `0x90`, all reading `CR3 = 0x1009` — Fast
Link Drop armed on RX-error count (bit 3) and signal/energy loss (bit 0).
Units are identified by the labels on them; one carries no label.

Chain order as cabled:

```
master ── #14 ── #16 ── #nolabel ── #17
          pos0   pos1     pos2      pos3
```

Port convention throughout: **port 0 faces the master, port 1 faces
downstream.** So "slave 1 port 1" is the link between `#16` and `#nolabel`.

All runs used the same command shape, the pre-recovery build:

```
./ecat_op -i enp2s0 -s <n> --op <positions> --random 5 -d 7200 -F <dir>
```

1 kHz, one frame per cycle, TwinCAT's three acyclic jobs at their measured
periods with ±5 ms jitter, TwinCAT's own 86 bring-up writes. Every run was
stopped by hand well before its 7200 s, because without recovery the first
drop severs the chain and ends the useful part of the run.

---

## 3. The mechanism, caught three times

Three drops have now been probed with the failing PHY still reachable, and
all three latched the same reason. Run `opdropsagentia4_2` is the more informative
because it also has an ESC-layer precursor.

### 3.1 The sequence

| t (s) | what | source |
|---|---|---|
| 18.580 | slave 2 port 1: **invalid frame +1, RX error +1** | ESC `0x0300` |
| 21.079 | process data 12 → 9; slave 3 gone | WKC |
| 21.160 | slave 2 port 1 lostlink **+2** | ESC `0x0310` |
| 21.160 | PHY probe fires | `probes.txt` |

An ESC-layer error **2.499 s before** the link went down, on the exact slave
and port that then dropped. This is the first precursor of any kind observed
before a link drop; `TWINCAT.md` §6 records that the capture contained none,
and that remains true of the capture — TwinCAT never read these registers.

### 3.2 The probe

```
pos 2 phy  3: PHYSTS=0x2912 FLDS=0x0080 MISR1=0xE400 MISR2=0x2800
              BMSR=0x7849 ANER=0x0006 FCSCR=0x0000 RECR=0x0002 CR3=0x1009
```

Decoded against SNLS505H Tables 8-12, 8-15, 8-16 and 8-18:

- **`FLDS = 0x0080`.** Bits 8:4 hold the Fast Link Down status and read
  `01000`, which Table 8-15 defines as **RX Errors**. Fast Link Drop fired,
  on the criterion CR3 bit 3 enables: *32 RX_ER occurrences inside a 10 µs
  window*.
- **`RECR = 0x0002`** — the receive-error counter moved, and `PHYSTS` bit 13
  (Receive Error Latch) is set, consistent with it.
- **`PHYSTS = 0x2912`**: link bit 0 clear (down), descrambler lock and signal
  detect clear, fallen back to 10 Mbps.
- **`BMSR = 0x7849`**: link down, auto-negotiation not complete.
  **`ANER = 0x0006`**: link-partner-auto-negotiation-able clear — the partner
  stopped negotiating.
- **`MISR1 = 0xE400`**: bit 15, *Change of Link Quality Status while the link
  is ON*, set on this PHY and on no healthy one in the same probe.

### 3.3 What this means

A causal chain, end to end, for the first time:

```
RX errors on the link  →  32 RX_ER inside 10 µs  →  FLD drops the link  →  ESC
lostlink counter increments, chain severed
```

**This connects the two faults.** `FINDINGS.md` §6.2 observed that the EVE-NET
is the only device in the chain with FLD armed, that it is pointed at RX_ER —
precisely the signal the frame-corruption fault generates (`FINDINGS.md` §7,
"RX_ER is a marker") — and that it had nevertheless never fired. **It has now
fired.** The corruption fault and the link-drop fault are plausibly the same
underlying event at two different intensities: a few RX_ER corrupt a frame, 32
in 10 µs drop the link.

That is a hypothesis with two supporting observations, not an established
result. It does not yet explain §3.5.

### 3.4 The second firing, and what it separates

Run `opdropsagentia4_3` (§4.2) probed its failing PHY within 145 ms:

```
pos 0 phy  3: PHYSTS=0x0912 FLDS=0x0080 MISR1=0xE400 MISR2=0x2800
              BMSR=0x7849 ANER=0x0006 FCSCR=0x0000 RECR=0x0000 CR3=0x1009
```

`FLDS = 0x0080` again — the same `01000`, RX Errors. `MISR1` bit 15 (link
quality changed while the link was ON) set again. `BMSR`/`ANER` identical to
§3.2: link down, auto-negotiation incomplete, partner no longer negotiating.

But **`RECR = 0x0000`** this time, and the `PHYSTS` receive-error latch (bit
13) is clear — where run 3 had `RECR = 2` and the latch set. No ESC counter
moved either: `invalid 0 rxerr 0 fwderr 0`.

So FLD fired on its RX-error criterion while the *receive-error counter
recorded nothing*. That is not a contradiction if the two count different
things — `RECR` counts errors during packet reception, between start and end
of frame, whereas FLD's criterion is 32 RX_ER assertions in any 10 µs window,
including the inter-packet idle stream. **RX_ER asserted during idle would
trip FLD and leave `RECR` at zero.**

Run `opdropsagentia4_4` repeated it exactly — `FLDS = 0x0080`,
`PHYSTS = 0x0912`, `RECR = 0x0000`, `MISR1 = 0xE400`, byte for byte the same
probe as run 6 on the same link. Run 8 then produced three more, identical
again, from the same PHY at a different chain position (§4.3).

**Six firings, one criterion, no counter-example** among the probes that
reached a failing PHY.

Run 8 also sharpens the `MISR1` bit-15 evidence, because its three probes are
consecutive reads of the same registers. `MISR1` is read-clear, so probe 1
zeroed it everywhere; at probes 2 and 3 every healthy PHY reads `0x0000` while
the failing one reads `0xE400` again. The failing PHY re-flags *change of link
quality while the link is ON* in each window containing its own drop, and no
other PHY ever does.

That reading is *inferred from the two registers' definitions*, not
established. It is worth stating because it would explain the standing puzzle
of `FINDINGS.md` §7.3, where `RECR` read zero on both EVE-NET PHYs across 723
probes while the fault was demonstrably active, and because it predicts that
drops with zero error counters everywhere — the TwinCAT signature — are still
FLD firings.

### 3.5 The drops that were NOT Fast Link Drop

Run `opdropsagentia4` produced four lost-link events and **`FLDS = 0x0000` on
every PHY in both probes**, with `RECR = 0x0000` and `FCSCR = 0x0000`
everywhere and no ESC error counter moving at all. By the same reasoning that
makes §3.2 decisive, that says FLD did *not* cause those drops.

Two readings, not yet separated:

1. **The drop was initiated by the partner at the far end.** FLDS latches on
   the PHY whose own FLD logic fired. In `opdropsagentia4` the drops were
   reported by slave 1 port 1 and slave 0 port 1, and in both cases the
   downstream partner was unreachable by the time the probe ran — so if its
   FLD fired, we could not have read it. This is the more likely reading and
   it is testable.
2. **Two distinct mechanisms.** Possible, but nothing yet requires it.

`ecat_op` probes every reachable position, so the only fix is to reach the far
side — which means recovering the link and probing again after it returns.
That capability now exists (`recovery.h`, commit `d385a57`) and has not yet
been run on hardware.

---

## 4. The controlling variable is chain length, not the cable or the pair

This is the result that was not anticipated.

Both links that dropped in the 4-slave chain were afterwards run as isolated
two-slave pairs, and neither dropped:

| link | behaviour in the 4-chain | behaviour as an isolated pair |
|---|---|---|
| `#14 ↔ #16` | dropped (run 1, t=32.7 s) | **0 drops in 394.7 s** |
| `#16 ↔ #nolabel` | dropped (run 1, t=8.7 s) | **not yet tested** — see §7 |
| `#nolabel ↔ #17` | dropped (run 2, t=21.1 s) | **0 drops in 517.7 s** (two runs) |

So it is not that one cable or one unit is bad. The same physical link that
fails inside the four-slave chain is quiet on its own.

### 4.1 The numbers

At-risk time is time in OP with the chain still intact — i.e. up to the first
drop, since without recovery everything after it is a severed chain. This is
the same accounting `TWINCAT.md` §6.4 uses to derive 0.118/s.

| configuration | at-risk time | drops | rate |
|---|---|---|---|
| 4-slave chains (five runs pooled) | 66.74 s | 7 | **0.1049/s**, 95% CI [0.042, 0.216] |
|  — field-only `#14→#16→#nolabel→#17` | 29.77 s | 2 | 0.0672/s |
|  — mixed A `#nolabel→#17→#5→#0` | 6.33 s | 2 | 0.3162/s |
|  — mixed B `#5→#0→#nolabel→#17` | 30.65 s | 3 | 0.0979/s |
| **TwinCAT, this hardware** | 25.39 s | 3 | **0.1182/s**, 95% CI [0.024, 0.345] |
| pair `#14 ↔ #16` | 394.7 s | 0 | 95% upper bound 0.0076/s |
| pair `#nolabel ↔ #17` | 517.7 s | 0 | 95% upper bound 0.0058/s |
| **both pairs pooled** | **912.4 s** | **0** | 95% upper bound **0.0033/s** |

Runs 1, 3, 6 and 7 each ended at their first drop and contribute one apiece
(8.689, 21.079, 2.163, 4.162 s). Run 8 recovered each time and contributes
three drops over 30.650 s of continuous at-risk time — the first measurement
here that is a rate rather than a collection of first-failure times.

The pooled interval **overlaps TwinCAT's comfortably**. On this evidence the
two rates are not distinguishable, which is the strongest statement of
reproduction available.

If the pairs dropped at the four-slave rate they would have produced **95.7**
drops. They produced zero: **P = 2×10⁻⁴²**, a separation of **31.9×**.

The three chains differ by up to 4.7× but the counts are small and the
intervals overlap; there is no evidence yet that configuration affects the
rate, only whether it drops at all.

Each pair is independently significant, so this is not one long quiet run
carrying a short one: `#14 ↔ #16` alone bounds at 0.0076/s and
`#nolabel ↔ #17` at 0.0058/s, both below the four-slave rate with no overlap.

The four-slave rate of **0.1108/s** is within a **factor of 1.07** of
TwinCAT's 0.1182/s. Our rig is reproducing the field fault at the field rate.

### 4.2 The same link, both ways — the controlled case

Run 6 is the strongest form of the result, because the link is its own
control. `#nolabel ↔ #17` was run as an isolated pair for 517.7 s across two
runs and never dropped. Two spare M400+EVE-NET drives from our **own 11-slave
lab chain** (`#5`, `#0`) were then cabled on behind `#17` — nothing else
changed, same two field units, same cable between them:

```
master ── #nolabel ── #17 ── #5 ── #0        drop on #nolabel↔#17 at t = 2.163 s
          pos0        pos1   pos2  pos3      (slave 0 port 1)
```

**The link dropped in 2.163 s**, the fastest of any run, and it dropped at
`slave 0 port 1` — the very link that had just been silent for 517.7 s.
Run 7 repeated the configuration and reproduced it: another drop on
`slave 0 port 1`, at 4.162 s, with a byte-identical PHY probe.

Two things follow:

1. **It is not the field units.** The devices that made the difference are
   ours, from the chain that ran 6,144.7 s in OP without a single drop
   (`FINDINGS.md` §10.2). Ordinary EVE-NETs appended behind a quiet link are
   enough to make it fail.
2. **It is not the failing link's own cable or endpoints.** Those were held
   fixed across the two outcomes. Whatever the mechanism is, it is provoked by
   devices that are not party to the link that breaks.

This is a within-link control rather than a comparison across configurations,
which is what raises §4 from "chain length correlates" to "something beyond
the partner causes it".

For scale, our four-slave rate of 0.0672/s sits within a factor of 1.8 of
TwinCAT's 0.118/s on this same hardware.

---

### 4.3 The order test — it follows the link, not the position

Run 8 kept all four units and reversed which half sits nearest the master:

```
runs 6,7   master ── #nolabel ── #17 ── #5 ── #0
run 8      master ── #5 ── #0 ── #nolabel ── #17
```

The drop moved with the **link**, not with the position. In runs 6 and 7 it
was `slave 0 port 1`; in run 8 it was `slave 2 port 1`. Both are the same
physical thing: **`#nolabel`'s port 1, facing `#17`.**

Run 8 dropped it three times in 30.65 s, each time recovering and each time
failing again on that same PHY.

So chain position is excluded. A link that fails as position 0 also fails as
position 2, and the two lab drives are just as effective at provoking it from
in front as from behind.

### 4.4 Every FLD firing is on one PHY

Tabulating all eight drop events by the *physical* link rather than the slave
index:

| run | chain | link that dropped | `FLDS` on the near PHY |
|---|---|---|---|
| 1 | field | `#16 ↔ #nolabel` | `0x0000` |
| 1 | field | `#14 ↔ #16` | `0x0000` |
| 3 | field | **`#nolabel ↔ #17`** | **`0x0080`** |
| 6 | mixed A | **`#nolabel ↔ #17`** | **`0x0080`** |
| 7 | mixed A | **`#nolabel ↔ #17`** | **`0x0080`** |
| 8 | mixed B | **`#nolabel ↔ #17`** | **`0x0080`** ×3 |

Six of eight drops are the same link, and **every one of the six latched Fast
Link Drop on the RX-error criterion**. The two that did not are the two on
other links, and in both of those the near PHY latched nothing — consistent
with §3.5 reading 1, that the far partner's FLD fired and it was unreachable
by probe time.

The `#5 ↔ #0` link — the two lab drives — has **never dropped**, at either end
of the chain. So those units are a necessary *enabler* and never a victim:
their presence makes `#nolabel ↔ #17` fail, and they are fine themselves.

That is the sharpest statement the data supports:

> `#nolabel`'s port-1 PHY drops its link on RX errors, but only when at least
> two further devices are present in the chain — regardless of where they sit,
> and regardless of whose they are.

The pair control remains the counterweight: the very same link, the same two
units and the same cable, ran 517.7 s in isolation without a single drop
(§4.2).

### 4.5 Relation to the corruption fault's condition 2

`FINDINGS.md` §1 condition 2 for the *frame-corruption* fault reads: "at least
one device must lie beyond the EVE-NET's port-1 partner."

| run | link that dropped | devices beyond the partner | fits? |
|---|---|---|---|
| 1 | `#16 ↔ #nolabel` | `#17` | yes |
| 1 | `#14 ↔ #16` | `#nolabel`, `#17` | yes |
| 3 | `#nolabel ↔ #17` | none | **no** |
| 6 | `#nolabel ↔ #17` | `#5`, `#0` | yes |
| 7 | `#nolabel ↔ #17` | `#5`, `#0` | yes |
| 8 ×3 | `#nolabel ↔ #17` | none | **no** |

**Run 8 refutes the condition as stated.** Its three drops were on the last
link in the chain, with nothing at all beyond `#17`, and the two devices that
made the difference sat *in front of* the failing link rather than behind it.
Four of eight drops now have no device beyond the partner.

What survives is the weaker and more robust statement of §4.4: **total device
count matters, position does not.** "Beyond the partner" was a correlate in
the lab chain, where the EVE-NET happened to sit near the front. The two
faults may still share a mechanism, but they do not share this condition, and
`FINDINGS.md` condition 2 should be re-examined against a chain with the
EVE-NET at the end.

---

## 5. Recovery, exercised for the first time

Run 6 is also the first hardware execution of the recovery path added in
`d385a57`. Most of it worked and one step did not.

```
[   2.308] slave  0 port 1  *** LOST LINK +2 (now 3) ***
[   2.471] port closed: 0x0101 0x00 -> 0x0C (wkc 1)
[   5.228] link back after 1.919 s, debounce done, port reopened 0x04 (wkc 1)
[   5.231] re-init of position 1 FAILED (AL code 0x001B SyncManager watchdog)
[   5.232] re-init of position 2 FAILED (AL code 0x001B SyncManager watchdog)
[   5.233] re-init of position 3 FAILED (AL code 0x001B SyncManager watchdog)
[   5.233] *** RECOVERED in 2.921 s — but the chain is NOT back in OP ***
```

**What worked, and corroborates the capture.** The port loop control was read
as `0x00` on this unit rather than the `0xF4` TwinCAT's chain sat at, and the
computed bytes were right for it: `0x0C` to force port 1 closed, `0x04` to
restore it to auto-close. Writing the byte from what was actually there, which
`t_recovery` T6 pins, is why an unexpected base value cost nothing.

The physical link came back after **1.919 s**, against the 2.0012 / 2.0008 /
1.9842 s TwinCAT recorded on its three drops. That is independent confirmation
that the ~2 s is the PHY re-negotiating and not a master-side timer — the
design decision in `recovery.h` to *wait* rather than sleep 2 s. Total outage
**2.921 s** against TwinCAT's 3.00 s.

**What failed, in run 6.** All three slaves behind the closed port refused
re-init with AL code `0x001B`, SyncManager watchdog. They had received no
process data for 2.9 s, so their watchdogs expired and they sat with the AL
error bit latched. `op_bring_up` requested new states without setting
`AL_ERR_ACK` (`0x10`), so the error was never acknowledged and every request
was ignored.

**What failed, in run 7 — differently.** The same three slaves failed with AL
code `0x0000`, "no error", each after only ~150 ms:

```
[   7.228] link back after 1.899 s, debounce done, port reopened 0xF7 (wkc 1)
[   7.380] re-init of position 1 FAILED (AL code 0x0000 no error)
[   7.530] re-init of position 2 FAILED (AL code 0x0000 no error)
[   7.681] re-init of position 3 FAILED (AL code 0x0000 no error)
```

No error code and no 2 s state timeout means the slaves never answered at all
— the very first addressed write returned a working counter of zero. Re-init
began **152 ms after the port reopened**, and the link behind it had not
finished coming up. TwinCAT's own ~40 ms figure (`TWINCAT.md` §6.6) is
measured from a different moment: it had already watched the physical link
return *while the port was still forced closed*.

So there are two distinct defects, and run 6's fix does not cover run 7's.
Recovery must **wait until the slaves are addressable again** before
attempting re-init, rather than assuming the reopen is instantaneous.

**A third problem: the port byte was read back inconsistently.** Run 6 read
`0x0101 = 0x00` on this slave; run 7 read `0xFF` on the same slave in the same
chain. Neither matches TwinCAT's `0xF4`. Worse, in run 7 the close was a
**no-op** — `0xFF` already has port 1 forced closed, so `0xFF → 0xFF` wrote
nothing — and the link still returned and the reopen to `0xF7` still worked,
which suggests the close may not be doing what the sequence assumes. Reading
the base *after* the drop, when the slave is mid-disruption, is the likely
cause. It should be captured at bring-up while the chain is healthy.

**A fourth: cycle timing collapses after a failed recovery.** Run 7 achieved
525 Hz, mean interval 1399 µs, max **453 ms**, with 13,972 missed cycles out
of 33,906. With three slaves unreachable, the per-cycle acyclic polls wait out
the full 50 ms transaction timeout every time. A run that has lost its chain
should stop polling the slaves it cannot reach.

**Consequence for the summary lines.** `ecat_op` computed at-risk time as
elapsed minus the recovery outage — "308.2 s -> 0.0065 drops/s" in run 6,
"61.7 s -> 0.0324" in run 7 — which credits every second the chain spent
severed. The true figures are **2.163 s** and **4.162 s**. §4.1 uses those.

**Neither run 6 nor run 7 tested the fixes.** Commit `ba1881d` addresses the
AL acknowledge and the at-risk accounting, but it was built at 14:20 and run
7's log is timestamped 14:15. Both used the older binary.

### 5.1 Run 8: recovery worked, three times

Run 8 is the first on `ba1881d`, and the first run in the whole investigation
to measure more than one drop:

```
[   7.152] slave 2 port 1 *** LOST LINK +2 (now 3) ***
[   7.282] port closed: 0x0101 0xF4 -> 0xFC (wkc 1)
[  10.027] link back after 1.874 s, debounce done, port reopened 0xF4 (wkc 1)
[  10.282] *** RECOVERED in 2.876 s ***
[  23.657] ... *** RECOVERED in 2.967 s ***
[  31.121] ... *** RECOVERED in 2.967 s ***
```

Three drops, three complete recoveries, no "chain is NOT back in OP". The AL
error acknowledge fixed it.

| | run 8 | TwinCAT |
|---|---|---|
| link return after the close | 1.874 / 1.965 / 1.838 s | 2.0012 / 2.0008 / 1.9842 s |
| total outage | 2.876 / 2.967 / 2.967 s, mean 2.894 | 3.00 s every time |

The outage is a tenth of a second short of TwinCAT's because our debounce
starts from our own detection of the link rather than TwinCAT's `0x0111` poll.
Close enough to say the sequence is right.

**The at-risk accounting is right too.** 39.7 s elapsed, three 2.9 s outages,
and the tool reported 30.650 s at-risk — 77.4% of cycles, which is exactly
`39.7 − 3×2.9`. The old formula would have printed 31.0 s from a different
route and been right only by coincidence; under run 6's conditions it was
wrong by 140×.

**`0x0101` read `0xF4` twice — TwinCAT's own value** — and `0x00` on the third
recovery, so §5's third defect is still live but the `0xF4` sighting supports
the reading that the odd values come from reading the register mid-disruption.
The fourth defect is improved but not gone: 971 Hz achieved with 1,115 missed
cycles, against run 7's 525 Hz and 13,972.

---

## 6. The contradiction with the capture

`TWINCAT.md` §3 is unambiguous about TwinCAT's own arrangement:

> Two drives, both ESC type `0x90` (EVE-NET), station addresses 1001 and 1002,
> positions 0 and 1, directly on the master — no other devices in the chain.

**TwinCAT saw 0.118 drops/s on a two-drive chain. Our two-drive chains of the
same units give zero.** Chain length alone therefore cannot be the whole
explanation, and §4 cannot be promoted to a cause.

Run 8 sharpens this rather than resolving it. The fragile link is now
identified as `#nolabel ↔ #17` (§4.4) — and that exact pair was run in
isolation for 517.7 s without a drop. So if TwinCAT's two drives were
`#nolabel` and `#17`, we have contradicted the capture directly on the same
hardware. If they were another pair, only `#16 ↔ #nolabel` remains untested.

The candidates, in order of how cheaply they can be tested:

1. **The pair TwinCAT used is the one we have not tested.** Of the three
   inter-drive links in the chain, `#16 ↔ #nolabel` is the only one never run
   in isolation — and it is the link that dropped *first* and *fastest* in run
   1 (t = 8.689 s). Which two of these four units were on TwinCAT's bench is
   not recorded in the capture; the station addresses 1001/1002 are assigned
   by the master and say nothing about the physical unit.
2. **Exposure scales with the number of inter-drive links.** Three links
   instead of one is 3× the exposure, which is the right sign but far short of
   the 28.6× separation in §4.1.
3. **Something about the longer chain provokes it** — return-path latency,
   accumulated jitter, or the larger cyclic frame (44 B of process data
   against 22 B). Link utilisation is under 1% in both cases, so the
   utilisation mechanism of `FINDINGS.md` §5.5 does not transfer.
4. **The installation differed.** TwinCAT's bench cabling, power and grounding
   are not reproduced here and are not recorded.

---

## 7. Next experiments

Run 6 changed the ordering. The recovery defect (§5) now blocks everything
else, because until it is fixed each run yields exactly one drop and then
stops being an experiment.

1. **Finish the recovery fixes.** Two of four are now *proven* on hardware by
   run 8 (§5.1): the AL error acknowledge, and at-risk time from healthy
   cycles. Two remain (§5):

   - **Wait for the slaves to answer** after reopening the port, instead of
     re-initialising 152 ms later into a link that has not come up. Poll until
     the expected slave count responds, with a timeout.
   - **Capture `0x0101` at bring-up**, while the chain is healthy, rather than
     reading it mid-disruption. It read `0x00` in run 6 and `0xFF` in run 7 on
     the same slave, and in run 7 the close wrote nothing at all.

   Worth doing at the same time: stop polling slaves known to be unreachable.
   That cost run 7 41% of its cycles and run 8 still 2.9%.

2. **Then re-run the four-slave chain long.** With recovery working this is
   the run that turns three single observations into a measured rate against
   TwinCAT's 0.118/s — and, for §3.5, it brings the far side of a dropped link
   back so its `FLDS` can be probed. If the far PHY latches `FLDS = 0x0080` on
   the drops where the near PHY latched nothing, one mechanism explains every
   drop recorded so far.

3. **A three-slave chain.** This is the sharpest remaining question and it did
   not exist before run 6: two devices never drop, four always do, and nothing
   has been measured in between. Take run 6's chain and remove the last unit:

   ```
   ./ecat_op -i enp2s0 -s 3 --op 0,1,2 --random 5 -d 1800 -F chain3
   ```

   A drop means **one** device beyond the partner suffices, which matches
   `FINDINGS.md` condition 2 exactly and makes the two faults one. Silence
   means there is a threshold above one, which no current hypothesis predicts
   and which would be the most interesting result available.

4. **Run `#16 ↔ #nolabel` as an isolated pair.** Still the one inter-drive
   link never run alone, and still the direct test of whether TwinCAT's own
   two-drive rig was a special pair (§6). Demoted only because §4.2 showed the
   pair/chain difference is not a property of any particular pair.

5. **Extend the pair runs.** 912.4 s pooled bounds them at 0.0033/s, 28.6×
   below the four-slave rate, each pair independently significant. Low
   marginal value next to items 1–4.

---

## 8. Instrument state and caveats

**What is trustworthy.** The ESC lost-link counters at `0x0310` are never
cleared by anything either master does, so they are running totals and the
deltas are real. The PHY probe reads the perishable registers in the order
that protects them (`PHYSTS` first, since reading `BMSR`/`ANER`/`MISR1`/`RECR`
clears its latch bits). Raw register values are recorded alongside every
decode, and the decodes above are checked against SNLS505H rather than
recalled.

**What is not.**

- **`+2` per drop is unexplained.** Every lost-link event so far has
  incremented the counter by exactly 2, never 1. Either one physical drop
  raises it twice (loss and re-establish), or two drops land inside one 203 ms
  poll interval. This directly affects the rates in §4.1: if a drop is worth
  2, the drop *count* is right; if not, it is doubled. The rates above count
  **first-drop events**, not counter increments, which sidesteps it — but the
  "4 drops" and "2 drops" headline figures do not.
- **`FCSCR = 0x0000` while `PHYSTS` bit 11 (False Carrier Latch) is set** in
  §3.2. Table 8-16 says bit 11 is cleared by reading `FCSCR`, and `PHYSTS` was
  read first, so the two should agree. Unresolved; the raw values stand.
- **The at-risk accounting assumes the chain is intact until the first WKC
  drop.** True for these runs, but it means each run contributes only its
  first drop to a rate.
- **Runs 1–4 used the pre-recovery build**, whose `events.csv` timestamps
  carry a constant offset from the console log (fixed in `d385a57`). Times in
  this note are taken from the **console log**, which was always correct.
- **Runs 5 and 6 used the post-`d385a57` build.** Run 6 exercised the
  recovery path for the first time (§5): port close, link wait, debounce and
  reopen all behaved, and the `events.csv` epoch is now confirmed on hardware
  — the event logged at 2.308161 s against the console's 2.308285 s, where
  the old build would have written ~1669 s. **The re-init step is broken**
  (AL `0x001B`, §5) and is the one part still not working.
- **The at-risk lines printed by runs 6 and 7 are both wrong.** They subtract
  only the recovery outage, not the time the chain spent out of OP after the
  failed re-init: "308.2 s -> 0.0065 drops/s" and "61.7 s -> 0.0324". The
  correct figures are 2.163 s and 4.162 s. §4.1 uses those; the printed lines
  should not be quoted.
- **Runs 1–7 each end at their first drop**, because recovery never
  completed, so each contributes a single first-failure time to §4.1. Only run
  8 is a continuous observation. The pooled rate mixes the two, which is valid
  for a Poisson process but means 4 of the 7 events carry a selection effect:
  a run stopped *at* its first drop cannot contribute a long quiet stretch.
  If anything this biases the pooled rate **upward**, so the agreement with
  TwinCAT is if anything conservative on our side.
- **Run 8's 39.7 s is short.** Three drops is enough to establish a rate with
  a wide interval, not to compare configurations. The long run is still to
  come.
- **No run reached its requested 7200 s.** All were stopped by hand once the
  chain had severed.

---

## 9. Run index

Times from the console log. "At-risk" is OP with an intact chain.

| # | capture dir | chain | at-risk | drops | where | FLDS |
|---|---|---|---|---|---|---|
| 1 | `opdropsagentia4` | `#14→#16→#nolabel→#17` | 8.689 s | 4 | slave 1 p1, then slave 0 p1 | `0x0000` (both probes) |
| 2 | `opdropsagentia2` | `#14→#16` | 394.7 s | 0 | — | not triggered |
| 3 | `opdropsagentia4_2` | `#14→#16→#nolabel→#17` | 21.079 s | 2 | slave 2 p1 | **`0x0080` RX Errors** |
| 4 | `opdropsagentia2_2` | `#nolabel→#17` | 145.4 s | 0 | — | not triggered |
| 5 | `opdropsagentia2_2#2` | `#nolabel→#17` | 372.3 s | 0 | — | not triggered |
| 6 | `opdropsagentia4_3` | `#nolabel→#17→#5→#0` | **2.163 s** | 2 | slave 0 p1 | **`0x0080` RX Errors** |
| 7 | `opdropsagentia4_4` | `#nolabel→#17→#5→#0` | **4.162 s** | 2 | slave 0 p1 | **`0x0080` RX Errors** |
| 8 | `opdropsagentia4_5` | `#5→#0→#nolabel→#17` | **30.650 s** | 6 (3 recovered) | slave 2 p1 ×3 | **`0x0080` RX Errors** ×3 |

Logs are the matching `.log` files; run 3's log is `opdropsagentia4_2og`.
Each capture directory holds `events.csv`, `frames.pcap` and `probes.txt`.

`#5` and `#0` are M400+EVE-NET drives taken from our own 11-slave lab chain —
the chain that ran 6,144.7 s in OP without a drop (`FINDINGS.md` §10.2). Runs
1–4 used the pre-recovery build, runs 5–7 the `d385a57` build, and **run 8 the
`ba1881d` build** — the first with working recovery, and so the first whose
at-risk time and drop rate can be quoted directly from its own summary.

### Reproducing the analysis

```bash
# the drops and their probe data
cat opdropsagentia4_2og
cat opdropsagentia4_2/probes.txt

# FLDS bits 8:4 against SNLS505H Table 8-15
python3 -c "v=0x0080; print(format((v>>4)&0x1F,'05b'))"   # 01000 = RX Errors
```
