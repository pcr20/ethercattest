# EtherCAT BER Test Tool

Tests EtherCAT physical layer bit error rate by saturating a slave chain
with NOP frames and logging CRC errors from both the host NIC PHY and
ESC slave registers.

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

## CSV Output

Each row (written every 5 seconds):

```
elapsed_s, frames_sent, frames_rcvd, frames_lost, nic_crc_errors,
brd_wkc_mismatches, slave0_p0_crc, slave0_p1_crc, slave0_p2_crc,
slave0_p3_crc, slave1_p0_crc, ...
```

Import into Excel or Python/pandas for post-analysis:

```python
import pandas as pd
df = pd.read_csv('results.csv')
df['ber_estimate'] = df['nic_crc_errors'] / (df['frames_sent'] * 12000)
print(df.tail())
```
