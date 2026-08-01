#ifndef ECAT_FRAME_H
#define ECAT_FRAME_H
/* EtherCAT frame builder and return-frame parser (the protocol heart).
 * All EtherCAT fields are little-endian on the wire (ETG.1000.4) — see the
 * le16put/le16get helpers in ecat_common.h and the endianness history there. */
#include "ecat_common.h"

/* Build one test frame into buf (loopback: single NOP; slaves: NOP + BRD +
 * per-slave CRC-APRD + per-slave lost-link-APRD). Returns the frame length.
 * Aborts if the overhead budget ever disagrees with what was written. */
int build_frame(uint8_t *buf, int buflen, const uint8_t *src_mac,
                int num_slaves, uint64_t seq, int loopback);

/* Parse a returned frame; update stats. Returns the payload sequence number
 * or UINT64_MAX if unparseable. fcs_ok gates everything read from untrusted
 * fields (WKC mismatch count, ESC CRC/lost-link accumulation). *payload_ok is
 * set iff the payload CRC32C verified; every received frame WITHOUT a valid
 * payload CRC increments payload_crc_errors (invalid or missing — a cut
 * frame cannot have a valid payload CRC, so it counts). */
uint64_t parse_return_frame(const uint8_t *buf, int len, int num_slaves,
                            int loopback, int fcs_ok, int *payload_ok);

#endif /* ECAT_FRAME_H */
