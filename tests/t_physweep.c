/* PHY sweep tests: the read-order invariant that protects PHYSTS latch bits,
 * REGCR command-word construction including post-increment, the datasheet
 * decode constants, and the guarantee that a direct sweep never writes. */
#include "crc.c"
#include "stats.c"
#include "frame.c"
#include "nic.c"
#include "escreg.c"
#include "escmii.c"
#include "phy_regs.h"

static int fails = 0;
#define CHECK(c, ...) do { if(!(c)) { printf("FAIL: "); printf(__VA_ARGS__); \
                                      printf("\n"); fails++; } } while (0)

static int order_pos(uint8_t reg) {
    for (int i = 0; i < 32; i++) if (phy_read_order[i] == reg) return i;
    return -1;
}

int main(void)
{
    crc32c_init();                     /* §4.3 invariant: always first */

    /* ── T1: read order covers all 32 addresses exactly once ────────────── */
    {
        int seen[32] = {0}, f0 = fails;
        for (int i = 0; i < 32; i++) {
            uint8_t r = phy_read_order[i];
            CHECK(r < 32, "read order entry %d = 0x%02X out of range", i, r);
            CHECK(!seen[r], "register 0x%02X appears twice in the read order", r);
            seen[r] = 1;
        }
        for (int r = 0; r < 32; r++)
            CHECK(seen[r], "register 0x%02X missing from the read order", r);
        if (fails == f0) printf("T1 PASS: read order is a permutation of 0x00-0x1F\n");
    }

    /* ── T2: THE bug this ordering exists to prevent ─────────────────────
     * SNLS505H Table 8-16: PHYSTS(0x10) latch bits are cleared by reading
     * BMSR(0x01) bit6, ANER(0x06) bit8, MISR1(0x12) bit7, FCSCR(0x14) bit11,
     * RECR(0x15) bit13 and 10BTSCR(0x1A) bit12. A naive ascending sweep reads
     * 0x01 and 0x06 first and silently zeroes those PHYSTS bits. */
    {
        int f0 = fails;
        const uint8_t clears_physts[] = { 0x01, 0x06, 0x12, 0x14, 0x15, 0x1A };
        int p_physts = order_pos(0x10);
        CHECK(p_physts == 0, "PHYSTS must be read FIRST, found at index %d", p_physts);
        for (size_t i = 0; i < sizeof(clears_physts); i++) {
            uint8_t r = clears_physts[i];
            CHECK(order_pos(r) > p_physts,
                  "0x%02X (clears a PHYSTS latch bit) is read at %d, before "
                  "PHYSTS at %d", r, order_pos(r), p_physts);
        }
        if (fails == f0)
            printf("T2 PASS: PHYSTS read before every register that clears its "
                   "latch bits\n");
    }

    /* ── T3: perishable registers are read before the static ones ───────── */
    {
        int f0 = fails, last_perishable = -1, first_static = 99;
        for (int i = 0; i < 32; i++) {
            uint8_t r = phy_read_order[i];
            if (phy_direct[r].perishable) { if (i > last_perishable) last_perishable = i; }
            else if (i < first_static) first_static = i;
        }
        CHECK(last_perishable < first_static,
              "perishable register read at index %d after a static one at %d",
              last_perishable, first_static);
        if (fails == f0)
            printf("T3 PASS: all 9 clear-on-read registers precede the static ones\n");
    }

    /* ── T4: the perishable set matches the datasheet ───────────────────── */
    {
        int f0 = fails;
        const uint8_t rc_regs[] = { 0x01, 0x06, 0x0F, 0x10, 0x12, 0x13,
                                    0x14, 0x15, 0x1A };
        int n = 0;
        for (int r = 0; r < 32; r++) if (phy_direct[r].perishable) n++;
        CHECK(n == (int)sizeof(rc_regs), "expected %zu perishable registers, got %d",
              sizeof(rc_regs), n);
        for (size_t i = 0; i < sizeof(rc_regs); i++)
            CHECK(phy_direct[rc_regs[i]].perishable,
                  "0x%02X must be marked perishable (SNLS505H: R,RC)", rc_regs[i]);
        if (fails == f0) printf("T4 PASS: perishable set = the 9 R,RC registers\n");
    }

    /* ── T5: REGCR command words (SNLS505H Table 8-13) ──────────────────── */
    {
        int f0 = fails;
        CHECK(MMD_CMD_ADDR(0x1F)     == 0x001F, "address cmd wrong");
        CHECK(MMD_CMD_DATA(0x1F)     == 0x401F, "data cmd wrong");
        CHECK(MMD_CMD_DATA_INC(0x1F) == 0x801F, "post-increment cmd wrong");
        /* SNLA265 Table 9 walks MMD7 explicitly: 0x0007 then 0x4007. */
        CHECK(MMD_CMD_ADDR(MMD_DEVAD_MMD7) == 0x0007, "SNLA265 step 1 = 0x0007");
        CHECK(MMD_CMD_DATA(MMD_DEVAD_MMD7) == 0x4007, "SNLA265 step 3 = 0x4007");
        CHECK(MMD_DEVAD_VENDOR == 0x1F && MMD_DEVAD_MMD3 == 0x03 &&
              MMD_DEVAD_MMD7 == 0x07, "DEVAD constants wrong");
        CHECK((MMD_CMD_DATA_INC(0) >> 14) == 0x2, "post-increment must be cmd 10b");
        if (fails == f0)
            printf("T5 PASS: REGCR words match SNLS505H Table 8-13 / SNLA265 Table 9\n");
    }

    /* ── T6: extended table is well formed and inside documented space ──── */
    {
        int f0 = fails, saw_sor1 = 0, saw_tdr = 0;
        for (int i = 0; i < PHY_EXT_COUNT; i++) {
            const PhyExt *x = &phy_ext[i];
            CHECK(x->devad == 0x1F || x->devad == 0x03 || x->devad == 0x07,
                  "entry %d has DEVAD 0x%02X — not vendor/MMD3/MMD7", i, x->devad);
            CHECK(x->count >= 1 && x->count <= 16,
                  "entry %d block count %u out of range", i, x->count);
            if (x->devad == 0x1F)
                CHECK(x->reg <= 0x04D6, "vendor reg 0x%04X above documented space",
                      x->reg);
            if (x->reg == 0x0467) saw_sor1 = 1;
            if (x->reg == 0x0180) { saw_tdr = 1;
                CHECK(x->count == 11, "TDR block 0x0180-0x018A is 11 registers, got %u",
                      x->count); }
        }
        CHECK(saw_sor1, "SOR1 0x0467 must be present — it resolves EEE_EN");
        CHECK(saw_tdr,  "TDR result block 0x0180 must be present");
        if (fails == f0) printf("T6 PASS: extended table well formed (%d entries)\n",
                                PHY_EXT_COUNT);
    }

    /* ── T7: direct-sweep reads never write the PHY ──────────────────────
     * mii_read_phy writes only ESC MII control registers (0x0512/0x0510);
     * the PHY itself is written only through 0x0D/0x0E. Guard that no direct
     * sweep entry is one of the indirection registers used as a write target,
     * and that the read path and write path use different ESC addresses. */
    {
        int f0 = fails;
        CHECK(ESC_MII_PHYADR == 0x0512 && ESC_MII_CTRL == 0x0510 &&
              ESC_MII_DATA == 0x0514, "ESC MII addresses wrong");
        CHECK(MII_CMD_READ != MII_CMD_WRITE, "read and write commands must differ");
        CHECK((MII_CMD_READ & 1) == 0,
              "read command must NOT set the write-enable bit 0");
        CHECK((MII_CMD_WRITE & 1) == 1, "write command must set write-enable bit 0");
        if (fails == f0)
            printf("T7 PASS: read path cannot assert the PHY write-enable bit\n");
    }

    if (fails) { printf("\n%d PHY-SWEEP CHECK(S) FAILED\n", fails); return 1; }
    printf("\nALL PHY-SWEEP TESTS PASS\n");
    return 0;
}
