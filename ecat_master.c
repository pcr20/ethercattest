/* Minimal EtherCAT master. WRITES TO THE SLAVE — see ecat_master.h. */
#include "ecat_master.h"
#include "nic.h"
#include "everest_pdo.h"

#define ETH_MIN_FRAME 60

/* ── Frame construction ───────────────────────────────────────────────────*/
int op_build_frame(uint8_t *buf, int buflen, const uint8_t *src_mac,
                   uint8_t cmd, uint8_t idx, uint16_t adp, uint16_t ado,
                   const uint8_t *data, uint16_t len)
{
    int need = ETH_HDR_LEN + ECAT_HDR_LEN + ECAT_DG_HDR_LEN + len + ECAT_DG_WKC_LEN;
    if (len > 1486 || need > buflen) return -1;

    int clear = need > ETH_MIN_FRAME ? need : ETH_MIN_FRAME;
    memset(buf, 0, (size_t)clear);

    memset(buf, 0xFF, 6);                        /* broadcast destination   */
    memcpy(buf + 6, src_mac, 6);
    buf[12] = (ETHERTYPE_ECAT >> 8) & 0xFF;
    buf[13] =  ETHERTYPE_ECAT       & 0xFF;

    int pos = ETH_HDR_LEN + ECAT_HDR_LEN;
    buf[pos++] = cmd;
    buf[pos++] = idx;
    le16put(buf + pos, adp); pos += 2;
    le16put(buf + pos, ado); pos += 2;
    le16put(buf + pos, (uint16_t)(len & 0x07FF)); pos += 2;   /* more bit 0  */
    buf[pos++] = 0; buf[pos++] = 0;                           /* IRQ         */
    if (data && len) memcpy(buf + pos, data, len);
    pos += len;
    buf[pos++] = 0; buf[pos++] = 0;                           /* WKC         */

    int ecat_len = pos - ETH_HDR_LEN - ECAT_HDR_LEN;
    le16put(buf + ETH_HDR_LEN, (uint16_t)((ecat_len & 0x07FF) | (0x1 << 12)));

    if (pos > buflen) {
        fprintf(stderr, "FATAL: op_build_frame wrote %d into %d bytes\n", pos, buflen);
        abort();
    }
    return pos < ETH_MIN_FRAME ? ETH_MIN_FRAME : pos;
}

/* Mailbox header (ETG.1000.6 §5.6): length(2) address(2) channel+prio(1)
 * type(1), where the type byte also carries the mailbox counter in its high
 * nibble. CoE header (2 bytes LE): bits[15:12] service, 2 = SDO request.
 *
 * The first draft of this function assumed counter 0 throughout and the
 * normal (0x31) download form for everything. t_opseq caught both against
 * TwinCAT's captured bytes: the counter advances per message, and a 4-byte
 * payload goes expedited (0x33) with the data in the size field. Getting
 * either wrong would have been rejected by the drive as a repeated frame or
 * a malformed SDO, with nothing but an abort code to explain it. */
int op_build_sdo_download(uint8_t *buf, int buflen, uint16_t index,
                          uint8_t subindex, const uint8_t *payload,
                          uint16_t payload_len, uint8_t counter)
{
    int expedited = (payload_len <= 4 && payload_len > 0);
    int mbx_body  = 2 /*CoE hdr*/ + 1 /*cs*/ + 2 /*index*/ + 1 /*sub*/ + 4
                  + (expedited ? 0 : payload_len);
    int total     = 6 + mbx_body;
    if (total > buflen) return -1;

    memset(buf, 0, (size_t)total);
    le16put(buf + 0, (uint16_t)mbx_body);        /* length excl. this header */
    le16put(buf + 2, 0);                         /* address                  */
    buf[4] = 0;                                  /* channel + priority       */
    buf[5] = (uint8_t)((counter & 0x07) << 4 | 0x03);   /* counter | CoE     */
    le16put(buf + 6, 0x2000);                    /* CoE service 2 = request  */

    /* Command specifier: ccs=1 (download) | complete access | size indicated,
     * plus the expedited bit and the count of unused data bytes. */
    uint8_t cs = 0x20 | 0x10 | 0x01;
    if (expedited) cs |= (uint8_t)(0x02 | (((4 - payload_len) & 3) << 2));
    buf[8]  = cs;
    le16put(buf + 9, index);
    buf[11] = subindex;

    if (expedited) {
        memcpy(buf + 12, payload, payload_len);  /* data sits in the size    */
    } else {
        buf[12] = (uint8_t)( payload_len        & 0xFF);
        buf[13] = (uint8_t)((payload_len >> 8)  & 0xFF);
        if (payload_len) memcpy(buf + 16, payload, payload_len);
    }
    return total;
}

