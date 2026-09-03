/* ESC register-dump frame tests: wire format read exactly as an ESC reads it
 * (little-endian, ETG.1000.4), position addressing, Ethernet minimum length,
 * and build->parse round trip. This is the test that would have caught the
 * htons endianness bug that made every frame a runt on real slaves. */
#include "crc.c"
#include "stats.c"
#include "frame.c"
#include "nic.c"
#include "escreg.c"

static int fails = 0;
#define CHECK(c, ...) do { if(!(c)) { printf("FAIL: "); printf(__VA_ARGS__); \
                                      printf("\n"); fails++; } } while (0)

static const uint8_t SRC[6] = {0x84,0x47,0x09,0x82,0xbc,0x24};

/* Walk the frame the way an ESC does: LE header, LE datagram len/more. */
static int walk(const uint8_t *buf, int len, int *ndg, uint16_t *etype)
{
    uint16_t eh = (uint16_t)(buf[14] | (buf[15] << 8));
    *etype = (eh >> 12) & 0xF;
    uint16_t elen = eh & 0x07FF;
    int pos = 16, n = 0;
    while (pos + 12 <= len) {
        uint16_t lf = (uint16_t)(buf[pos+6] | (buf[pos+7] << 8));
        uint16_t dl = lf & 0x07FF;
        int more = (lf >> 15) & 1;
        pos += 10 + dl + 2;
        n++;
        if (!more) break;
    }
    *ndg = n;
    /* ESC-visible end of the EtherCAT payload */
    return (pos == 16 + elen) ? pos : -1;
}

