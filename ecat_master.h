#ifndef ECAT_MASTER_H
#define ECAT_MASTER_H
/* ── Minimal EtherCAT master: bring selected slaves to OP ───────────────────
 *
 * *** THIS MODULE WRITES TO THE SLAVE. ***
 *
 * It exists for one experiment: the TwinCAT capture of 2026-09-23 shows two
 * M400+EVE-NET drives dropping the link between them roughly every 12 seconds
 * while in OP, with every error counter at zero. Our rig has never reproduced
 * a link drop — and has never put a drive into OP. This module closes that
 * gap so the same conditions can be created while our own instrumentation is
 * watching (per-frame 0x0300-0x0327 polling, and a PHY probe on a lost-link
 * increment that captures FLDS, which TwinCAT cannot read).
 *
 * The write sequence is a replay of what TwinCAT actually did, taken from the
 * capture datagram by datagram. tests/twincat_seq.h holds those bytes and
 * t_opseq asserts our builders reproduce them exactly.
 *
 * NOTHING WRITTEN HERE IS PERSISTENT. There are no EEPROM writes: SII is not
 * touched at all (see DEVIATIONS). Station address, SyncManager, FMMU and AL
 * state all live in volatile ESC registers and are gone at power-off.
 *
 * DEVIATIONS from the captured sequence, all deliberate:
 *
 *   1. No SII/EEPROM identity read. TwinCAT takes EEPROM control (0x0500,
 *      0x0502) and reads vendor ID and product code from SII words 8 and 10.
 *      We read the ESC type at 0x0000 instead, which is enough to refuse to
 *      configure anything that is not a type-0x90 device, and avoids
 *      contending with the drive firmware for the EEPROM interface.
 *
 *   2. No periodic BWR 0x0300. TwinCAT broadcasts a counter clear about
 *      twelve times a second. Doing that would erase the evidence this whole
 *      exercise exists to collect. The one-off clear during bus reset is kept
 *      (we baseline after it).
 *
 * Both deviations are printed at startup so a run's log states what it did.
 *
 * Addressing modes used (ETG.1000.4, all fields little-endian):
 *   APRD/APWR  auto-increment: ADP = -position, slave sees 0 as it passes
 *   FPRD/FPWR  fixed: ADP = station address assigned via APWR 0x0010
 *   BRD/BWR    broadcast: ADP = 0, every slave participates
 *   LRW        logical: the 32-bit logical address goes in the ADP/ADO field */
#include "escreg.h"

#define OP_MAX_SLAVES  32

/* Datagram commands. APRD/BRD/NOP come from ecat_common.h; the write and
 * logical commands are defined here so that a translation unit which does not
 * include this header cannot emit one. */
#define ECAT_CMD_APWR_M  0x02
#define ECAT_CMD_FPRD_M  0x04
#define ECAT_CMD_FPWR_M  0x05
#define ECAT_CMD_BWR_M   0x08
#define ECAT_CMD_LRW_M   0x0C

/* AL states (register 0x0120 control / 0x0130 status, low nibble). */
#define AL_INIT    0x01
#define AL_PREOP   0x02
#define AL_SAFEOP  0x04
#define AL_OP      0x08
#define AL_ERR_ACK 0x10   /* OR into a control write to acknowledge an error */

/* ESC registers this module touches. */
#define REG_TYPE        0x0000
#define REG_STATION     0x0010
#define REG_DL_CTRL     0x0100
#define REG_DL_CTRL_P   0x0101   /* port loop control byte                   */
#define REG_DL_CTRL_3   0x0103
#define REG_DL_STATUS_P 0x0111   /* loop/communication status per port       */
#define REG_AL_CTRL     0x0120
#define REG_AL_STATUS   0x0130
#define REG_AL_CODE     0x0134
#define REG_IRQ_MASK    0x0200
#define REG_ERR_CNT     0x0300
#define REG_EEP_CFG     0x0500
#define REG_FMMU0       0x0600
#define REG_SM0         0x0800
#define REG_DC_RECV     0x0910
#define REG_DC_SPEED    0x0930
#define REG_DC_FILT     0x0934
#define REG_DC_CYC_CTL  0x0981
#define REG_MBX_OUT     0x1000   /* SM0 buffer: mailbox master->slave        */
#define REG_MBX_OUT_END 0x107F   /* last byte; writing it marks mailbox full */
#define REG_MBX_IN      0x1400   /* SM1 buffer: mailbox slave->master        */

/* One configured slave. */
typedef struct {
    int      position;        /* chain position (0-based)                    */
    uint16_t station;         /* station address we assign                   */
    uint8_t  esc_type;        /* 0x0000, must be 0x90 to configure           */
    uint32_t log_addr;        /* logical address of its process data         */
    uint16_t log_len;         /* process-data length in bytes                */
    uint8_t  mbx_bit;         /* bit index in the mailbox-state FMMU         */
    uint16_t al_state;        /* last read AL status                         */
    uint16_t al_code;         /* last read AL status code                    */
} OpSlave;

typedef struct {
    EscCtx    ctx;
    OpSlave   sl[OP_MAX_SLAVES];
    int       n_op;           /* slaves being driven to OP                   */
    int       chain_len;      /* total slaves present, for diagnostics       */
    int       verbose;
} OpMaster;

/* ── Frame construction (pure; exposed for tests) ──────────────────────────
 * Builds a single-datagram frame. adp/ado are placed verbatim, so the caller
 * decides the addressing mode. For a read, pass data = NULL and the length to
 * be requested; the payload is zero-filled. Returns the frame length, or -1
 * if it does not fit. */
int op_build_frame(uint8_t *buf, int buflen, const uint8_t *src_mac,
                   uint8_t cmd, uint8_t idx, uint16_t adp, uint16_t ado,
                   const uint8_t *data, uint16_t len);