/* ── Transactions ─────────────────────────────────────────────────────────*/
int op_xact(OpMaster *m, uint8_t cmd, uint16_t adp, uint16_t ado,
            void *data, uint16_t len)
{
    uint8_t tx[1600], rx[2048];
    uint8_t idx = ++m->ctx.idx_seq;
    int is_read = (cmd == ECAT_CMD_APRD || cmd == ECAT_CMD_FPRD_M ||
                   cmd == ECAT_CMD_BRD);

    int n = op_build_frame(tx, (int)sizeof tx, m->ctx.src_mac, cmd, idx,
                           adp, ado, is_read ? NULL : (const uint8_t *)data, len);
    if (n < 0) return -1;

    for (int attempt = 0; attempt < 3; attempt++) {
        if (send(m->ctx.sock, tx, (size_t)n, 0) < 0) return -1;
        m->ctx.frames_sent++;

        uint64_t deadline = now_ns() + (uint64_t)m->ctx.timeout_ms * 1000000ULL;
        while (now_ns() < deadline) {
            ssize_t r = recv(m->ctx.sock, rx, sizeof rx, MSG_DONTWAIT);
            if (r < (ssize_t)(ETH_HDR_LEN + ECAT_HDR_LEN + ECAT_DG_HDR_LEN)) {
                if (r < 0) { struct timespec ts = {0, 50000}; nanosleep(&ts, NULL); }
                continue;
            }
            if (rx[12] != ((ETHERTYPE_ECAT >> 8) & 0xFF) ||
                rx[13] !=  (ETHERTYPE_ECAT & 0xFF)) continue;
            int p = ETH_HDR_LEN + ECAT_HDR_LEN;
            if (rx[p] != cmd || rx[p + 1] != idx) continue;   /* not ours    */
            uint16_t dlen = (uint16_t)(le16get(rx + p + 6) & 0x07FF);
            if (p + ECAT_DG_HDR_LEN + dlen + 2 > r) continue;
            if (is_read && data && dlen)
                memcpy(data, rx + p + ECAT_DG_HDR_LEN, dlen < len ? dlen : len);
            m->ctx.frames_matched++;
            return le16get(rx + p + ECAT_DG_HDR_LEN + dlen);   /* WKC        */
        }
        m->ctx.retries++;
    }
    m->ctx.timeouts++;
    return -1;
}

int op_apwr(OpMaster *m, int position, uint16_t ado, const void *d, uint16_t n)
{   return op_xact(m, ECAT_CMD_APWR_M, (uint16_t)(-(int16_t)position), ado,
                   (void *)d, n); }

int op_aprd(OpMaster *m, int position, uint16_t ado, void *d, uint16_t n)
{   return op_xact(m, ECAT_CMD_APRD, (uint16_t)(-(int16_t)position), ado, d, n); }

int op_fpwr(OpMaster *m, uint16_t station, uint16_t ado, const void *d, uint16_t n)
{   return op_xact(m, ECAT_CMD_FPWR_M, station, ado, (void *)d, n); }

int op_fprd(OpMaster *m, uint16_t station, uint16_t ado, void *d, uint16_t n)
{   return op_xact(m, ECAT_CMD_FPRD_M, station, ado, d, n); }

int op_bwr(OpMaster *m, uint16_t ado, const void *d, uint16_t n)
{   return op_xact(m, ECAT_CMD_BWR_M, 0, ado, (void *)d, n); }

/* ── AL status codes (ETG.1000.6 Table: AL Status Code definitions) ────────
 * Only the ones a bring-up realistically hits are named; anything else is
 * reported as a raw value rather than guessed at. */
