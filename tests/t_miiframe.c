/* MII-over-ESC tests: APWR wire format read exactly as an ESC reads it
 * (little-endian, ETG.1000.4), the 0x0512 PHY selector encoding checked
 * against the vendor procedure byte-for-byte, the 0x0510 command words, and
 * the invariant that the READ-ONLY module can still never emit a write. */
#include "crc.c"
#include "stats.c"
#include "frame.c"
#include "nic.c"
#include "escreg.c"
#include "escmii.c"

static int fails = 0;
#define CHECK(c, ...) do { if(!(c)) { printf("FAIL: "); printf(__VA_ARGS__); \
                                      printf("\n"); fails++; } } while (0)

static const uint8_t SRC[6] = {0x84,0x47,0x09,0x81,0xef,0x71};

int main(void)
{
    crc32c_init();                    /* §4.3 invariant: always first */
    uint8_t buf[MAX_FRAME];

    /* ── T1: the 0x0512 selector encoding, against the vendor procedure ──
     * "WRITE to 0x0512 value 0x0B00 -> Register 0x0B, Address PHY 0"
     * "WRITE to 0x0512 value 0x0F00 -> Register 0x0F (FLDS), PHY 0"
     * 0x0512 is the PHY ADDRESS byte and 0x0513 the PHY REGISTER byte, so a
     * 16-bit LE write of V puts (V&0xFF) at 0x0512 and (V>>8) at 0x0513. */
    CHECK(mii_sel(0, 0x0B) == 0x0B00, "mii_sel(0,0x0B)=0x%04X want 0x0B00", mii_sel(0,0x0B));
    CHECK(mii_sel(0, 0x0F) == 0x0F00, "mii_sel(0,0x0F)=0x%04X want 0x0F00", mii_sel(0,0x0F));
    CHECK(mii_sel(1, 0x00) == 0x0001, "mii_sel(1,0x00)=0x%04X want 0x0001", mii_sel(1,0x00));
    {   /* verify the BYTES land where the ESC expects them */
        uint8_t d[2]; le16put(d, mii_sel(0, 0x0B));
        CHECK(d[0] == 0x00, "0x0512 byte (PHY addr) = 0x%02X want 0x00", d[0]);
        CHECK(d[1] == 0x0B, "0x0513 byte (PHY reg)  = 0x%02X want 0x0B", d[1]);
        le16put(d, mii_sel(3, 0x0F));
        CHECK(d[0] == 0x03, "0x0512 byte (PHY addr) = 0x%02X want 0x03", d[0]);
        CHECK(d[1] == 0x0F, "0x0513 byte (PHY reg)  = 0x%02X want 0x0F", d[1]);
    }
    if (!fails) printf("T1 PASS: 0x0512 selector matches vendor procedure "
                       "(0x0B00/0x0F00), bytes land correctly\n");

    /* ── T2: 0x0510 command words, from the vendor procedure ────────────── */
    int f0 = fails;
    CHECK(MII_CMD_READ  == 0x0100, "read cmd 0x%04X want 0x0100", MII_CMD_READ);
    CHECK(MII_CMD_WRITE == 0x0201, "write cmd 0x%04X want 0x0201", MII_CMD_WRITE);
    CHECK((MII_CMD_READ  & (1u << 8)) != 0, "read cmd must set bit 8");
    CHECK((MII_CMD_WRITE & (1u << 0)) != 0, "write cmd must set bit 0 (write enable)");
    CHECK((MII_CMD_WRITE & (1u << 9)) != 0, "write cmd must set bit 9 (write command)");
    CHECK(MII_STAT_BUSY == 0x8000, "busy must be bit 15");
    if (fails == f0) printf("T2 PASS: 0x0510 read=0x0100 write=0x0201 busy=bit15\n");

    /* ── T3: APWR frame, walked exactly as an ESC reads it ──────────────── */
    f0 = fails;
    {
        uint8_t data[2]; le16put(data, 0x140F);      /* the FLD-enable value */
        int len = esc_build_write_frame(buf, sizeof(buf), SRC, 0, 0x21,
                                        0x0514, data, 2);
        CHECK(len == 60, "APWR frame len=%d want 60 (Ethernet minimum)", len);
        CHECK(buf[12] == 0x88 && buf[13] == 0xA4, "EtherType must be BE 0x88A4");

        uint16_t eh = (uint16_t)(buf[14] | (buf[15] << 8));   /* LE */
        CHECK(((eh >> 12) & 0xF) == 1, "ECAT type=%u want 1", (eh >> 12) & 0xF);
        CHECK((eh & 0x07FF) == 14, "ECAT len=%u want 14 (10 hdr + 2 data + 2 wkc)",
              eh & 0x07FF);

        CHECK(buf[16] == ECAT_CMD_APWR, "cmd=0x%02X want APWR 0x02", buf[16]);
        CHECK(buf[16] != ECAT_CMD_APRD, "APWR must differ from APRD");
        CHECK(buf[17] == 0x21, "idx=0x%02X want 0x21", buf[17]);
        uint16_t adp = (uint16_t)(buf[18] | (buf[19] << 8));
        uint16_t ado = (uint16_t)(buf[20] | (buf[21] << 8));
        CHECK(adp == 0, "ADP=%u want 0 for position 0", adp);
        CHECK(ado == 0x0514, "ADO=0x%04X want 0x0514", ado);
        uint16_t lf = (uint16_t)(buf[22] | (buf[23] << 8));
        CHECK((lf & 0x07FF) == 2, "dgram len=%u want 2", lf & 0x07FF);
        CHECK(((lf >> 15) & 1) == 0, "single datagram must clear the more bit");
        /* the payload the ESC will write into 0x0514, little-endian */
        CHECK(buf[26] == 0x0F && buf[27] == 0x14,
              "payload = %02X %02X want 0F 14 (0x140F LE)", buf[26], buf[27]);
    }
    if (fails == f0) printf("T3 PASS: APWR frame LE-correct, cmd=0x02, "
                            "ADO=0x0514, payload 0x140F little-endian\n");

    /* ── T4: ADP = -position for a chain, as for APRD ────────────────────── */
    f0 = fails;
    for (uint16_t p = 0; p < 5; p++) {
        uint8_t d[2] = {0xAA, 0xBB};
        int len = esc_build_write_frame(buf, sizeof(buf), SRC, p, 0, 0x0510, d, 2);
        CHECK(len > 0, "build failed for position %u", p);
        uint16_t adp = (uint16_t)(buf[18] | (buf[19] << 8));
        CHECK(adp == (uint16_t)(-(int16_t)p), "position %u -> ADP 0x%04X", p, adp);
    }
    if (fails == f0) printf("T4 PASS: ADP = -position (LE) for positions 0..4\n");

    /* ── T5: response parsing recovers the WKC, rejects foreign frames ──── */
    f0 = fails;
    {
        uint8_t d[2]; le16put(d, 0x3100);
        int len = esc_build_write_frame(buf, sizeof(buf), SRC, 0, 0x55,
                                        0x0512, d, 2);
        CHECK(len > 0, "build failed");
        le16put(buf + 28, 1);                       /* slave sets WKC = 1 */
        CHECK(parse_write_resp(buf, len, 0x55, 2) == 1, "WKC should parse as 1");
        CHECK(parse_write_resp(buf, len, 0x56, 2) == -1, "foreign idx must be rejected");
        buf[16] = ECAT_CMD_APRD;
        CHECK(parse_write_resp(buf, len, 0x55, 2) == -1, "APRD reply must not match APWR");
        buf[16] = ECAT_CMD_APWR; buf[13] = 0x00;
        CHECK(parse_write_resp(buf, len, 0x55, 2) == -1, "non-EtherCAT frame must be rejected");
    }
    if (fails == f0) printf("T5 PASS: APWR response WKC parsed, foreign idx / "
                            "APRD / non-EtherCAT rejected\n");

    /* ── T6: budget guards ──────────────────────────────────────────────── */
    f0 = fails;
    {
        uint8_t d[ESC_MAX_DATA + 8];
        memset(d, 0, sizeof(d));
        CHECK(esc_build_write_frame(buf, sizeof(buf), SRC, 0, 0, 0, d, 0) == -1,
              "zero-length write must be rejected");
        CHECK(esc_build_write_frame(buf, sizeof(buf), SRC, 0, 0, 0, d,
                                    ESC_MAX_DATA + 1) == -1,
              "oversize write must be rejected");
        CHECK(esc_build_write_frame(buf, 20, SRC, 0, 0, 0, d, 2) == -1,
              "undersize buffer must be rejected");
    }
    if (fails == f0) printf("T6 PASS: length and buffer guards reject bad input\n");

    /* ── T7: the READ-ONLY module still cannot emit a write ─────────────────
     * ecat_regdump links escreg.o and NOT escmii.o. Guard the property that
     * escreg's builder emits APRD for every datagram it produces. */
    f0 = fails;
    {
        EscRead r[4];
        for (int i = 0; i < 4; i++) {
            memset(&r[i], 0, sizeof(r[i]));
            r[i].addr = (uint16_t)(0x0300 + 4 * i); r[i].len = 4;
        }
        int len = esc_build_read_frame(buf, sizeof(buf), SRC, 0, 0, r, 4);
        CHECK(len > 0, "read frame build failed");
        int pos = 16, n = 0;
        while (pos + ECAT_DG_HDR_LEN <= len) {
            CHECK(buf[pos] == ECAT_CMD_APRD,
                  "datagram %d cmd=0x%02X — read builder emitted a non-APRD command",
                  n, buf[pos]);
            CHECK(buf[pos] != ECAT_CMD_APWR, "datagram %d must never be APWR", n);
            uint16_t lf = (uint16_t)(buf[pos+6] | (buf[pos+7] << 8));
            pos += ECAT_DG_HDR_LEN + (lf & 0x07FF) + ECAT_DG_WKC_LEN;
            n++;
            if (!((lf >> 15) & 1)) break;
        }
        CHECK(n == 4, "expected 4 datagrams, walked %d", n);
    }
    if (fails == f0) printf("T7 PASS: escreg builder still emits APRD only "
                            "(ecat_regdump stays read-only)\n");

    if (fails) { printf("\n%d MII-FRAME CHECK(S) FAILED\n", fails); return 1; }
    printf("\nALL MII-FRAME TESTS PASS\n");
    return 0;
}
