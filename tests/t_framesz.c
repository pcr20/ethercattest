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
    /* -b: frame_min_bytes() must agree with build_frame for every chain, and
     * build_frame must honour a buflen at (and just above) that minimum. One
     * byte below, build_frame would force the payload up to PL_HDR_LEN and
     * write past buflen -- which is why main.c rejects it rather than trying. */
    {
        int f0=fails;
        for(int slaves=0;slaves<=8;slaves++){
            for(int lb=0;lb<=1;lb++){
                if(lb && slaves) continue;
                int fmin=frame_min_bytes(slaves,lb);
                if(fmin<ETH_MIN_FRAME){
                    printf("FAIL: min %d below Ethernet minimum %d (s=%d lb=%d)\n",
                           fmin,ETH_MIN_FRAME,slaves,lb); fails++; continue;
                }
                for(int b=fmin;b<=fmin+2 && b<=MAX_FRAME;b++){
                    uint8_t *buf=malloc(MAX_FRAME+64);
                    memset(buf+b,0xAA,MAX_FRAME+64-b);
                    int len=build_frame(buf,b,src,lb?0:slaves,1234,lb);
                    if(len>b){ printf("FAIL: build_frame(buflen=%d) returned %d "
                                      "(s=%d lb=%d)\n",b,len,slaves,lb); fails++; }
                    for(int i=b;i<MAX_FRAME+64;i++)
                        if(buf[i]!=0xAA){ printf("FAIL: wrote past buflen=%d at +%d "
                                                 "(s=%d lb=%d)\n",b,i-b,slaves,lb);
                                          fails++; break; }
                    free(buf);
                }
            }
        }
        /* The minimum must actually track the slave count, not be a constant. */
        if(!(frame_min_bytes(9,0) > frame_min_bytes(4,0))){
            printf("FAIL: minimum does not grow with slave count\n"); fails++; }
        if(fails==f0) printf("frame_min_bytes agrees with build_frame for 0-8 "
                             "slaves; -b at the minimum is safe\n");
    }

    printf("\n%s\n",fails?"*** FRAME SIZE FAILURES ***":"ALL FRAME-SIZE TESTS PASS");
    return fails;
}
