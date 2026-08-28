/* ESC register access — READ-ONLY (APRD only). See escreg.h. */
#include "escreg.h"
#include "nic.h"

/* ── Frame construction ─────────────────────────────────────────────────────
 * Layout: [Eth 14][ECAT hdr 2][ dgram: hdr 10 + data + WKC 2 ] * n [pad]
 * All EtherCAT fields little-endian (ETG.1000.4). The Ethernet EtherType is
 * genuinely big-endian (network order) and is the one field that stays BE.
 *
 * PADDING: unlike the BER tester's full-size frames, a register-read frame is
 * tiny. Ethernet requires a minimum 60-byte frame (64 with FCS), so we pad
 * explicitly rather than relying on driver behaviour. */
#define ETH_MIN_FRAME 60

int esc_build_read_frame(uint8_t *buf, int buflen, const uint8_t *src_mac,
                         uint16_t position, uint8_t idx_base,
                         const EscRead *reads, int n) {
    if (n < 1 || n > ESC_MAX_DGRAMS) return -1;

    /* Budget check up front: header + per-datagram (hdr+data+wkc). */
    int need = ETH_HDR_LEN + ECAT_HDR_LEN;
    for (int i = 0; i < n; i++) {
        if (reads[i].len == 0 || reads[i].len > ESC_MAX_DATA) return -1;
        need += ECAT_DG_HDR_LEN + reads[i].len + ECAT_DG_WKC_LEN;
    }
    if (need > buflen) return -1;

    memset(buf, 0, (size_t)(need > ETH_MIN_FRAME ? need : ETH_MIN_FRAME));

    /* Ethernet: broadcast destination, our MAC, EtherType 0x88A4 (BE). */
    memset(buf, 0xFF, 6);
    memcpy(buf + 6, src_mac, 6);
    buf[12] = (ETHERTYPE_ECAT >> 8) & 0xFF;
    buf[13] =  ETHERTYPE_ECAT       & 0xFF;

    int pos = ETH_HDR_LEN + ECAT_HDR_LEN;   /* ECAT header filled in at end */

    /* ADP = -position (two's complement); each slave increments it, so the
     * addressed slave sees 0 and processes the datagram. */
    uint16_t adp = (uint16_t)(-(int16_t)position);

    for (int i = 0; i < n; i++) {
        int is_last = (i == n - 1);
        buf[pos++] = ECAT_CMD_APRD;
        buf[pos++] = (uint8_t)(idx_base + i);      /* echoed back; matching */
        le16put(buf + pos, adp);            pos += 2;   /* ADP (position)   */
        le16put(buf + pos, reads[i].addr);  pos += 2;   /* ADO (register)   */
        le16put(buf + pos, (uint16_t)(reads[i].len & 0x07FF)
                           | (is_last ? 0 : 0x8000));   /* len + more bit   */
        pos += 2;
        buf[pos++] = 0; buf[pos++] = 0;                 /* IRQ              */
        pos += reads[i].len;                            /* data (zeroed)    */
        buf[pos++] = 0; buf[pos++] = 0;                 /* WKC              */
    }

    /* EtherCAT header: length of all datagrams, type 1, little-endian. */
    int ecat_len = pos - ETH_HDR_LEN - ECAT_HDR_LEN;
    le16put(buf + ETH_HDR_LEN, (uint16_t)((ecat_len & 0x07FF) | (0x1 << 12)));

    if (pos > buflen) {   /* accounting self-check, as in build_frame() */
        fprintf(stderr, "FATAL: esc_build_read_frame wrote %d into %d bytes\n",
                pos, buflen);
        abort();
    }
    /* Pad to the Ethernet minimum; padding is outside the EtherCAT length so
     * slaves ignore it. */
    if (pos < ETH_MIN_FRAME) pos = ETH_MIN_FRAME;
    return pos;
}

int esc_parse_read_frame(const uint8_t *buf, int len, uint8_t idx_base,
                         EscRead *reads, int n) {
    if (len < ETH_HDR_LEN + ECAT_HDR_LEN + ECAT_DG_HDR_LEN) return -1;
    if (buf[12] != ((ETHERTYPE_ECAT >> 8) & 0xFF) ||
        buf[13] != (ETHERTYPE_ECAT & 0xFF)) return -1;

    uint16_t eh = le16get(buf + ETH_HDR_LEN);
    if (((eh >> 12) & 0xF) != 1) return -1;      /* not an EtherCAT type-1 frame */

    int pos = ETH_HDR_LEN + ECAT_HDR_LEN, matched = 0;
    for (int i = 0; i < n; i++) {
        if (pos + ECAT_DG_HDR_LEN > len) break;
        uint8_t  cmd    = buf[pos];
        uint8_t  idx    = buf[pos + 1];
        uint16_t lf     = le16get(buf + pos + 6);
        uint16_t dg_len = lf & 0x07FF;
        int      more   = (lf >> 15) & 1;
        pos += ECAT_DG_HDR_LEN;
        if (pos + dg_len + ECAT_DG_WKC_LEN > len) break;

        /* Match strictly on our rolling index and the command we sent. */
        if (cmd == ECAT_CMD_APRD && idx == (uint8_t)(idx_base + i) &&
            dg_len == reads[i].len) {
            memcpy(reads[i].data, buf + pos, dg_len);
            reads[i].wkc = le16get(buf + pos + dg_len);
            reads[i].ok  = 1;
            matched++;
        }
        pos += dg_len + ECAT_DG_WKC_LEN;
        if (!more) break;
    }
    return matched;
}

