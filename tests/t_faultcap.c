/* Fault capture: ring conservation under overflow, pcap file format, and the
 * burst-settled trigger that decides when it is safe to pause and probe. */
#define FAULTCAP_REAL 1   /* see the guard in frame.c */
#include "crc.c"
#include "stats.c"
#include "frame.c"
#include "nic.c"
#include "escreg.c"
#include "escmii.c"
#include "faultcap.c"

static int fails = 0;
#define CHECK(c, ...) do { if(!(c)) { printf("FAIL: "); printf(__VA_ARGS__); \
                                      printf("\n"); fails++; } } while (0)

int main(void)
{
    crc32c_init();                     /* §4.3 invariant: always first */
    char dir[] = "/tmp/faultcapXXXXXX";
    CHECK(mkdtemp(dir) != NULL, "tmpdir");

    /* ── T1: the capture set opens and the pcap header is well formed ───── */
    {
        int f0 = fails;
        CHECK(faultcap_open("lo", 2, dir) == 0, "faultcap_open failed");
        faultcap_flush();
        char p[512]; snprintf(p, sizeof(p), "%s/frames.pcap", dir);
        FILE *f = fopen(p, "rb"); CHECK(f != NULL, "pcap not created");
        uint32_t magic = 0; uint16_t vmaj = 0, vmin = 0; uint32_t snap = 0, link = 0;
        if (f) {
            fread(&magic,4,1,f); fread(&vmaj,2,1,f); fread(&vmin,2,1,f);
            fseek(f, 16, SEEK_SET); fread(&snap,4,1,f); fread(&link,4,1,f); fclose(f);
        }
        CHECK(magic == 0xa1b2c3d4u, "pcap magic 0x%08X, want 0xa1b2c3d4", magic);
        CHECK(vmaj == 2 && vmin == 4, "pcap version %u.%u, want 2.4", vmaj, vmin);
        CHECK(link == 1, "link type %u, want 1 (Ethernet)", link);
        CHECK(snap == 65535, "snaplen %u", snap);
        if (fails == f0) printf("T1 PASS: pcap header valid (magic, v2.4, "
                                "LINKTYPE_ETHERNET)\n");
    }

    /* ── T2: a captured frame round-trips byte-exactly through the pcap ── */
    {
        int f0 = fails;
        uint8_t tx[MAX_FRAME];
        int len = build_frame(tx, sizeof(tx), (const uint8_t[]){1,2,3,4,5,6},
                              2, 12345, 0);
        CHECK(len > 0, "build_frame");
        faultcap_frame(1500000000ULL, FAULTCAP_REASON_FCS, tx, len);
        faultcap_flush();

        char p[512]; snprintf(p, sizeof(p), "%s/frames.pcap", dir);
        FILE *f = fopen(p, "rb");
        CHECK(f != NULL, "pcap reopen");
        if (f) {
            fseek(f, 24, SEEK_SET);            /* past the global header */
            uint32_t ph[4] = {0};
            fread(ph, 4, 4, f);
            CHECK((int)ph[2] == len, "caplen %u, want %d", ph[2], len);
            CHECK((int)ph[3] == len, "origlen %u, want %d", ph[3], len);
            CHECK(ph[0] == 1, "timestamp seconds %u, want 1", ph[0]);
            uint8_t back[MAX_FRAME];
            size_t got = fread(back, 1, (size_t)len, f);
            CHECK((int)got == len, "short read of frame bytes");
            CHECK(memcmp(back, tx, (size_t)len) == 0,
                  "frame bytes did not round-trip unchanged");
            fclose(f);
        }
        if (fails == f0)
            printf("T2 PASS: frame bytes round-trip byte-exactly through pcap\n");
    }

    /* ── T3: ring overflow drops and counts, never blocks or corrupts ──── */
    {
        int f0 = fails;
        uint64_t before = atomic_load(&g_fdrop);
        uint8_t junk[128]; memset(junk, 0xAB, sizeof(junk));
        for (int i = 0; i < FRAME_RING * 3; i++)      /* 3x capacity, undrained */
            faultcap_frame(2000000000ULL + i, FAULTCAP_REASON_LENGTH,
                           junk, (int)sizeof(junk));
        uint64_t dropped = atomic_load(&g_fdrop) - before;
        CHECK(dropped == (uint64_t)(FRAME_RING * 3 - FRAME_RING),
              "expected %d drops, got %lu", FRAME_RING * 2, dropped);
        uint64_t stored = atomic_load(&g_fhead) - atomic_load(&g_ftail);
        CHECK(stored == FRAME_RING, "ring should hold exactly %d, holds %lu",
              FRAME_RING, stored);
        faultcap_flush();
        if (fails == f0)
            printf("T3 PASS: overflow drops and counts (%lu dropped), never "
                   "blocks the RX thread\n", dropped);
    }

    /* ── T4: burst-settled trigger ───────────────────────────────────────
     * Probing pauses TX, so firing on the first error of a burst would
     * suppress the remainder and bias the sample. The trigger must stay low
     * while events keep arriving and only rise after a quiet period. */
    {
        int f0 = fails;
        const uint64_t QUIET = 200ULL * 1000000ULL;    /* 200 ms */
        uint64_t t = 5000000000ULL;
        CHECK(faultcap_burst_settled(t, QUIET) == 0 ||
              faultcap_event_count() > 0, "no events yet -> not settled");

        faultcap_esc_event(t, 5, 1, "invalid", 1);
        CHECK(faultcap_burst_settled(t + 10000000ULL, QUIET) == 0,
              "10 ms after an event the burst is NOT settled");
        faultcap_esc_event(t + 50000000ULL, 5, 1, "invalid", 1);   /* +50 ms */
        CHECK(faultcap_burst_settled(t + 150000000ULL, QUIET) == 0,
              "a second event must restart the quiet window");
        CHECK(faultcap_burst_settled(t + 50000000ULL + QUIET, QUIET) == 1,
              "settled once quiet_ns has elapsed since the LAST event");
        CHECK(faultcap_event_count() >= 2, "both events counted");
        if (fails == f0)
            printf("T4 PASS: trigger waits for the burst to drain, and each "
                   "new event restarts the quiet window\n");
    }

    faultcap_close();
    if (fails) { printf("\n%d FAULTCAP CHECK(S) FAILED\n", fails); return 1; }
    printf("\nALL FAULTCAP TESTS PASS\n");
    return 0;
}
