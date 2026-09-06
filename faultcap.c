/* Fault capture: pcap frame logging + error-triggered PHY probe. See faultcap.h */
#include "faultcap.h"
#include "escmii.h"          /* ESC/MII transport: APRD + APWR              */
#include "phy_regs.h"        /* phy_id_present(), DP83822 register names    */

/* ── Rings ─────────────────────────────────────────────────────────────────
 * SPSC, single RX producer and single supervisor consumer, drop-and-count
 * when full. Sized so a long burst cannot wedge the producer. */
#define FRAME_RING   256
#define EVENT_RING   4096

typedef struct { uint64_t t_ns; uint32_t reason; uint16_t len;
                 uint8_t  buf[MAX_FRAME + 8]; } FrameRec;
typedef struct { uint64_t t_ns; int16_t slave; int16_t port;
                 uint32_t delta; char counter[12]; } EventRec;

static FrameRec  g_fring[FRAME_RING];
static EventRec  g_ering[EVENT_RING];
static _Atomic uint64_t g_fhead, g_ftail, g_fdrop;
static _Atomic uint64_t g_ehead, g_etail, g_edrop;
static _Atomic uint64_t g_events;        /* total ESC error events seen      */
static _Atomic uint64_t g_last_event_ns; /* for burst-settled detection      */

static FILE *g_pcap, *g_events_csv, *g_probes;
static int   g_nslaves;
static uint64_t g_probe_seq;

/* ── pcap ──────────────────────────────────────────────────────────────────
 * Classic libpcap format, written directly — no library. LINKTYPE_ETHERNET.
 * NOTE: with rx-fcs on, frames carry their 4-byte FCS trailer, which normal
 * Ethernet captures do not. Wireshark may flag the trailing bytes; that is
 * expected and the FCS is exactly what we want retained. */
static int pcap_write_header(FILE *f) {
    struct { uint32_t magic; uint16_t vmaj, vmin; int32_t tz; uint32_t sig, snap, link; }
    h = { 0xa1b2c3d4u, 2, 4, 0, 0, 65535, 1 };
    return fwrite(&h, sizeof(h), 1, f) == 1 ? 0 : -1;
}
static void pcap_write_frame(FILE *f, uint64_t t_ns, const uint8_t *b, int len) {
    struct { uint32_t sec, usec, caplen, origlen; } ph;
    ph.sec     = (uint32_t)(t_ns / 1000000000ULL);
    ph.usec    = (uint32_t)((t_ns % 1000000000ULL) / 1000ULL);
    ph.caplen  = (uint32_t)len;
    ph.origlen = (uint32_t)len;
    fwrite(&ph, sizeof(ph), 1, f);
    fwrite(b, 1, (size_t)len, f);
}

int faultcap_open(const char *iface, int num_slaves, const char *dir) {
    (void)iface;
    char p[512];
    g_nslaves = num_slaves;
    snprintf(p, sizeof(p), "%s/frames.pcap", dir);
    g_pcap = fopen(p, "wb");
    if (!g_pcap || pcap_write_header(g_pcap) != 0) {
        fprintf(stderr, "faultcap: cannot open %s\n", p); return -1; }
    snprintf(p, sizeof(p), "%s/events.csv", dir);
    g_events_csv = fopen(p, "w");
    if (!g_events_csv) { fprintf(stderr, "faultcap: cannot open %s\n", p); return -1; }
    fprintf(g_events_csv, "t_rel_s,kind,slave,port,counter,delta,frame_len,reason\n");
    snprintf(p, sizeof(p), "%s/probes.txt", dir);
    g_probes = fopen(p, "w");
    if (!g_probes) { fprintf(stderr, "faultcap: cannot open %s\n", p); return -1; }
    fflush(g_events_csv);
    return 0;
}

void faultcap_close(void) {
    faultcap_flush();
    if (g_pcap) fclose(g_pcap);
    if (g_events_csv) fclose(g_events_csv);
    if (g_probes) fclose(g_probes);
    g_pcap = g_events_csv = g_probes = NULL;
}

