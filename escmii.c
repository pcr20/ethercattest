/* PHY register access over the ESC MII management interface. WRITES. See
 * escmii.h — this module is linked into ecat_phy only, never ecat_regdump. */
#include "escmii.h"
#include "nic.h"

#define ETH_MIN_FRAME 60

/* ── APWR frame construction ───────────────────────────────────────────────
 * Identical layout to esc_build_read_frame() except the command byte is APWR
 * and the data field carries the payload to be written rather than zeros.
 * All EtherCAT fields little-endian (ETG.1000.4); EtherType stays big-endian. */
int esc_build_write_frame(uint8_t *buf, int buflen, const uint8_t *src_mac,
                          uint16_t position, uint8_t idx,
                          uint16_t addr, const uint8_t *data, uint16_t len)
{
    if (len == 0 || len > ESC_MAX_DATA) return -1;
    int need = ETH_HDR_LEN + ECAT_HDR_LEN + ECAT_DG_HDR_LEN + len + ECAT_DG_WKC_LEN;
    if (need > buflen) return -1;

    memset(buf, 0, (size_t)(need > ETH_MIN_FRAME ? need : ETH_MIN_FRAME));

    memset(buf, 0xFF, 6);                       /* broadcast destination */
    memcpy(buf + 6, src_mac, 6);
    buf[12] = (ETHERTYPE_ECAT >> 8) & 0xFF;
    buf[13] =  ETHERTYPE_ECAT       & 0xFF;

    int pos = ETH_HDR_LEN + ECAT_HDR_LEN;
    uint16_t adp = (uint16_t)(-(int16_t)position);   /* ADP = -position */

    buf[pos++] = ECAT_CMD_APWR;
    buf[pos++] = idx;
    le16put(buf + pos, adp);  pos += 2;              /* ADP */
    le16put(buf + pos, addr); pos += 2;              /* ADO */
    le16put(buf + pos, (uint16_t)(len & 0x07FF));    /* len, more bit clear */
    pos += 2;
    buf[pos++] = 0; buf[pos++] = 0;                  /* IRQ */
    memcpy(buf + pos, data, len); pos += len;        /* payload to write */
    buf[pos++] = 0; buf[pos++] = 0;                  /* WKC */

    int ecat_len = pos - ETH_HDR_LEN - ECAT_HDR_LEN;
    le16put(buf + ETH_HDR_LEN, (uint16_t)((ecat_len & 0x07FF) | (0x1 << 12)));

    if (pos > buflen) {   /* accounting self-check, as in build_frame() */
        fprintf(stderr, "FATAL: esc_build_write_frame wrote %d into %d bytes\n",
                pos, buflen);
        abort();
    }
    if (pos < ETH_MIN_FRAME) pos = ETH_MIN_FRAME;
    return pos;
}

/* Parse an APWR response: we only need the WKC back. Returns WKC or -1. */
static int parse_write_resp(const uint8_t *buf, int len, uint8_t idx, uint16_t wlen)
{
    if (len < ETH_HDR_LEN + ECAT_HDR_LEN + ECAT_DG_HDR_LEN) return -1;
    if (buf[12] != ((ETHERTYPE_ECAT >> 8) & 0xFF) ||
        buf[13] != (ETHERTYPE_ECAT & 0xFF)) return -1;
    uint16_t eh = le16get(buf + ETH_HDR_LEN);
    if (((eh >> 12) & 0xF) != 1) return -1;

    int pos = ETH_HDR_LEN + ECAT_HDR_LEN;
    if (pos + ECAT_DG_HDR_LEN > len) return -1;
    uint8_t  cmd    = buf[pos];
    uint8_t  ridx   = buf[pos + 1];
    uint16_t dg_len = le16get(buf + pos + 6) & 0x07FF;
    pos += ECAT_DG_HDR_LEN;
    if (pos + dg_len + ECAT_DG_WKC_LEN > len) return -1;
    if (cmd != ECAT_CMD_APWR || ridx != idx || dg_len != wlen) return -1;
    return (int)le16get(buf + pos + dg_len);
}

int esc_write(EscCtx *ctx, uint16_t addr, const uint8_t *data, uint16_t len)
{
    uint8_t tx[MAX_FRAME], rx[MAX_FRAME + 32];

    for (int attempt = 0; attempt < 3; attempt++) {
        uint8_t idx = ctx->idx_seq++;
        int flen = esc_build_write_frame(tx, sizeof(tx), ctx->src_mac,
                                         ctx->position, idx, addr, data, len);
        if (flen < 0) return -1;
        if (send(ctx->sock, tx, flen, 0) < 0) {
            if (attempt == 2) { perror("send"); return -1; }
            continue;
        }
        ctx->frames_sent++;

        uint64_t deadline = now_ns() + (uint64_t)ctx->timeout_ms * 1000000ULL;
        for (;;) {
            uint64_t now = now_ns();
            if (now >= deadline) break;
            int wait_ms = (int)((deadline - now) / 1000000ULL) + 1;
            struct pollfd pfd = { .fd = ctx->sock, .events = POLLIN };
            if (poll(&pfd, 1, wait_ms) <= 0) break;
            int rlen = (int)recv(ctx->sock, rx, sizeof(rx), MSG_DONTWAIT);
            if (rlen <= 0) continue;
            int wkc = parse_write_resp(rx, rlen, idx, len);
            if (wkc >= 0) { ctx->frames_matched++; return wkc; }
        }
        ctx->retries++;
    }
    ctx->timeouts++;
    return -1;
}

int esc_write16(EscCtx *ctx, uint16_t addr, uint16_t value) {
    uint8_t d[2]; le16put(d, value);
    return esc_write(ctx, addr, d, 2);
}