const char *op_al_code_name(uint16_t code)
{
    switch (code) {
    case 0x0000: return "no error";
    case 0x0011: return "invalid requested state change";
    case 0x0012: return "unknown requested state";
    case 0x0014: return "invalid mailbox configuration (bootstrap)";
    case 0x0016: return "invalid mailbox configuration (preop)";
    case 0x0017: return "invalid SyncManager configuration";
    case 0x0018: return "no valid inputs available";
    case 0x0019: return "no valid outputs";
    case 0x001A: return "synchronisation error";
    case 0x001B: return "SyncManager watchdog";
    case 0x001D: return "invalid output configuration";
    case 0x001E: return "invalid input configuration";
    case 0x0024: return "invalid input mapping";
    case 0x0025: return "invalid output mapping";
    case 0x0026: return "inconsistent settings";
    case 0x0028: return "free-run needs 3-buffer mode";
    case 0x002D: return "invalid output FMMU configuration";
    case 0x0030: return "invalid DC SYNC configuration";
    case 0x0032: return "PLL error";
    case 0x0035: return "invalid DC sync0 cycle time";
    default:     return "unlisted code — see ETG.1000.6";
    }
}

/* ── Startup warning ──────────────────────────────────────────────────────*/
void op_print_write_warning(const OpMaster *m)
{
    printf("\n");
    printf("  *** THIS TOOL WRITES TO THE SLAVES ***\n");
    printf("\n");
    printf("  Broadcast to ALL %d slave(s) in the chain:\n", m->chain_len);
    printf("    0x0101  port loop control      0x0103  DL control\n");
    printf("    0x0200  interrupt mask         0x0010  station address (cleared)\n");
    printf("    0x0300  error counters CLEARED ONCE at start (then never again)\n");
    printf("    0x0600  all FMMUs cleared      0x0800  all SyncManagers cleared\n");
    printf("    0x0910/0x0930/0x0934/0x0981  distributed-clock registers\n");
    printf("\n");
    printf("  Per slave, only for the %d position(s) named with --op:\n", m->n_op);
    printf("    0x0010  station address        0x0120  AL control (INIT..OP)\n");
    printf("    0x0800/0x0808/0x0810/0x0818  SyncManagers\n");
    printf("    0x0600/0x0610/0x0620         FMMUs\n");
    printf("    0x1000  CoE mailbox: PDO mapping objects 0x1600-0x1602,\n");
    printf("            0x1A00-0x1A02, 0x1C12, 0x1C13\n");
    printf("\n");
    printf("  NOT written: the SII/EEPROM. Nothing here survives a power cycle.\n");
    printf("  On exit every configured slave is driven back to INIT.\n");
    printf("\n");
    printf("  Deviations from the captured TwinCAT sequence (deliberate):\n");
    printf("    - no SII identity read; ESC type at 0x0000 is used instead\n");
    printf("    - no periodic 0x0300 clear; it would erase the evidence\n");
    printf("\n");
}

/* ── Bus reset ────────────────────────────────────────────────────────────
 * Mirrors the capture at t=8.668-8.690. The 256-byte FMMU and SyncManager
 * clears and the DC writes are broadcast, so they reach every slave in the
 * chain — including ones not being driven to OP. Those slaves are in INIT
 * with nothing configured, so there is nothing to lose, but it is stated in
 * the warning because it is not confined to the --op list. */
int op_bus_reset(OpMaster *m, int chain_len)
{
    uint8_t zero[256]; memset(zero, 0, sizeof zero);
    uint8_t v1;
    uint16_t v2;

    v1 = 0x00; if (op_bwr(m, REG_DL_CTRL_P, &v1, 1) < 0) return -1;
    if (op_bwr(m, REG_DL_CTRL_P, &v1, 1) < 0) return -1;   /* twice, as TwinCAT */
    if (op_bwr(m, REG_ERR_CNT, zero, 8) < 0) return -1;

    /* Port loop control: every slave forwards to the next (0xF4 = port 1
     * auto-close); the last one closes port 1 (0xFC) because nothing follows.
     * TwinCAT wrote exactly this for its two-slave chain. */
    for (int p = 0; p < chain_len; p++) {
        v1 = (p == chain_len - 1) ? 0xFC : 0xF4;
        if (op_apwr(m, p, REG_DL_CTRL_P, &v1, 1) < 0) return -1;
    }

    v2 = 0x0004; if (op_bwr(m, REG_IRQ_MASK, &v2, 2) < 0) return -1;
    v2 = 0x0000; if (op_bwr(m, REG_STATION,  &v2, 2) < 0) return -1;
    if (op_bwr(m, REG_ERR_CNT,  zero, 8)   < 0) return -1;
    if (op_bwr(m, REG_FMMU0,    zero, 256) < 0) return -1;
    if (op_bwr(m, REG_SM0,      zero, 256) < 0) return -1;
    if (op_bwr(m, REG_DC_RECV,  zero, 32)  < 0) return -1;
    v1 = 0x00;   if (op_bwr(m, REG_DC_CYC_CTL, &v1, 1) < 0) return -1;
    v2 = 0x1000; if (op_bwr(m, REG_DC_SPEED,   &v2, 2) < 0) return -1;
    v2 = 0x0C00; if (op_bwr(m, REG_DC_FILT,    &v2, 2) < 0) return -1;
    v1 = 0x00;   if (op_bwr(m, REG_DL_CTRL_3,  &v1, 1) < 0) return -1;

    /* INIT with error acknowledge, per configured slave. */
    for (int i = 0; i < m->n_op; i++) {
        v2 = AL_INIT | AL_ERR_ACK;
        if (op_apwr(m, m->sl[i].position, REG_AL_CTRL, &v2, 2) < 0) return -1;
    }
    return 0;
}