void faultcap_frame(uint64_t t_ns, uint32_t reason, const uint8_t *buf, int len) {
    if (len <= 0 || len > MAX_FRAME + 8) return;
    uint64_t h = atomic_load_explicit(&g_fhead, memory_order_relaxed);
    uint64_t t = atomic_load_explicit(&g_ftail, memory_order_acquire);
    if (h - t >= FRAME_RING) {                    /* full: drop and count */
        atomic_fetch_add_explicit(&g_fdrop, 1, memory_order_relaxed); return; }
    FrameRec *r = &g_fring[h % FRAME_RING];
    r->t_ns = t_ns; r->reason = reason; r->len = (uint16_t)len;
    memcpy(r->buf, buf, (size_t)len);
    atomic_store_explicit(&g_fhead, h + 1, memory_order_release);
}

void faultcap_esc_event(uint64_t t_ns, int slave, int port,
                        const char *counter, unsigned delta) {
    atomic_fetch_add_explicit(&g_events, 1, memory_order_relaxed);
    atomic_store_explicit(&g_last_event_ns, t_ns, memory_order_relaxed);
    uint64_t h = atomic_load_explicit(&g_ehead, memory_order_relaxed);
    uint64_t t = atomic_load_explicit(&g_etail, memory_order_acquire);
    if (h - t >= EVENT_RING) {
        atomic_fetch_add_explicit(&g_edrop, 1, memory_order_relaxed); return; }
    EventRec *r = &g_ering[h % EVENT_RING];
    r->t_ns = t_ns; r->slave = (int16_t)slave; r->port = (int16_t)port;
    r->delta = delta;
    snprintf(r->counter, sizeof(r->counter), "%s", counter);
    atomic_store_explicit(&g_ehead, h + 1, memory_order_release);
}

uint64_t faultcap_event_count(void) {
    return atomic_load_explicit(&g_events, memory_order_relaxed);
}

int faultcap_burst_settled(uint64_t now_ns, uint64_t quiet_ns) {
    uint64_t last = atomic_load_explicit(&g_last_event_ns, memory_order_relaxed);
    if (!last) return 0;
    return (now_ns - last) >= quiet_ns;
}

void faultcap_flush(void) {
    uint64_t t = atomic_load_explicit(&g_ftail, memory_order_relaxed);
    uint64_t h = atomic_load_explicit(&g_fhead, memory_order_acquire);
    while (t < h) {
        FrameRec *r = &g_fring[t % FRAME_RING];
        if (g_pcap) pcap_write_frame(g_pcap, r->t_ns, r->buf, r->len);
        if (g_events_csv)
            fprintf(g_events_csv, "%.9f,frame,,,,,%u,0x%X\n",
                    (double)r->t_ns / 1e9, r->len, r->reason);
        t++;
    }
    atomic_store_explicit(&g_ftail, t, memory_order_release);

    t = atomic_load_explicit(&g_etail, memory_order_relaxed);
    h = atomic_load_explicit(&g_ehead, memory_order_acquire);
    while (t < h) {
        EventRec *r = &g_ering[t % EVENT_RING];
        if (g_events_csv)
            fprintf(g_events_csv, "%.9f,esc,%d,%d,%s,%u,,\n",
                    (double)r->t_ns / 1e9, r->slave, r->port, r->counter, r->delta);
        t++;
    }
    atomic_store_explicit(&g_etail, t, memory_order_release);
    if (g_pcap) fflush(g_pcap);
    if (g_events_csv) fflush(g_events_csv);
}

/* ── The triggered probe ───────────────────────────────────────────────────
 * Reads only the PERISHABLE registers, in the order that protects them
 * (SNLS505H Table 8-16: PHYSTS latch bits are cleared by reading BMSR, ANER,
 * MISR1, FCSCR, RECR and 10BTSCR, so PHYSTS must be read FIRST). Static
 * configuration registers are omitted deliberately — they do not change and
 * reading them lengthens the pause during which no measurement is happening.
 *
 * CR3 is the one exception: it is static, but it says whether Fast Link Down
 * is armed, which is the context every FLDS reading needs. */
