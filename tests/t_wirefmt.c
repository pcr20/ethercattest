/* Test harness: includes the module sources under test directly, so each
 * test's include list documents exactly what the tested code depends on.
 * crc32c_init() is called first in every main — with shared extern tables an
 * uninitialised CRC degenerates to a constant (see crc.h). */
#include "crc.c"
#include "stats.c"
#include "frame.c"

/* Byte-level wire-format verification against ETG.1000.4 — reading the frame
 * exactly as an ESC does (little-endian). This is the test that would have
 * caught the htons endianness bug that made real slaves cut every frame. */
static int fails=0;
#define CHECK(cond,fmt,...) do{ if(!(cond)){printf("FAIL: " fmt "\n",##__VA_ARGS__);fails++;} }while(0)

static void verify(int slaves,int lb){
    uint8_t src[6]={0x84,0x47,0x09,0x82,0xbc,0x24};
    uint8_t buf[MAX_FRAME+64]; memset(buf,0,sizeof(buf));
    int len=build_frame(buf,MAX_FRAME,src,slaves,0xDEADBEEFULL,lb);

    /* EtherType 0x88A4 big-endian (Ethernet field, correctly BE) */
    CHECK(buf[12]==0x88&&buf[13]==0xA4,"s=%d EtherType bytes %02x %02x",slaves,buf[12],buf[13]);

    /* EtherCAT frame header: 16-bit LE, bits 0-10 length, bits 12-15 type=1 */
    uint16_t eh = (uint16_t)(buf[14] | (buf[15]<<8));   /* read as ESC does: LE */
    uint16_t elen = eh & 0x07FF, etype = (eh>>12)&0xF;
    CHECK(etype==1,"s=%d lb=%d EtherCAT type=%u (must be 1) hdr=0x%04x",slaves,lb,etype,eh);
    CHECK(elen==(uint16_t)(len-16),"s=%d lb=%d EtherCAT len=%u expect %d",slaves,lb,elen,len-16);

    /* Walk datagrams as an ESC would */
    int pos=16, ndg=0, last_more=-1;
    while(pos+12<=len){
        uint8_t cmd=buf[pos];
        uint16_t lf=(uint16_t)(buf[pos+6]|(buf[pos+7]<<8));   /* LE */
        uint16_t dlen=lf&0x07FF; int more=(lf>>15)&1;
        pos += 10 + dlen + 2;   /* hdr + data + WKC */
        ndg++; last_more=more;
        if(ndg==1) CHECK(cmd==0x00,"s=%d first dg cmd=%u (NOP=0)",slaves,cmd);
        if(!more) break;
    }
    CHECK(pos==len,"s=%d lb=%d datagram walk ends at %d, frame len %d (ESC would cut here)",slaves,lb,pos,len);
    int expect_ndg = lb? 1 : (slaves? 1+1+2*slaves : 1);
    CHECK(ndg==expect_ndg,"s=%d lb=%d datagrams walked=%d expect %d",slaves,lb,ndg,expect_ndg);
    CHECK(last_more==0,"s=%d lb=%d last datagram has more=1 (chain never terminates)",slaves,lb);
    if(!fails) printf("PASS: s=%d lb=%d — hdr LE type=1 len=%u, %d datagrams walk to exactly frame end\n",slaves,lb,elen,ndg);
}

int main(void){
    crc32c_init();
    crc32c_init();
    verify(0,1);            /* loopback */
    for(int s=1;s<=4;s++) verify(s,0);
    printf("\n%s\n",fails?"*** WIRE FORMAT FAILURES ***":"ALL WIRE-FORMAT TESTS PASS");
    return fails;
}
