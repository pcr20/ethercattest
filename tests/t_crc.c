/* Test harness: includes the module sources under test directly, so each
 * test's include list documents exactly what the tested code depends on.
 * crc32c_init() is called first in every main — with shared extern tables an
 * uninitialised CRC degenerates to a constant (see crc.h). */
#include "crc.c"
#include "stats.c"
#include "frame.c"

int main(void){
    crc32c_init();
    printf("SSE4.2: %s\n", g_have_sse42 ? "yes" : "no");
    uint8_t mac[6]={0x84,0x47,9,0x82,0xbc,0x24};
    uint8_t buf[MAX_FRAME];
    int fails=0;

    /* Test 1: build + parse round trip, seq preserved, no CRC error */
    for (uint64_t s=0; s<1000; s++){
        int len = build_frame(buf,sizeof(buf),mac,0,s,1);
        atomic_store(&g_stats.payload_crc_errors,0);
        uint64_t got = parse_return_frame(buf,len,0,1,1,NULL);
        if (got != s) { printf("FAIL seq %lu->%lu\n",s,got); fails++; break; }
        if (atomic_load(&g_stats.payload_crc_errors)!=0){
            printf("FAIL: false CRC error on clean frame seq %lu\n",s); fails++; break; }
    }
    printf("Test 1 (clean round-trip, 1000 frames): %s\n", fails?"FAIL":"PASS");

    /* Test 2: corrupt one payload byte -> exactly one CRC error */
    {
        int len = build_frame(buf,sizeof(buf),mac,0,42,1);
        /* flip a byte in the random payload region (after 12-byte pl header,
         * which sits at ETH(14)+ECAT(2)+DG_HDR(10) = offset 26, +PL_DATA_OFF) */
        int pl_off = ETH_HDR_LEN+ECAT_HDR_LEN+ECAT_DG_HDR_LEN+PL_DATA_OFF+20;
        atomic_store(&g_stats.payload_crc_errors,0);
        buf[pl_off] ^= 0xFF;
        parse_return_frame(buf,len,0,1,1,NULL);
        uint64_t e = atomic_load(&g_stats.payload_crc_errors);
        printf("Test 2 (1 corrupted payload byte): crc_errors=%lu -> %s\n",
               e, e==1?"PASS":"FAIL");
        if(e!=1)fails++;
    }

    /* Test 3: corrupt the seq bytes -> CRC also catches it (CRC covers seq) */
    {
        int len = build_frame(buf,sizeof(buf),mac,0,99,1);
        int seq_off = ETH_HDR_LEN+ECAT_HDR_LEN+ECAT_DG_HDR_LEN+PL_SEQ_OFF;
        atomic_store(&g_stats.payload_crc_errors,0);
        buf[seq_off+3] ^= 0x01;   /* flip a bit in the seq */
        parse_return_frame(buf,len,0,1,1,NULL);
        uint64_t e = atomic_load(&g_stats.payload_crc_errors);
        printf("Test 3 (corrupted seq byte): crc_errors=%lu -> %s\n",
               e, e==1?"PASS":"FAIL");
        if(e!=1)fails++;
    }

    /* Test 4: payload is actually pseudo-random, not constant */
    {
        int len = build_frame(buf,sizeof(buf),mac,0,7,1);
        int d = ETH_HDR_LEN+ECAT_HDR_LEN+ECAT_DG_HDR_LEN+PL_DATA_OFF;
        int uniq=0; uint8_t seen[256]={0};
        for(int i=0;i<64;i++){ if(!seen[buf[d+i]]){seen[buf[d+i]]=1;uniq++;} }
        printf("Test 4 (payload entropy): %d distinct bytes in 64 -> %s\n",
               uniq, uniq>30?"PASS":"FAIL");
        if(uniq<=30)fails++;
        (void)len;
    }

    /* Test 5: same seq -> identical payload (deterministic) */
    {
        uint8_t b1[MAX_FRAME], b2[MAX_FRAME];
        int l1=build_frame(b1,sizeof(b1),mac,0,12345,1);
        int l2=build_frame(b2,sizeof(b2),mac,0,12345,1);
        int same = (l1==l2) && memcmp(b1,b2,l1)==0;
        printf("Test 5 (deterministic payload): %s\n", same?"PASS":"FAIL");
        if(!same)fails++;
    }

    printf("\n%s\n", fails?"*** FAILURES ***":"ALL CRC/PAYLOAD TESTS PASS");
    return fails?1:0;
}