int op_check_identity(OpMaster *m)
{
    int bad = 0;
    for (int i = 0; i < m->n_op; i++) {
        uint8_t t = 0;
        int wkc = op_aprd(m, m->sl[i].position, REG_TYPE, &t, 1);
        if (wkc < 1) {
            fprintf(stderr, "  position %d: no response to identity read\n",
                    m->sl[i].position);
            bad = 1; continue;
        }
        m->sl[i].esc_type = t;
        printf("  position %2d: ESC type 0x%02X %s\n", m->sl[i].position, t,
               t == 0x90 ? "(EVE-NET — will be configured)"
                         : "*** NOT type 0x90 — REFUSED ***");
        if (t != 0x90) bad = 1;
    }
    if (bad)
        fprintf(stderr,
            "\n  The PDO mapping in this tool was captured from an Everest NET\n"
            "  drive. Writing it to anything else is a guess, so nothing has\n"
            "  been configured. Check the --op positions.\n");
    return bad ? -1 : 0;
}

/* ── State machine ────────────────────────────────────────────────────────*/
int op_set_state(OpMaster *m, OpSlave *s, uint16_t state, int timeout_ms)
{
    uint16_t req = state;
    if (op_fpwr(m, s->station, REG_AL_CTRL, &req, 2) < 1) return -1;

    uint64_t deadline = now_ns() + (uint64_t)timeout_ms * 1000000ULL;
    for (;;) {
        uint16_t st = 0;
        if (op_fprd(m, s->station, REG_AL_STATUS, &st, 2) >= 1) {
            s->al_state = st;
            if ((st & 0x0F) == (state & 0x0F) && !(st & 0x10)) return 0;
            if (st & 0x10) {                       /* slave signalled error  */
                uint16_t code = 0;
                op_fprd(m, s->station, REG_AL_CODE, &code, 2);
                s->al_code = code;
                return -1;
            }
        }
        if (now_ns() > deadline) return -2;      /* never arrived          */
        struct timespec ts = {0, 1000000}; nanosleep(&ts, NULL);   /* 1 ms   */
    }
}

/* Validate a CoE SDO download response. See ecat_master.h. */
int op_parse_sdo_response(const uint8_t *mbx, int len, uint16_t expect_index,
                          uint32_t *abort_out)
{
    if (abort_out) *abort_out = 0;
    if (len < 16) return -2;
    uint16_t mbx_len = le16get(mbx);
    if (mbx_len == 0) return -2;                 /* empty buffer            */
    if ((mbx[5] & 0x0F) != 0x03) return -2;      /* not CoE                 */
    uint16_t index = (uint16_t)(mbx[9] | (mbx[10] << 8));
    uint8_t  cs    = mbx[8];
    if (cs == 0x80) {                            /* SDO abort               */
        if (abort_out)
            *abort_out = (uint32_t)mbx[12] | ((uint32_t)mbx[13] << 8) |
                         ((uint32_t)mbx[14] << 16) | ((uint32_t)mbx[15] << 24);
        return -1;
    }
    /* A download response is command specifier 0x60. Anything else, or an
     * answer about a different object, is not the confirmation we asked for:
     * treating a stale buffer as success is how the first version of this
     * function walked past a full mailbox and wedged the drive. */
    if (index != expect_index) return -2;
    if ((cs & 0xE0) != 0x60) return -2;
    return 0;
}

