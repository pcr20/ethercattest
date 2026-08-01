/* Test harness: includes the module sources under test directly, so each
 * test's include list documents exactly what the tested code depends on.
 * crc32c_init() is called first in every main — with shared extern tables an
 * uninitialised CRC degenerates to a constant (see crc.h). */
#include "crc.c"
#include "stats.c"
#include "frame.c"
static int is_bad(const uint8_t*b,int raw){
    uint32_t r=eth_crc32(b,raw);
    return (r!=ETH_FCS_RESIDUAL && r!=ETH_FCS_RESIDUAL2);
}
int main(void){
    crc32c_init();
    uint8_t mac[6]={0x84,0x47,9,0x82,0xbc,0x24};
    uint8_t buf[MAX_FRAME+8];
    int fails=0;
    /* 100 good frames, various lengths -> all must pass */
    for(int t=0;t<100;t++){
        int len=build_frame(buf,MAX_FRAME-(t*13%300),mac,0,t,1);
        uint32_t fcs=eth_crc32(buf,len); memcpy(buf+len,&fcs,4);
        if(is_bad(buf,len+4)){printf("FAIL: good frame t=%d flagged bad\n",t);fails++;break;}
    }
    printf("100 good frames all pass: %s\n", fails?"FAIL":"PASS");
    /* corrupt each of 100 frames in a random byte -> all must be caught */
    int missed=0;
    for(int t=0;t<100;t++){
        int len=build_frame(buf,MAX_FRAME-(t*13%300),mac,0,t,1);
        uint32_t fcs=eth_crc32(buf,len); memcpy(buf+len,&fcs,4);
        buf[20+(t%(len-20))] ^= (1<<(t%8));
        if(!is_bad(buf,len+4)) missed++;
    }
    printf("100 corrupted frames all detected: %s (missed=%d)\n",
           missed==0?"PASS":"FAIL", missed);
    return fails||missed;
}