static const struct { uint8_t reg; const char *name; } probe_regs[] = {
    { 0x10, "PHYSTS" },   /* first — see above                */
    { 0x0F, "FLDS"   },   /* which FLD criterion fired, if any */
    { 0x12, "MISR1"  },   /* link/energy/RX-error interrupts   */
    { 0x13, "MISR2"  },
    { 0x01, "BMSR"   },   /* link, latch-low                   */
    { 0x06, "ANER"   },   /* parallel-detect fault             */
    { 0x14, "FCSCR"  },   /* false carrier count               */
    { 0x15, "RECR"   },   /* RX_ER count — 16-bit              */
    { 0x0B, "CR3"    },   /* FLD armed? static, for context    */
};
#define NPROBE_REGS ((int)(sizeof(probe_regs)/sizeof(probe_regs[0])))

int faultcap_probe(const char *iface, uint64_t trigger_ns) {
    faultcap_flush();                    /* frames and events to disk first */
    if (!g_probes) return 0;

    uint64_t seq = ++g_probe_seq;
    fprintf(g_probes, "\n═══ probe %lu, triggered at t=+%.6f s, after %lu event(s) ═══\n",
            seq, (double)trigger_ns / 1e9, faultcap_event_count());

    int probed = 0;
    for (int pos = 0; pos < g_nslaves; pos++) {
        EscCtx ctx;
        if (esc_open(&ctx, iface, (uint16_t)pos, 10) != 0) {
            fprintf(g_probes, "  pos %d: cannot open socket\n", pos); continue; }

        /* Is MII reachable, and does the PDI hold it? */
        uint16_t ctrl = 0; uint8_t pdi = 0;
        int have_ctrl = (esc_read16(&ctx, ESC_MII_CTRL, &ctrl) == 0);
        int have_pdi  = (esc_read8(&ctx, ESC_MII_PDI_ACC, &pdi) == 0);
        if (!have_ctrl) {
            fprintf(g_probes, "  pos %d: no MII management\n", pos);
            esc_close(&ctx); continue;
        }
        if (have_pdi && (pdi & 1)) {
            fprintf(g_probes, "  pos %d: PDI owns MII — not probed\n", pos);
            esc_close(&ctx); continue;
        }

        /* Addresses are strapped per board (EVS-XCR-E 0,1; EVE-NET 1,3), so
         * discover rather than assume. Reads only the identifier, which has no
         * latched bits. */
        int found = 0;
        for (int a = 0; a < 32; a++) {
            uint16_t id1 = 0, id2 = 0;
            if (mii_read_phy(&ctx, (uint8_t)a, 0x02, &id1, NULL) != 0) continue;
            if (mii_read_phy(&ctx, (uint8_t)a, 0x03, &id2, NULL) != 0) continue;
            if (!phy_id_present(id1, id2)) continue;
            found++;
            fprintf(g_probes, "  pos %d phy %2d:", pos, a);
            for (int i = 0; i < NPROBE_REGS; i++) {
                uint16_t v = 0;
                if (mii_read_phy(&ctx, (uint8_t)a, probe_regs[i].reg, &v, NULL) != 0)
                    fprintf(g_probes, " %s=ERR", probe_regs[i].name);
                else
                    fprintf(g_probes, " %s=0x%04X", probe_regs[i].name, v);
            }
            fprintf(g_probes, "\n");
        }
        if (!found) fprintf(g_probes, "  pos %d: no PHY answered\n", pos);
        else probed++;
        esc_close(&ctx);
    }

    uint64_t fd = atomic_load_explicit(&g_fdrop, memory_order_relaxed);
    uint64_t ed = atomic_load_explicit(&g_edrop, memory_order_relaxed);
    if (fd || ed)
        fprintf(g_probes, "  [ring overflow: %lu frame(s), %lu event(s) dropped]\n",
                fd, ed);
    fflush(g_probes);
    return probed;
}