/* Write one CoE SDO into the mailbox and wait for the slave to answer.
 *
 * The payload goes into SM0's buffer at 0x1000; the final byte of the buffer
 * (0x107F) is then written separately, which is what marks the mailbox full.
 *
 * The response MUST be read as the full 0x80-byte buffer. A SyncManager
 * buffer is only released when the access ends at its last byte, so a short
 * read leaves the mailbox full, the slave cannot queue its next response, and
 * it stops acknowledging further requests — which is exactly how the first
 * version of this failed on hardware at the third object, silently.
 *
 * The working counter doubles as the "is there anything there" flag: reading
 * a buffer that is not full is not acknowledged, so WKC 0 means "not yet"
 * and needs no guess about SyncManager status bits. */
#define MBX_BUF_LEN 128

static int op_sdo_write(OpMaster *m, OpSlave *s, uint16_t index,
                        uint8_t subindex, const uint8_t *payload, uint16_t plen,
                        uint8_t counter)
{
    uint8_t mbx[MBX_BUF_LEN];
    int n = op_build_sdo_download(mbx, (int)sizeof mbx, index, subindex,
                                  payload, plen, counter);
    if (n < 0) return -1;

    int wkc = op_fpwr(m, s->station, REG_MBX_OUT, mbx, (uint16_t)n);
    if (wkc < 1) {
        fprintf(stderr, "  SDO 0x%04X: mailbox write not accepted (wkc %d) — "
                "the mailbox is probably still full\n", index, wkc);
        return -1;
    }
    uint8_t trigger = 0;
    wkc = op_fpwr(m, s->station, REG_MBX_OUT_END, &trigger, 1);
    if (wkc < 1) {
        fprintf(stderr, "  SDO 0x%04X: mailbox trigger not accepted (wkc %d)\n",
                index, wkc);
        return -1;
    }

    for (int attempt = 0; attempt < 400; attempt++) {
        uint8_t in[MBX_BUF_LEN];
        memset(in, 0, sizeof in);
        /* Full-buffer read: releases the buffer AND tells us, via the WKC,
         * whether there was anything in it. */
        int rwkc = op_fprd(m, s->station, REG_MBX_IN, in, MBX_BUF_LEN);
        if (rwkc >= 1) {
            uint32_t abort_code = 0;
            int r = op_parse_sdo_response(in, MBX_BUF_LEN, index, &abort_code);
            if (r == 0) return 0;
            if (r == -1) {
                fprintf(stderr, "  SDO 0x%04X:%u ABORTED by the drive, "
                        "code 0x%08X\n", index, subindex, abort_code);
                return -1;
            }
            /* Something else was in the buffer. It is now released, so keep
             * waiting for our own answer rather than calling this a success. */
        }
        struct timespec ts = {0, 500000}; nanosleep(&ts, NULL);   /* 0.5 ms  */
    }
    fprintf(stderr, "  SDO 0x%04X:%u — no mailbox response in 200 ms\n",
            index, subindex);
    return -1;
}

/* The PDO mapping comes from everest_pdo.h, generated from the capture.
 * It is NOT written out here: the hand-written version was wrong in four of
 * eight objects and cost a hardware run. */