int esc_read16(EscCtx *ctx, uint16_t addr, uint16_t *value) {
    uint8_t b[2];
    int wkc = esc_read_range(ctx, addr, 2, b);
    if (wkc < 1) return -1;
    *value = le16get(b);
    return 0;
}

int esc_read8(EscCtx *ctx, uint16_t addr, uint8_t *value) {
    uint8_t b[1];
    int wkc = esc_read_range(ctx, addr, 1, b);
    if (wkc < 1) return -1;
    *value = b[0];
    return 0;
}

/* ── MII management ─────────────────────────────────────────────────────── */
int mii_wait_idle(EscCtx *ctx, int timeout_ms, uint16_t *status_out)
{
    uint64_t deadline = now_ns() + (uint64_t)timeout_ms * 1000000ULL;
    for (;;) {
        uint16_t st;
        if (esc_read16(ctx, ESC_MII_CTRL, &st) != 0) return -1;
        if (status_out) *status_out = st;
        if (!(st & MII_STAT_BUSY)) return 0;
        if (now_ns() >= deadline) return -1;
        sleep_ns(200 * 1000);          /* 200 us between polls */
    }
}

int mii_read_phy(EscCtx *ctx, uint8_t phy_addr, uint8_t phy_reg,
                 uint16_t *value, uint16_t *status_out)
{
    uint16_t st = 0;

    /* Interface must be idle before we touch the selector. */
    if (mii_wait_idle(ctx, 100, &st) != 0) return -1;

    /* 1. Select PHY address (0x0512) + PHY register (0x0513) in one LE word. */
    if (esc_write16(ctx, ESC_MII_PHYADR, mii_sel(phy_addr, phy_reg)) < 1) return -1;

    /* 2. Issue the read command. */
    if (esc_write16(ctx, ESC_MII_CTRL, MII_CMD_READ) < 1) return -1;

    /* 3. Wait for busy to clear. */
    if (mii_wait_idle(ctx, 100, &st) != 0) return -1;
    if (status_out) *status_out = st;
    if (st & (MII_STAT_CMD_ERR | MII_STAT_READ_ERR)) return -2;

    /* 4. Read the 16-bit data register. NOTE: the vendor read procedure says
     * "read 0x0515", which is only the HIGH BYTE of this 16-bit register. We
     * read the full word so no bits are lost; the caller prints both. */
    if (esc_read16(ctx, ESC_MII_DATA, value) != 0) return -1;
    return 0;
}

int mii_write_phy(EscCtx *ctx, uint8_t phy_addr, uint8_t phy_reg,
                  uint16_t value, int verify, uint16_t *readback)
{
    uint16_t st = 0;

    if (mii_wait_idle(ctx, 100, &st) != 0) return -1;

    /* 1. Select PHY address + register. */
    if (esc_write16(ctx, ESC_MII_PHYADR, mii_sel(phy_addr, phy_reg)) < 1) return -1;
    /* 2. Load the data to be written. */
    if (esc_write16(ctx, ESC_MII_DATA, value) < 1) return -1;
    /* 3. Write enable (bit 0) + write command (bit 9). */
    if (esc_write16(ctx, ESC_MII_CTRL, MII_CMD_WRITE) < 1) return -1;
    /* 4. Wait for busy to clear. */
    if (mii_wait_idle(ctx, 100, &st) != 0) return -1;
    if (st & MII_STAT_CMD_ERR) return -2;

    /* 5. Verify with a read-back. */
    if (verify) {
        uint16_t rb = 0;
        if (mii_read_phy(ctx, phy_addr, phy_reg, &rb, NULL) != 0) return -1;
        if (readback) *readback = rb;
        if (rb != value) return -3;
    }
    return 0;
}

/* ── Extended / MMD access ──────────────────────────────────────────────────
 * Sequence (SNLS505H Table 8-13, SNLA265 Table 9):
 *   1. REGCR <- DEVAD                (command 00 = address)
 *   2. ADDAR <- register address
 *   3. REGCR <- 0x4000|DEVAD         (command 01 = data, no post-increment)
 *   4. read ADDAR                    -> the register value
 * The block variant uses command 10 (post-increment on read and write) so
 * step 4 can be repeated, each read advancing the internal pointer. */
static int mmd_set_pointer(EscCtx *ctx, uint8_t phy_addr, uint8_t devad,
                           uint16_t reg, uint16_t data_cmd)
{
    if (mii_write_phy(ctx, phy_addr, PHY_REG_REGCR,
                      MMD_CMD_ADDR(devad), 0, NULL) != 0) return -1;
    if (mii_write_phy(ctx, phy_addr, PHY_REG_ADDAR, reg, 0, NULL) != 0) return -1;
    if (mii_write_phy(ctx, phy_addr, PHY_REG_REGCR, data_cmd, 0, NULL) != 0) return -1;
    return 0;
}

int mii_mmd_read(EscCtx *ctx, uint8_t phy_addr, uint8_t devad,
                 uint16_t reg, uint16_t *value)
{
    if (mmd_set_pointer(ctx, phy_addr, devad, reg, MMD_CMD_DATA(devad)) != 0)
        return -1;
    return mii_read_phy(ctx, phy_addr, PHY_REG_ADDAR, value, NULL);
}

int mii_mmd_read_block(EscCtx *ctx, uint8_t phy_addr, uint8_t devad,
                       uint16_t start_reg, int n, uint16_t *out)
{
    if (n < 1) return -1;
    if (mmd_set_pointer(ctx, phy_addr, devad, start_reg,
                        MMD_CMD_DATA_INC(devad)) != 0) return -1;
    for (int i = 0; i < n; i++)
        if (mii_read_phy(ctx, phy_addr, PHY_REG_ADDAR, &out[i], NULL) != 0)
            return -1;
    return 0;
}
