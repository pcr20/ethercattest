/* Test harness: includes the module sources under test directly, so each
 * test's include list documents exactly what the tested code depends on.
 * crc32c_init() is called first in every main — with shared extern tables an
 * uninitialised CRC degenerates to a constant (see crc.h). */
#include "crc.c"
#include "stats.c"
#include "frame.c"

/* Verify Change 1 (FCS gating of ESC counter reads) and Option C (lost-link):
 * 1. good frame with counter values -> accumulates
 * 2. CORRUPT frame with garbage counters -> ignored entirely
 * 3. next good frame -> correct value, no pollution from step 2 */

static int walk_to_dg(uint8_t *buf,int len,int target){ /* returns data offset of Nth datagram (0-based) */
    int pos=16,n=0;
    while(pos+12<=len){
        uint16_t lf=le16get(buf+pos+6); uint16_t dl=lf&0x7FF; int more=(lf>>15)&1;
        if(n==target) return pos+10;
        pos+=10+dl+2; n++;
        if(!more) break;
    }
    return -1;
}
static void set_wkc(uint8_t *buf,int len,int dg,uint16_t wkc){
    int pos=16,n=0;
    while(pos+12<=len){
        uint16_t lf=le16get(buf+pos+6); uint16_t dl=lf&0x7FF; int more=(lf>>15)&1;
        if(n==dg){ le16put(buf+pos+10+dl,wkc); return; }
        pos+=10+dl+2; n++;
        if(!more) break;
    }
}

int main(void){
    crc32c_init();
    crc32c_init();
    int fails=0;
    uint8_t src[6]={1,2,3,4,5,6};
    uint8_t buf[MAX_FRAME]; 
    int slaves=2;
    memset((void*)&g_stats,0,sizeof(g_stats));
    int len=build_frame(buf,MAX_FRAME,src,slaves,7,0);
    /* datagrams: 0=NOP 1=BRD 2=diagAPRD(s0) 3=diagAPRD(s1).
     * The per-port CRC counters and the lost-link counters used to live in two
     * separate datagrams; they are now one ESC_DIAG_LEN block per slave, so
     * both offsets are inside the SAME datagram: 0x0300 at +0, 0x0310 at +16. */
    int crc_s0=walk_to_dg(buf,len,2);
    if(crc_s0<0){printf("FAIL: walk\n");return 1;}
    int ll_s0 = crc_s0 + 16;                 /* 0x0310 within the same block */
    /* BRD wkc = 2 (matches slaves) so no mismatch noise */
    set_wkc(buf,len,1,2);
    set_wkc(buf,len,2,1); set_wkc(buf,len,3,1);

    /* Step 0: the FIRST valid read baselines rather than accumulating (these
     * registers are cumulative in the slave and persist across sessions), so
     * establish a zero baseline before the steps below. */
    parse_return_frame(buf,len,slaves,0,/*fcs_ok=*/1,NULL);

    /* Step 1: good frame, s0 port1 crc=5 (reg 0x0302 = byte offset 2), s0 port1 lost=3 (0x0311 = byte 1) */
    buf[crc_s0+2]=5; buf[ll_s0+1]=3;
    parse_return_frame(buf,len,slaves,0,/*fcs_ok=*/1,NULL);
    if(g_stats.esc_crc[0][1]!=5||g_stats.esc_lostlnk[0][1]!=3){
        printf("FAIL T1: crc=%lu lost=%lu (exp 5,3)\n",g_stats.esc_crc[0][1],g_stats.esc_lostlnk[0][1]);fails++;}
    else printf("T1 PASS: good frame accumulates crc=5 lost-link=3\n");

    /* Step 2: CORRUPT frame (fcs_ok=0) with garbage counters — must be ignored */
    buf[crc_s0+2]=200; buf[ll_s0+1]=250;
    parse_return_frame(buf,len,slaves,0,/*fcs_ok=*/0,NULL);
    if(g_stats.esc_crc[0][1]!=5||g_stats.esc_lostlnk[0][1]!=3){
        printf("FAIL T2: corrupt frame polluted counters: crc=%lu lost=%lu\n",
               g_stats.esc_crc[0][1],g_stats.esc_lostlnk[0][1]);fails++;}
    else printf("T2 PASS: corrupt frame (garbage 200/250) fully ignored\n");

    /* Step 3: next good frame with true values (crc now 7, lost now 4) —
     * delta from the LAST GOOD prev (5,3), not from the garbage */
    buf[crc_s0+2]=7; buf[ll_s0+1]=4;
    parse_return_frame(buf,len,slaves,0,/*fcs_ok=*/1,NULL);
    if(g_stats.esc_crc[0][1]!=7||g_stats.esc_lostlnk[0][1]!=4){
        printf("FAIL T3: crc=%lu lost=%lu (exp 7,4)\n",g_stats.esc_crc[0][1],g_stats.esc_lostlnk[0][1]);fails++;}
    else printf("T3 PASS: next good frame lands exactly (7,4) — no pollution\n");

    /* Step 4: BRD WKC mismatch also gated: corrupt frame with wrong WKC must not count */
    uint64_t mm0=atomic_load(&g_stats.brd_wkc_mismatches);
    set_wkc(buf,len,1,1);   /* wrong WKC (1 != 2) */
    parse_return_frame(buf,len,slaves,0,/*fcs_ok=*/0,NULL);
    uint64_t mm1=atomic_load(&g_stats.brd_wkc_mismatches);
    parse_return_frame(buf,len,slaves,0,/*fcs_ok=*/1,NULL);
    uint64_t mm2=atomic_load(&g_stats.brd_wkc_mismatches);
    if(mm1!=mm0||mm2!=mm0+1){printf("FAIL T4: mm %lu->%lu->%lu\n",mm0,mm1,mm2);fails++;}
    else printf("T4 PASS: WKC mismatch gated (corrupt ignored, good counted)\n");

    printf("\n%s\n",fails?"*** ESC GATE FAILURES ***":"ALL ESC-GATE TESTS PASS");
    return fails;
}
