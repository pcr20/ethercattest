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
/* Is this frame's Ethernet header intact, i.e. does it carry the EtherCAT
 * EtherType where it should? A frame that lost its leading bytes on the wire
 * has payload at offset 12-13 and will fail this test.
 *
 * Callers MUST gate parse_return_frame() on this. That parser reads byte 16
 * to decide whether a frame is another master's (the foreign-frame test); on
 * a prefix-chopped frame byte 16 is arbitrary payload, so without the gate a
 * damaged frame of ours is misfiled as foreign and subtracted from TxOk. */
static inline int frame_hdr_is_ecat(const uint8_t *buf, int len) {
    return len >= (int)ETH_HDR_LEN && buf[12] == 0x88 && buf[13] == 0xA4;
}

uint64_t parse_return_frame(const uint8_t *buf, int len, int num_slaves,
                            int loopback, int fcs_ok, int *payload_ok);

#endif /* ECAT_FRAME_H */