int main(void)
{
    crc32c_init();

    /* T1: single read, wire format as an ESC sees it. */
    {
        uint8_t buf[MAX_FRAME];
        EscRead r = { .addr = 0x0000, .len = 1 };
        int flen = esc_build_read_frame(buf, sizeof(buf), SRC, 0, 0, &r, 1);
        CHECK(flen >= 60, "T1 frame len %d < Ethernet minimum 60", flen);
        CHECK(buf[12] == 0x88 && buf[13] == 0xA4,
              "T1 EtherType %02x%02x (want 88A4, big-endian)", buf[12], buf[13]);
        int ndg; uint16_t et;
        int end = walk(buf, flen, &ndg, &et);
        CHECK(et == 1, "T1 EtherCAT type %u (want 1) — endianness bug?", et);
        CHECK(ndg == 1, "T1 walked %d datagrams (want 1)", ndg);
        CHECK(end > 0, "T1 datagram chain does not land on the declared end");
        CHECK(buf[16] == ECAT_CMD_APRD, "T1 cmd %u (want APRD=1)", buf[16]);
        if (!fails) printf("T1 PASS: single APRD, LE type=1, chain exact, len=%d\n", flen);
    }

    /* T2: position addressing — ADP must be -position, little-endian. */
    {
        uint8_t buf[MAX_FRAME];
        EscRead r = { .addr = 0x0300, .len = 8 };
        for (int p = 0; p <= 4; p++) {
            int flen = esc_build_read_frame(buf, sizeof(buf), SRC,
                                            (uint16_t)p, 0, &r, 1);
            (void)flen;
            uint16_t adp = (uint16_t)(buf[18] | (buf[19] << 8));
            uint16_t ado = (uint16_t)(buf[20] | (buf[21] << 8));
            CHECK(adp == (uint16_t)(-(int16_t)p),
                  "T2 pos %d: ADP=0x%04x want 0x%04x", p, adp,
                  (uint16_t)(-(int16_t)p));
            CHECK(ado == 0x0300, "T2 ADO=0x%04x want 0x0300", ado);
        }
        printf("T2 PASS: ADP = -position (LE) for positions 0..4, ADO correct\n");
    }

    /* T3: multi-datagram frame — more-bits set on all but the last. */
    {
        uint8_t buf[MAX_FRAME];
        EscRead r[4];
        memset(r, 0, sizeof(r));
        for (int i = 0; i < 4; i++) { r[i].addr = (uint16_t)(0x0100 + i*2); r[i].len = 2; }
        int flen = esc_build_read_frame(buf, sizeof(buf), SRC, 0, 0x10, r, 4);
        int ndg; uint16_t et;
        int end = walk(buf, flen, &ndg, &et);
        CHECK(ndg == 4, "T3 walked %d datagrams (want 4)", ndg);
        CHECK(end > 0, "T3 chain does not land on declared end");
        /* idx must increment from the base so responses can be matched. */
        CHECK(buf[17] == 0x10, "T3 first idx %u (want 0x10)", buf[17]);
        printf("T3 PASS: 4-datagram chain, more-bits correct, idx base honoured\n");
    }

    /* T4: build -> (simulate slave fill) -> parse round trip. */
    {
        uint8_t buf[MAX_FRAME];
        EscRead tx[3];
        memset(tx, 0, sizeof(tx));
        tx[0].addr = 0x0000; tx[0].len = 2;
        tx[1].addr = 0x0110; tx[1].len = 2;
        tx[2].addr = 0x0310; tx[2].len = 4;
        int flen = esc_build_read_frame(buf, sizeof(buf), SRC, 0, 0x20, tx, 3);

        /* Slave fills data and sets WKC=1 on each datagram. */
        int pos = 16;
        uint8_t fill[3][4] = { {0x11,0x02}, {0x3F,0x00}, {1,2,3,4} };
        for (int i = 0; i < 3; i++) {
            uint16_t dl = (uint16_t)(buf[pos+6] | (buf[pos+7] << 8)) & 0x07FF;
            memcpy(buf + pos + 10, fill[i], dl);
            le16put(buf + pos + 10 + dl, 1);        /* WKC = 1 */
            pos += 10 + dl + 2;
        }

        EscRead rx[3];
        memcpy(rx, tx, sizeof(rx));
        for (int i = 0; i < 3; i++) rx[i].ok = 0;
        int m = esc_parse_read_frame(buf, flen, 0x20, rx, 3);
        CHECK(m == 3, "T4 matched %d datagrams (want 3)", m);
        CHECK(rx[0].ok && rx[0].wkc == 1 && rx[0].data[0] == 0x11,
              "T4 dg0 ok=%d wkc=%u data0=0x%02x", rx[0].ok, rx[0].wkc, rx[0].data[0]);
        CHECK(rx[2].ok && rx[2].data[3] == 4, "T4 dg2 lost-link data mismatch");
        printf("T4 PASS: build->fill->parse round trip, data and WKC recovered\n");
    }

    /* T5: parser rejects frames that are not ours (wrong idx base). */
    {
        uint8_t buf[MAX_FRAME];
        EscRead tx = { .addr = 0x0000, .len = 2 };
        int flen = esc_build_read_frame(buf, sizeof(buf), SRC, 0, 0x30, &tx, 1);
        EscRead rx = { .addr = 0x0000, .len = 2 };
        int m = esc_parse_read_frame(buf, flen, 0x99, &rx, 1);   /* wrong base */
        CHECK(m == 0 && !rx.ok, "T5 matched a foreign frame (m=%d)", m);
        /* and a non-EtherCAT frame is rejected outright */
        buf[12] = 0x08; buf[13] = 0x00;                          /* IPv4 */
        m = esc_parse_read_frame(buf, flen, 0x30, &rx, 1);
        CHECK(m == -1, "T5 accepted a non-EtherCAT frame (m=%d)", m);
        printf("T5 PASS: foreign idx and non-EtherCAT frames rejected\n");
    }

    /* T6: budget guards — oversize/zero length and too many datagrams. */
    {
        uint8_t buf[MAX_FRAME];
        EscRead r = { .addr = 0, .len = ESC_MAX_DATA + 1 };
        CHECK(esc_build_read_frame(buf, sizeof(buf), SRC, 0, 0, &r, 1) < 0,
              "T6 oversize datagram accepted");
        r.len = 0;
        CHECK(esc_build_read_frame(buf, sizeof(buf), SRC, 0, 0, &r, 1) < 0,
              "T6 zero-length datagram accepted");
        EscRead many[ESC_MAX_DGRAMS + 1];
        memset(many, 0, sizeof(many));
        for (unsigned i = 0; i < ESC_MAX_DGRAMS + 1; i++) { many[i].addr = 0; many[i].len = 2; }
        CHECK(esc_build_read_frame(buf, sizeof(buf), SRC, 0, 0,
                                   many, ESC_MAX_DGRAMS + 1) < 0,
              "T6 too many datagrams accepted");
        printf("T6 PASS: length and datagram-count guards reject bad input\n");
    }

    /* T7: READ-ONLY by construction — no APWR opcode anywhere in a built
     * frame, for any tier address we dump. */
    {
        uint8_t buf[MAX_FRAME];
        /* Every address any tier dumps. Keep in step with regdump_main.c's
         * tier tables — a register added there but not here would not be
         * covered by this read-only guarantee. */
        uint16_t addrs[] = {0x0000,0x0007,0x0110,0x0500,0x0510,0x0516,0x0517,
                            0x0300,0x0308,0x030C,0x030D,0x0310,0x0314,0x0320};
        for (unsigned i = 0; i < sizeof(addrs)/sizeof(addrs[0]); i++) {
            EscRead r = { .addr = addrs[i], .len = 2 };
            int flen = esc_build_read_frame(buf, sizeof(buf), SRC, 0, 0, &r, 1);
            int pos = 16, n = 0;
            while (pos + 12 <= flen && n < 8) {
                CHECK(buf[pos] == ECAT_CMD_APRD,
                      "T7 addr 0x%04x: cmd 0x%02x is not APRD", addrs[i], buf[pos]);
                uint16_t lf = (uint16_t)(buf[pos+6] | (buf[pos+7] << 8));
                pos += 10 + (lf & 0x07FF) + 2; n++;
                if (!((lf >> 15) & 1)) break;
            }
        }
        printf("T7 PASS: every tier register builds APRD only (read-only)\n");
    }

    printf("\n%s\n", fails ? "*** ESC FRAME FAILURES ***" : "ALL ESC-FRAME TESTS PASS");
    return fails;
}
