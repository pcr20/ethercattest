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

**Four slaves in the chain → drops within 9 to 21 s. Either half of that chain
run as a pair → nothing in 540 s.**

| configuration | units | time in OP | first drop | total drops |
|---|---|---|---|---|
| 4-slave chain | `#14 → #16 → #nolabel → #17` | 78.4 s | **8.689 s** | 4 |
| 4-slave chain (repeat) | `#14 → #16 → #nolabel → #17` | 47.3 s | **21.079 s** | 2 |
| 2-slave pair | `#14 → #16` | 394.7 s | — | **0** |
| 2-slave pair | `#nolabel → #17` | 145.4 s | — | **0** |

On one drop the PHY latched **which mechanism fired**, and it is Fast Link
Drop on the RX-error criterion (§3). That is the first direct evidence of a
cause anywhere in this investigation, and it connects the two faults that
`FINDINGS.md` has so far treated as independent.

The result does **not** reduce to a bad cable or a bad unit. Both links that
dropped in the 4-slave chain were subsequently run as isolated pairs and
neither dropped (§4).

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

## 3. The mechanism, caught once

Run `opdropsagentia4_2` is the important one. It is the only drop so far where
the PHY that dropped the link was still reachable when probed, and it latched
the reason.

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

That is a hypothesis with one supporting observation, not an established
result. It does not yet explain §3.4.

### 3.4 The drops that were NOT Fast Link Drop

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
| `#16 ↔ #nolabel` | dropped (run 1, t=8.7 s) | **not yet tested** — see §6 |
| `#nolabel ↔ #17` | dropped (run 2, t=21.1 s) | **0 drops in 145.4 s** |

So it is not that one cable or one unit is bad. The same physical link that
fails inside the four-slave chain is quiet on its own.

### 4.1 The numbers

At-risk time is time in OP with the chain still intact — i.e. up to the first
drop, since without recovery everything after it is a severed chain. This is
the same accounting `TWINCAT.md` §6.4 uses to derive 0.118/s.

| configuration | at-risk time | drops | rate |
|---|---|---|---|
| 4-slave chain (both runs pooled) | 29.77 s | 2 | **0.0672/s**, one per 14.9 s |
| 2-slave pairs (both pooled) | 540.1 s | 0 | 95% upper bound **0.0055/s** |

If the pairs dropped at the four-slave rate they would have produced **36.3**
drops. They produced zero: **P = 1.7×10⁻¹⁶**, a separation of **12.1×** with
disjoint intervals.

For scale, our four-slave rate of 0.0672/s sits within a factor of 1.8 of
TwinCAT's 0.118/s on this same hardware.

---

## 5. The contradiction with the capture

`TWINCAT.md` §3 is unambiguous about TwinCAT's own arrangement:

> Two drives, both ESC type `0x90` (EVE-NET), station addresses 1001 and 1002,
> positions 0 and 1, directly on the master — no other devices in the chain.

**TwinCAT saw 0.118 drops/s on a two-drive chain. Our two-drive chains of the
same units give zero.** Chain length alone therefore cannot be the whole
explanation, and §4 cannot be promoted to a cause.

The candidates, in order of how cheaply they can be tested:

1. **The pair TwinCAT used is the one we have not tested.** Of the three
   inter-drive links in the chain, `#16 ↔ #nolabel` is the only one never run
   in isolation — and it is the link that dropped *first* and *fastest* in run
   1 (t = 8.689 s). Which two of these four units were on TwinCAT's bench is
   not recorded in the capture; the station addresses 1001/1002 are assigned
   by the master and say nothing about the physical unit.
2. **Exposure scales with the number of inter-drive links.** Three links
   instead of one is 3× the exposure, which is the right sign but far short of
   the 12× separation in §4.1.
3. **Something about the longer chain provokes it** — return-path latency,
   accumulated jitter, or the larger cyclic frame (44 B of process data
   against 22 B). Link utilisation is under 1% in both cases, so the
   utilisation mechanism of `FINDINGS.md` §5.5 does not transfer.
4. **The installation differed.** TwinCAT's bench cabling, power and grounding
   are not reproduced here and are not recorded.

---

## 6. Next experiments

In priority order. The first is decisive and takes minutes.

1. **Run `#16 ↔ #nolabel` as an isolated pair.** It is the one untested link,
   it dropped fastest in the chain, and it is the leading candidate for
   TwinCAT's own pair. A drop here collapses §4 and §5 together: the fault
   would be a property of that pair, and chain length would be irrelevant.
   Silence here makes chain length real and sharpens §5 to items 2–4.

   ```
   ./ecat_op -i enp2s0 -s 2 --op 0,1 --random 5 -d 1800 -F pair_16_nolabel
   ```

2. **Re-run the 4-slave chain with recovery enabled** (commit `d385a57`,
   untested on hardware). This is what turns single observations into a rate,
   and — the reason it matters for §3.4 — it brings the far side of a dropped
   link back so its `FLDS` can be probed.

3. **Probe the partner after recovery.** If the far PHY latches
   `FLDS = 0x0080` on the drops where the near PHY latched nothing, §3.4
   reading 1 is confirmed and a single mechanism explains everything.

4. **Extend the pair runs.** 394.7 s and 145.4 s bound the pair rate at
   0.0055/s, which is 21× below TwinCAT's 0.118/s but only 12× below the
   4-slave rate. An hour on each pair would tighten that by an order of
   magnitude.

---

## 7. Instrument state and caveats

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
- **No run reached its requested 7200 s.** All were stopped by hand once the
  chain had severed.

---

## 8. Run index

Times from the console log. "At-risk" is OP with an intact chain.

| # | capture dir | chain | at-risk | drops | where | FLDS |
|---|---|---|---|---|---|---|
| 1 | `opdropsagentia4` | `#14→#16→#nolabel→#17` | 8.689 s | 4 | slave 1 p1, then slave 0 p1 | `0x0000` (both probes) |
| 2 | `opdropsagentia2` | `#14→#16` | 394.7 s | 0 | — | not triggered |
| 3 | `opdropsagentia4_2` | `#14→#16→#nolabel→#17` | 21.079 s | 2 | slave 2 p1 | **`0x0080` RX Errors** |
| 4 | `opdropsagentia2_2` | `#nolabel→#17` | 145.4 s | 0 | — | not triggered |
| 5 | `opdropsagentia2_2#2` | 2-slave | *in progress* | | | |

Logs are the matching `.log` files; run 3's log is `opdropsagentia4_2og`.
Each capture directory holds `events.csv`, `frames.pcap` and `probes.txt`.

### Reproducing the analysis

```bash
# the drops and their probe data
cat opdropsagentia4_2og
cat opdropsagentia4_2/probes.txt

# FLDS bits 8:4 against SNLS505H Table 8-15
python3 -c "v=0x0080; print(format((v>>4)&0x1F,'05b'))"   # 01000 = RX Errors
```