int op_bring_up(OpMaster *m, OpSlave *s)
{
    uint8_t  sm[16], fmmu[16];
    uint16_t v2;

    /* Station address, so fixed addressing works from here on. */
    v2 = s->station;
    if (op_apwr(m, s->position, REG_STATION, &v2, 2) < 1) {
        fprintf(stderr, "  position %d: station address write failed\n", s->position);
        return -1;
    }

    /* Mailbox SyncManagers. SM0 out at 0x1000, SM1 in at 0x1400, both 0x80
     * bytes, control bytes 0x26 and 0x22 as captured. */
    memset(sm, 0, 16);
    if (op_fpwr(m, s->station, REG_SM0,        sm, 16) < 1) return -1;
    { const uint8_t sm0[8] = {0x00,0x10, 0x80,0x00, 0x26, 0x00, 0x01, 0x00};
      if (op_fpwr(m, s->station, REG_SM0,      sm0, 8) < 1) return -1; }
    { const uint8_t sm1[8] = {0x00,0x14, 0x80,0x00, 0x22, 0x00, 0x01, 0x00};
      if (op_fpwr(m, s->station, REG_SM0 + 8,  sm1, 8) < 1) return -1; }
    if (op_fpwr(m, s->station, REG_SM0 + 16,   sm, 16) < 1) return -1;

    /* FMMU 2 maps the SM1 status byte (0x080D) into the logical image one bit
     * per slave, so mailbox-full can be polled with process data. */
    { uint8_t f2[16] = {0x00,0x00,0x00,0x09, 0x01,0x00, s->mbx_bit, s->mbx_bit,
                        0x0D,0x08, 0x00, 0x01, 0x01, 0x00,0x00,0x00};
      if (op_fpwr(m, s->station, REG_FMMU0 + 32, f2, 16) < 1) return -1; }

    { uint8_t one = 0x01;
      if (op_fpwr(m, s->station, REG_EEP_CFG, &one, 1) < 1) return -1; }

    if (op_set_state(m, s, AL_PREOP, 2000) < 0) {
        fprintf(stderr, "  position %d: PREOP failed — AL state 0x%02X, "
                "code 0x%04X (%s)\n", s->position, s->al_state & 0x0F,
                s->al_code, op_al_code_name(s->al_code));
        return -1;
    }

    /* PDO mapping. Order matters: the assignment objects 0x1C12/0x1C13 are
     * written last, after the maps they refer to exist. */
    for (int i = 0; i < EVEREST_PDO_N; i++)
        if (op_sdo_write(m, s, everest_pdo[i].index, 0, everest_pdo[i].data,
                         everest_pdo[i].len, (uint8_t)i) < 0) {
            fprintf(stderr, "  position %d: PDO mapping failed at 0x%04X\n",
                    s->position, everest_pdo[i].index);
            return -1;
        }

    /* Process-data SyncManagers: SM2 outputs at 0x1800, SM3 inputs at 0x1C00. */
    { const uint8_t sm2[8] = {0x00,0x18, (uint8_t)(s->log_len & 0xFF), 0x00,
                              0x64, 0x00, 0x01, 0x00};
      if (op_fpwr(m, s->station, REG_SM0 + 16, sm2, 8) < 1) return -1; }
    { const uint8_t sm3[8] = {0x00,0x1C, (uint8_t)(s->log_len & 0xFF), 0x00,
                              0x20, 0x00, 0x01, 0x00};
      if (op_fpwr(m, s->station, REG_SM0 + 24, sm3, 8) < 1) return -1; }

    /* FMMU 0 = outputs (type 2, write), FMMU 1 = inputs (type 1, read), both
     * at the same logical address so one LRW moves both. */
    { uint32_t la = s->log_addr;
      uint8_t f0[16] = {(uint8_t)la,(uint8_t)(la>>8),(uint8_t)(la>>16),(uint8_t)(la>>24),
                        (uint8_t)(s->log_len & 0xFF), 0x00, 0x00, 0x07,
                        0x00,0x18, 0x00, 0x02, 0x01, 0x00,0x00,0x00};
      if (op_fpwr(m, s->station, REG_FMMU0, f0, 16) < 1) return -1;
      memcpy(fmmu, f0, 16);
      fmmu[8] = 0x00; fmmu[9] = 0x1C; fmmu[11] = 0x01;   /* inputs from 0x1C00 */
      if (op_fpwr(m, s->station, REG_FMMU0 + 16, fmmu, 16) < 1) return -1; }

    int r = op_set_state(m, s, AL_SAFEOP, 2000);
    if (r == -1) {
        fprintf(stderr, "  position %d: SAFEOP refused — AL code 0x%04X (%s)\n",
                s->position, s->al_code, op_al_code_name(s->al_code));
        return -1;
    }
    if (r == -2) {
        fprintf(stderr, "  position %d: SAFEOP timed out, slave stayed in "
                "state 0x%02X\n", s->position, s->al_state & 0x0F);
        return -1;
    }
    return 0;      /* OP needs process data flowing: op_go_operational() */
}

/* SAFEOP -> OP, with the cyclic exchange running throughout.
 *
 * The drive will not leave SAFEOP until it is receiving valid outputs. The
 * first attempt requested OP with nothing on the wire yet and simply timed
 * out, reporting an AL code of 0 — no refusal, no error, just a slave that
 * never moved. TwinCAT never hits this because its cyclic task is already
 * running when it writes the state. */
