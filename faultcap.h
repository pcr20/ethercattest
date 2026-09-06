#ifndef ECAT_FAULTCAP_H
#define ECAT_FAULTCAP_H
/* ── Fault capture: pcap frame logging + error-triggered PHY probe ──────────
 *
 * Single header, single object. ecat_ber links faultcap.o (plus escreg.o and
 * escmii.o for the ESC/MII transport) and includes only this file.
 *
 * TWO DISTINCT POPULATIONS, deliberately kept separate:
 *
 *   frames  — frames that fail OUR checks on arrival (bad FCS, bad/absent
 *             payload CRC, wrong length). These are genuinely damaged frames
 *             and their bytes are written to a pcap. Only ~6% of damage events
 *             produce one, because the NIC drops the rest before user space.
 *
 *   events  — an ESC per-port error counter moved. Catches ~100% of damage
 *             events including those whose frame never reached us, but says
 *             only "slave S port P counted an error", not which frame.
 *
 * EVENTS drive the probe; FRAMES are the forensic detail. Both are timestamped
 * on the same clock so they can be correlated afterwards.
 *
 * BURST HANDLING: damage arrives in bursts. Probing requires pausing TX, so
 * triggering on the first error of a burst would suppress the remainder and
 * bias the sample. Instead the burst is allowed to drain — every failing frame
 * in it is logged — and the probe fires only after a quiet period.
 *
 * THREADING: faultcap_frame() and faultcap_esc_event() are called from the RX
 * thread and never block or allocate; they push into lock-free SPSC rings that
 * drop-and-count when full (§4.4 — console or file I/O in the RX path stalls
 * the drain and invalidates the measurement). The supervisor thread drains
 * them and does all file and network I/O. */
#include "ecat_common.h"

#define FAULTCAP_REASON_FCS      (1u << 0)
#define FAULTCAP_REASON_PAYLOAD  (1u << 1)
#define FAULTCAP_REASON_LENGTH   (1u << 2)

/* Open the capture set under dir/: frames.pcap, events.csv, probes.txt.
 * num_slaves positions are probed on each trigger. Returns 0 on success. */
int  faultcap_open(const char *iface, int num_slaves, const char *dir);
void faultcap_close(void);

/* RX thread. Non-blocking. buf/len are the frame as received (including the
 * FCS trailer when rx-fcs is on). */
void faultcap_frame(uint64_t t_ns, uint32_t reason, const uint8_t *buf, int len);

/* RX thread. Non-blocking. Records that an ESC counter moved. */
void faultcap_esc_event(uint64_t t_ns, int slave, int port,
                        const char *counter, unsigned delta);

/* Supervisor. Total error events seen (the N in "run until N errors"). */
uint64_t faultcap_event_count(void);

/* Supervisor. Non-zero once at least one event has been seen AND no further
 * event has arrived for quiet_ns — i.e. the burst has drained. */
int  faultcap_burst_settled(uint64_t now_ns, uint64_t quiet_ns);

/* Supervisor, blocking. Drains both rings to disk, then probes every position
 * and appends the perishable PHY registers to probes.txt. The caller MUST
 * have paused TX and muted RX accounting first — this puts frames on the same
 * wire. Returns the number of positions successfully probed. */
int  faultcap_probe(const char *iface, uint64_t trigger_ns);

/* Supervisor. Drain rings without probing (used at shutdown). */
void faultcap_flush(void);

#endif /* ECAT_FAULTCAP_H */