/* ── Socket setup ───────────────────────────────────────────────────────── */
int esc_open(EscCtx *ctx, const char *iface, uint16_t position, int timeout_ms) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->position   = position;
    ctx->timeout_ms = timeout_ms > 0 ? timeout_ms : 10;

    int sock = socket(AF_PACKET, SOCK_RAW, htons(ETHERTYPE_ECAT));
    if (sock < 0) { perror("socket"); return -1; }

    int ifindex = get_ifindex(sock, iface);
    if (ifindex < 0) { fprintf(stderr, "cannot get ifindex for %s\n", iface);
                       close(sock); return -1; }
    if (get_mac(sock, iface, ctx->src_mac) < 0) {
        fprintf(stderr, "cannot get MAC for %s\n", iface); close(sock); return -1; }

    struct sockaddr_ll sll;
    memset(&sll, 0, sizeof(sll));
    sll.sll_family   = AF_PACKET;
    sll.sll_protocol = htons(ETHERTYPE_ECAT);
    sll.sll_ifindex  = ifindex;
    if (bind(sock, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
        perror("bind"); close(sock); return -1; }

    ctx->sock = sock;
    ctx->ifindex = ifindex;
    return 0;
}

void esc_close(EscCtx *ctx) {
    if (ctx->sock > 0) close(ctx->sock);
    ctx->sock = -1;
}

/* ── Transaction: send one frame, wait for our response ─────────────────── */
int esc_read_multi(EscCtx *ctx, EscRead *reads, int n) {
    uint8_t tx[MAX_FRAME], rx[MAX_FRAME + 32];

    for (int attempt = 0; attempt < 3; attempt++) {
        uint8_t idx_base = ctx->idx_seq;
        ctx->idx_seq = (uint8_t)(ctx->idx_seq + n);

        for (int i = 0; i < n; i++) { reads[i].ok = 0; reads[i].wkc = 0; }

        int flen = esc_build_read_frame(tx, sizeof(tx), ctx->src_mac,
                                        ctx->position, idx_base, reads, n);
        if (flen < 0) return -1;
        if (send(ctx->sock, tx, flen, 0) < 0) {
            if (attempt == 2) { perror("send"); return -1; }
            continue;
        }
        ctx->frames_sent++;

        /* Wait for OUR frame back. Other frames (none expected — we are the
         * only master) are skipped without consuming the timeout budget. */
        uint64_t deadline = now_ns() + (uint64_t)ctx->timeout_ms * 1000000ULL;
        for (;;) {
            uint64_t now = now_ns();
            if (now >= deadline) break;
            int wait_ms = (int)((deadline - now) / 1000000ULL) + 1;
            struct pollfd pfd = { .fd = ctx->sock, .events = POLLIN };
            int pr = poll(&pfd, 1, wait_ms);
            if (pr <= 0) break;

            int rlen = (int)recv(ctx->sock, rx, sizeof(rx), MSG_DONTWAIT);
            if (rlen <= 0) continue;
            if (esc_parse_read_frame(rx, rlen, idx_base, reads, n) > 0) {
                ctx->frames_matched++;
                return 0;
            }
        }
        ctx->retries++;
    }
    ctx->timeouts++;
    return -1;
}

int esc_read_range(EscCtx *ctx, uint16_t addr, uint16_t len, uint8_t *buf) {
    int last_wkc = -1;
    uint16_t done = 0;
    while (done < len) {
        EscRead r[ESC_MAX_DGRAMS];
        int n = 0;
        while (n < ESC_MAX_DGRAMS && done < len) {
            uint16_t chunk = (uint16_t)(len - done);
            if (chunk > ESC_MAX_DATA) chunk = ESC_MAX_DATA;
            memset(&r[n], 0, sizeof(r[n]));
            r[n].addr = (uint16_t)(addr + done);
            r[n].len  = chunk;
            done = (uint16_t)(done + chunk);
            n++;
        }
        if (esc_read_multi(ctx, r, n) != 0) return -1;
        uint16_t off = (uint16_t)(r[0].addr - addr);
        for (int i = 0; i < n; i++) {
            if (!r[i].ok) return -1;
            memcpy(buf + off, r[i].data, r[i].len);
            off = (uint16_t)(off + r[i].len);
            last_wkc = r[i].wkc;
        }
    }
    return last_wkc;
}