int op_go_operational(OpMaster *m, uint32_t log_addr, uint16_t pd_len,
                      int timeout_ms)
{
    OpCycle c; memset(&c, 0, sizeof c);
    c.pd_len = pd_len;

    /* Prime the outputs so the drives have seen valid process data before
     * they are asked to go operational. */
    for (int i = 0; i < 100; i++) {
        op_cycle(m, &c, log_addr);
        struct timespec ts = {0, 2000000}; nanosleep(&ts, NULL);   /* 2 ms  */
    }

    for (int i = 0; i < m->n_op; i++) {
        uint16_t req = AL_OP;
        if (op_fpwr(m, m->sl[i].station, REG_AL_CTRL, &req, 2) < 1) {
            fprintf(stderr, "  position %d: OP request not accepted\n",
                    m->sl[i].position);
            return -1;
        }
    }

    uint64_t deadline = now_ns() + (uint64_t)timeout_ms * 1000000ULL;
    int done = 0;
    while (now_ns() < deadline) {
        op_cycle(m, &c, log_addr);          /* keep the outputs alive        */
        done = 0;
        for (int i = 0; i < m->n_op; i++) {
            uint16_t st = 0;
            if (op_fprd(m, m->sl[i].station, REG_AL_STATUS, &st, 2) >= 1) {
                m->sl[i].al_state = st;
                if (st & 0x10) {
                    uint16_t code = 0;
                    op_fprd(m, m->sl[i].station, REG_AL_CODE, &code, 2);
                    m->sl[i].al_code = code;
                    fprintf(stderr, "  position %d: OP refused — AL code "
                            "0x%04X (%s)\n", m->sl[i].position, code,
                            op_al_code_name(code));
                    return -1;
                }
                if ((st & 0x0F) == AL_OP) done++;
            }
        }
        if (done == m->n_op) return 0;
        struct timespec ts = {0, 2000000}; nanosleep(&ts, NULL);
    }
    for (int i = 0; i < m->n_op; i++)
        fprintf(stderr, "  position %d: still in state 0x%02X after %d ms\n",
                m->sl[i].position, m->sl[i].al_state & 0x0F, timeout_ms);
    return -1;
}

void op_shutdown(OpMaster *m)
{
    for (int i = 0; i < m->n_op; i++) {
        uint16_t req = AL_INIT;
        op_fpwr(m, m->sl[i].station, REG_AL_CTRL, &req, 2);
    }
}

/* ── Cyclic exchange ──────────────────────────────────────────────────────
 * One frame: LRW over the process-data image, then one APRD per slave over
 * the whole diagnostic block. Datagram indices are idx_base for the LRW and
 * idx_base+1+i for slave i, so the response can be demultiplexed. */
int op_build_cyclic(uint8_t *buf, int buflen, const uint8_t *src_mac,
                    uint8_t idx_base, uint32_t log_addr, const uint8_t *pd,
                    uint16_t pd_len, int chain_len)
{
    int need = ETH_HDR_LEN + ECAT_HDR_LEN
             + (ECAT_DG_OVERHEAD + pd_len)
             + chain_len * (ECAT_DG_OVERHEAD + ESC_DIAG_LEN);
    if (need > buflen || chain_len > OP_MAX_SLAVES) return -1;

    memset(buf, 0, (size_t)(need > ETH_MIN_FRAME ? need : ETH_MIN_FRAME));
    memset(buf, 0xFF, 6);
    memcpy(buf + 6, src_mac, 6);
    buf[12] = (ETHERTYPE_ECAT >> 8) & 0xFF;
    buf[13] =  ETHERTYPE_ECAT       & 0xFF;

    int pos = ETH_HDR_LEN + ECAT_HDR_LEN;

    /* LRW: the 32-bit logical address occupies the ADP and ADO fields. */
    buf[pos++] = ECAT_CMD_LRW_M;
    buf[pos++] = idx_base;
    le16put(buf + pos, (uint16_t)(log_addr & 0xFFFF));        pos += 2;
    le16put(buf + pos, (uint16_t)((log_addr >> 16) & 0xFFFF)); pos += 2;
    le16put(buf + pos, (uint16_t)((pd_len & 0x07FF) | 0x8000)); pos += 2;
    buf[pos++] = 0; buf[pos++] = 0;
    if (pd && pd_len) memcpy(buf + pos, pd, pd_len);
    pos += pd_len;
    buf[pos++] = 0; buf[pos++] = 0;

    for (int i = 0; i < chain_len; i++) {
        int last = (i == chain_len - 1);
        buf[pos++] = ECAT_CMD_APRD;
        buf[pos++] = (uint8_t)(idx_base + 1 + i);
        le16put(buf + pos, (uint16_t)(-(int16_t)i)); pos += 2;
        le16put(buf + pos, ESC_DIAG_BASE);          pos += 2;
        le16put(buf + pos, (uint16_t)(ESC_DIAG_LEN | (last ? 0 : 0x8000)));
        pos += 2;
        buf[pos++] = 0; buf[pos++] = 0;
        memset(buf + pos, 0, ESC_DIAG_LEN); pos += ESC_DIAG_LEN;
        buf[pos++] = 0; buf[pos++] = 0;
    }

    int ecat_len = pos - ETH_HDR_LEN - ECAT_HDR_LEN;
    le16put(buf + ETH_HDR_LEN, (uint16_t)((ecat_len & 0x07FF) | (0x1 << 12)));
    if (pos > buflen) {
        fprintf(stderr, "FATAL: op_build_cyclic wrote %d into %d\n", pos, buflen);
        abort();
    }
    return pos < ETH_MIN_FRAME ? ETH_MIN_FRAME : pos;
}

