/* Test harness: includes the module sources under test directly, so each
 * test's include list documents exactly what the tested code depends on.
 * crc32c_init() is called first in every main — with shared extern tables an
 * uninitialised CRC degenerates to a constant (see crc.h). */
#include "crc.c"
#include "stats.c"
#include "frame.c"

/* New payload-CRC semantics: payload_crc_errors counts EVERY received frame
 * without a valid payload CRC — invalid OR missing (cut/unparseable).
 * Plus the Rx frame-length error counter. */
static uint64_t plerr(void){ return atomic_load(&g_stats.payload_crc_errors); }

int main(void){
    crc32c_init();
    crc32c_init();
    int fails=0;
    uint8_t src[6]={1,2,3,4,5,6};
    uint8_t buf[MAX_FRAME];
    memset((void*)&g_stats,0,sizeof(g_stats));
    int flen=build_frame(buf,MAX_FRAME,src,4,42,0);   /* -s 4 frame, 1514 */

    /* T1: intact frame -> payload_ok=1, no error */
    int pok=0; uint64_t e0=plerr();
    parse_return_frame(buf,flen,4,0,1,&pok);
    if(!pok||plerr()!=e0){printf("T1 FAIL pok=%d err=%lu\n",pok,plerr()-e0);fails++;}
    else printf("T1 PASS: intact frame -> valid payload CRC, no increment\n");

    /* T2: payload bit flip -> increment */
    e0=plerr(); buf[100]^=0x01;
    parse_return_frame(buf,flen,4,0,0,&pok);
    buf[100]^=0x01;
    if(plerr()!=e0+1){printf("T2 FAIL err=%lu\n",plerr()-e0);fails++;}
    else printf("T2 PASS: corrupted payload -> increment\n");

    /* T3: CUT frames (the previously-invisible population) -> increment.
       Cut at various lengths incl. below gates A (10), B (20), C (300, 1030). */
    int cuts[]={10,20,300,1030};
    for(int i=0;i<4;i++){
        e0=plerr();
        parse_return_frame(buf,cuts[i],4,0,0,&pok);
        if(plerr()!=e0+1){printf("T3 FAIL cut=%d err=%lu (exp 1)\n",cuts[i],plerr()-e0);fails++;}
        else printf("T3 PASS: frame cut at %4d bytes -> payload CRC missing -> increment\n",cuts[i]);
    }

    /* T4: population completeness — every received frame is exactly one of
       {valid payload CRC} or {counted}. 100 frames: 60 good, 25 cut, 15 flipped. */
    memset((void*)&g_stats,0,sizeof(g_stats));
    int good=0;
    for(int i=0;i<100;i++){
        build_frame(buf,MAX_FRAME,src,4,1000+i,0);
        int ok=0;
        if(i<60) parse_return_frame(buf,flen,4,0,1,&ok);
        else if(i<85) parse_return_frame(buf,200+i,4,0,0,&ok);   /* cut */
        else { buf[500]^=0xFF; parse_return_frame(buf,flen,4,0,0,&ok); }
        if(ok) good++;
    }
    if(good!=60||plerr()!=40){printf("T4 FAIL good=%d err=%lu (exp 60/40)\n",good,plerr());fails++;}
    else printf("T4 PASS: 100 frames -> 60 valid + 40 counted = complete partition\n");

    /* T5: Rx length check. Publish expected size, then test. */
    memset((void*)&g_stats,0,sizeof(g_stats));
    atomic_store(&g_wire_bits_per_frame,(uint64_t)(flen+4)*8);   /* 1518*8 */
    int r;
    r=rx_check_len(flen+4,1);   /* rx-fcs on: raw includes FCS -> 1518 = correct */
    if(r||atomic_load(&g_stats.rx_len_errors)!=0){printf("T5a FAIL\n");fails++;}
    else printf("T5a PASS: correct length (1518, rx-fcs on) -> no error\n");
    r=rx_check_len(1030,1);     /* cut frame */
    if(!r||atomic_load(&g_stats.rx_len_errors)!=1){printf("T5b FAIL\n");fails++;}
    else printf("T5b PASS: cut frame (1030) -> length error counted\n");
    r=rx_check_len(flen,0);     /* rx-fcs off: raw excludes FCS -> 1514 = correct */
    if(r){printf("T5c FAIL\n");fails++;}
    else printf("T5c PASS: correct length (1514, rx-fcs off) -> no error\n");
    atomic_store(&g_wire_bits_per_frame,0);
    r=rx_check_len(999,1);      /* expected unknown -> no count */
    if(r||atomic_load(&g_stats.rx_len_errors)!=1){printf("T5d FAIL\n");fails++;}
    else printf("T5d PASS: expected size unknown -> check inert\n");

    printf("\n%s\n",fails?"*** PAYLOAD-SEMANTICS FAILURES ***":"ALL PAYLOAD-SEMANTICS TESTS PASS");
    return fails;
}
