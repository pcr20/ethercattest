/* Test harness: includes the module sources under test directly, so each
 * test's include list documents exactly what the tested code depends on.
 * crc32c_init() is called first in every main — with shared extern tables an
 * uninitialised CRC degenerates to a constant (see crc.h). */
#include "crc.c"
#include "stats.c"
#include "frame.c"

/* Verify build_frame never writes past MAX_FRAME for any slave count.
 * Method: heap buffer of MAX_FRAME + 64 canary bytes (0xAA). Build with buflen
 * = MAX_FRAME, then check (a) returned length <= MAX_FRAME, (b) canary intact.
 * Before the fix, -s N overwrote 8*N canary bytes (FORTIFY abort on stack). */
int main(void){
    crc32c_init();
    crc32c_init();
    int fails=0;
    uint8_t src[6]={0x84,0x47,0x09,0x82,0xbc,0x24};
    for(int slaves=0;slaves<=8;slaves++){
        for(int lb=0;lb<=1;lb++){
            if(lb && slaves) continue;   /* loopback ignores slaves */
            uint8_t *buf=malloc(MAX_FRAME+64);
            memset(buf,0,MAX_FRAME); memset(buf+MAX_FRAME,0xAA,64);
            int len=build_frame(buf,MAX_FRAME,src,slaves,12345,lb);
            int canary_ok=1;
            for(int i=0;i<64;i++) if(buf[MAX_FRAME+i]!=0xAA){canary_ok=0;break;}
            if(len>MAX_FRAME||!canary_ok){
                printf("FAIL: slaves=%d lb=%d len=%d canary=%s\n",
                       slaves,lb,len,canary_ok?"ok":"OVERWRITTEN");fails++;
            } else {
                printf("PASS: slaves=%d lb=%d len=%d (<=%d), canary intact\n",
                       slaves,lb,len,MAX_FRAME);
            }
            free(buf);
        }
    }
    printf("\n%s\n",fails?"*** FRAME SIZE FAILURES ***":"ALL FRAME-SIZE TESTS PASS");
    return fails;
}