int op_cycle(OpMaster *m, OpCycle *c, uint32_t log_addr)
{
    uint8_t tx[1600], rx[2048];
    uint8_t base = m->ctx.idx_seq;
    m->ctx.idx_seq = (uint8_t)(base + m->chain_len + 1);

    int n = op_build_cyclic(tx, (int)sizeof tx, m->ctx.src_mac, base, log_addr,
                            c->pd, c->pd_len, m->chain_len);
    if (n < 0) return -1;
    if (send(m->ctx.sock, tx, (size_t)n, 0) < 0) return -1;
    m->ctx.frames_sent++;

    c->lrw_wkc = 0xFFFF;
    for (int i = 0; i < m->chain_len; i++) c->diag_wkc[i] = 0xFFFF;

    uint64_t deadline = now_ns() + (uint64_t)m->ctx.timeout_ms * 1000000ULL;
    while (now_ns() < deadline) {
        ssize_t r = recv(m->ctx.sock, rx, sizeof rx, MSG_DONTWAIT);
        if (r < (ssize_t)(ETH_HDR_LEN + ECAT_HDR_LEN + ECAT_DG_HDR_LEN)) {
            if (r < 0) { struct timespec ts = {0, 20000}; nanosleep(&ts, NULL); }
            continue;
        }
        if (rx[12] != ((ETHERTYPE_ECAT >> 8) & 0xFF) ||
            rx[13] !=  (ETHERTYPE_ECAT & 0xFF)) continue;
        int p = ETH_HDR_LEN + ECAT_HDR_LEN;
        if (rx[p] != ECAT_CMD_LRW_M || rx[p + 1] != base) continue;

        /* Walk the datagram chain, filling in what we asked for. */
        int more = 1;
        while (more && p + ECAT_DG_HDR_LEN <= r) {
            uint8_t cmd = rx[p], idx = rx[p + 1];
            uint16_t lf = le16get(rx + p + 6);
            uint16_t dl = lf & 0x07FF;
            more = (lf & 0x8000) != 0;
            if (p + ECAT_DG_HDR_LEN + dl + 2 > r) break;
            const uint8_t *dat = rx + p + ECAT_DG_HDR_LEN;
            uint16_t wkc = le16get(dat + dl);
            if (cmd == ECAT_CMD_LRW_M && idx == base) {
                c->lrw_wkc = wkc;
                if (dl && dl <= sizeof c->pd) memcpy(c->pd, dat, dl);
            } else if (cmd == ECAT_CMD_APRD) {
                int s = (int)(uint8_t)(idx - base) - 1;
                if (s >= 0 && s < m->chain_len && dl >= ESC_DIAG_LEN) {
                    memcpy(c->diag[s], dat, ESC_DIAG_LEN);
                    c->diag_wkc[s] = wkc;
                }
            }
            p += ECAT_DG_HDR_LEN + dl + 2;
        }
        m->ctx.frames_matched++;
        return 0;
    }
    m->ctx.timeouts++;
    return -1;
}