/* Builds the mailbox header + CoE header + SDO download for a complete-access
 * write of `payload` to `index`. Returns the total mailbox length, or -1.
 * Two forms, both as TwinCAT used them: a payload of 4 bytes or fewer goes
 * expedited (command specifier 0x33 — the data sits in the size field), and
 * anything longer goes normal (0x31 — a 4-byte size, then the payload). Both
 * set the complete-access bit, because these are whole PDO-mapping objects.
 *
 * `counter` is the mailbox counter in the type byte. TwinCAT increments it
 * per message across a configuration burst (0,1,2...7); the slave uses it to
 * spot a repeated frame, so it must advance. */
int op_build_sdo_download(uint8_t *buf, int buflen, uint16_t index,
                          uint8_t subindex, const uint8_t *payload,
                          uint16_t payload_len, uint8_t counter);

/* ── Transactions ─────────────────────────────────────────────────────────*/
/* Send one datagram and wait for it to come back. Returns the working
 * counter, or -1 on timeout. For reads, up to len bytes land in data. */
int op_xact(OpMaster *m, uint8_t cmd, uint16_t adp, uint16_t ado,
            void *data, uint16_t len);

/* Convenience wrappers. position is 0-based; station is the assigned address.*/
int op_apwr(OpMaster *m, int position, uint16_t ado, const void *d, uint16_t n);
int op_aprd(OpMaster *m, int position, uint16_t ado, void *d, uint16_t n);
int op_fpwr(OpMaster *m, uint16_t station, uint16_t ado, const void *d, uint16_t n);
int op_fprd(OpMaster *m, uint16_t station, uint16_t ado, void *d, uint16_t n);
int op_bwr (OpMaster *m, uint16_t ado, const void *d, uint16_t n);

/* ── Bring-up ─────────────────────────────────────────────────────────────*/
/* Print, in plain language, every class of register this run will write.
 * Called before anything is sent. */
void op_print_write_warning(const OpMaster *m);

/* Reset the bus: clear FMMUs, SyncManagers, DC and error counters, set port
 * loop control, and drive every configured slave to INIT. Broadcast writes
 * touch EVERY slave in the chain, not only the ones being driven to OP. */
int op_bus_reset(OpMaster *m, int chain_len);

/* Read 0x0000 on each configured slave and refuse anything that is not a
 * type-0x90 device. Returns 0 if all pass. */
int op_check_identity(OpMaster *m);

/* Bring one slave up as far as SAFEOP: station address, mailbox
 * SyncManagers, PDO mapping by SDO, process-data SyncManagers, FMMUs,
 * PREOP -> SAFEOP. Returns 0 on success; on failure al_code says why.
 *
 * It stops at SAFEOP deliberately. A drive will not enter OP until valid
 * process data is already arriving, so the last step needs the cyclic
 * exchange running — see op_go_operational(). */
int op_bring_up(OpMaster *m, OpSlave *s);

/* Take every configured slave from SAFEOP to OP, cycling throughout.
 * Primes the outputs with process data first, requests OP, and keeps the
 * cyclic exchange running while waiting — which is what the drives require
 * and what TwinCAT does, its cyclic task never having stopped. Returns 0 when
 * all reach OP. */
int op_go_operational(OpMaster *m, uint32_t log_addr, uint16_t pd_len,
                      int timeout_ms);

/* Request a state and wait for the slave to report it. Returns 0 on success,
 * -1 if the slave signalled an AL error (code in s->al_code), or -2 if it
 * simply never got there — the two need different messages, because an AL
 * code of 0 on a timeout means "nothing was refused", not "no error". */
int op_set_state(OpMaster *m, OpSlave *s, uint16_t state, int timeout_ms);

/* Validate a CoE SDO download response sitting in a mailbox buffer.
 * Returns 0 on success, -1 on an SDO abort (code in *abort_out), -2 if the
 * buffer holds something that is not our answer — a stale response from an
 * earlier request, or a non-CoE message. Pure; unit-tested. */
int op_parse_sdo_response(const uint8_t *mbx, int len, uint16_t expect_index,
                          uint32_t *abort_out);

/* Decode an AL status code into a short phrase. Never returns NULL. */
const char *op_al_code_name(uint16_t code);

/* Drive every configured slave back to INIT. Called on exit so the rig is
 * left as it was found. */
void op_shutdown(OpMaster *m);

/* ── Cyclic exchange ──────────────────────────────────────────────────────
 * One frame per cycle carries everything: the LRW that keeps the drives in
 * OP, and one APRD per slave reading the whole 0x0300-0x0327 diagnostic
 * block. That is the point of this tool — the drives are operational AND
 * every slave's error counters are sampled every cycle, in the same frame,
 * with no extra traffic. */
typedef struct {
    uint16_t lrw_wkc;                                  /* -1 if no response */
    uint8_t  diag[OP_MAX_SLAVES][ESC_DIAG_LEN];
    uint16_t diag_wkc[OP_MAX_SLAVES];
    uint8_t  pd[128];                                  /* process data      */
    uint16_t pd_len;
} OpCycle;

/* Build the cyclic frame. Exposed for tests. Returns the frame length. */
int op_build_cyclic(uint8_t *buf, int buflen, const uint8_t *src_mac,
                    uint8_t idx_base, uint32_t log_addr, const uint8_t *pd,
                    uint16_t pd_len, int chain_len);

/* Send one cyclic frame and parse the response. Returns 0 if the frame came
 * back, -1 on timeout. Per-datagram results are in c. */
int op_cycle(OpMaster *m, OpCycle *c, uint32_t log_addr);

#endif /* ECAT_MASTER_H */
