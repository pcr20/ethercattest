/* Test harness: includes the module sources under test directly, so each
 * test's include list documents exactly what the tested code depends on.
 * crc32c_init() is called first in every main — with shared extern tables an
 * uninitialised CRC degenerates to a constant (see crc.h). */
#include "crc.c"
#include "stats.c"
#include "frame.c"
#include <pthread.h>

/* Bad-FCS SPSC ring: order, overflow drop-and-count (never block), and
 * conservation under a real producer/consumer thread race. */
static uint64_t drained=0, budget_used=0;
static void drain_count(int *budget, uint64_t *supp){
    /* consumer identical to badfcs_drain but counting instead of printing */
    uint64_t h=atomic_load_explicit(&g_bfe_head,memory_order_acquire);
    uint64_t t=atomic_load_explicit(&g_bfe_tail,memory_order_relaxed);
    while(t<h){ BadFcsEv ev=g_bfe[t&(BADFCS_RING-1)]; (void)ev;
        if(*budget>0){(*budget)--;budget_used++;} else (*supp)++;
        drained++; t++; }
    atomic_store_explicit(&g_bfe_tail,t,memory_order_release);
}
static void reset(void){atomic_store(&g_bfe_head,0);atomic_store(&g_bfe_tail,0);
    atomic_store(&g_bfe_ringdrop,0);drained=0;budget_used=0;}

static void *producer(void *arg){
    uint64_t n=*(uint64_t*)arg;
    for(uint64_t i=0;i<n;i++) badfcs_push((uint32_t)i,(int)(i&0x7FF));
    return NULL;
}

int main(void){
    crc32c_init();
    int fails=0;

    /* T1: order + exact count under capacity */
    reset();
    for(int i=0;i<100;i++) badfcs_push(i,1518);
    /* verify FIFO order via tail walk */
    uint64_t h=atomic_load(&g_bfe_head);
    int order_ok=1;
    for(uint64_t t=0;t<h;t++) if(g_bfe[t&(BADFCS_RING-1)].tp!=(uint32_t)t) order_ok=0;
    int b=1000; uint64_t supp=0; drain_count(&b,&supp);
    if(!order_ok||drained!=100||supp!=0){printf("T1 FAIL\n");fails++;}
    else printf("T1 PASS: 100 pushed -> 100 drained, FIFO order, none suppressed\n");

    /* T2: overflow — push 5000 into a 4096 ring, no consumer: exactly 4096
       land, 904 dropped-and-counted, producer never blocks. */
    reset();
    for(int i=0;i<5000;i++) badfcs_push(i,60);
    uint64_t rd=atomic_load(&g_bfe_ringdrop);
    b=1000000; supp=0; drain_count(&b,&supp);
    if(drained!=BADFCS_RING||rd!=5000-BADFCS_RING){
        printf("T2 FAIL drained=%lu rd=%lu\n",drained,rd);fails++;}
    else printf("T2 PASS: 5000 into full ring -> %d stored + %lu dropped-and-counted, no block\n",BADFCS_RING,rd);

    /* T3: print budget — 3000 events, cap 1000: 1000 printed, 2000 suppressed */
    reset();
    b=1000; supp=0;
    for(int i=0;i<3000;i++){ badfcs_push(i,60); if(i%500==499) drain_count(&b,&supp); }
    drain_count(&b,&supp);
    if(budget_used!=1000||supp!=2000){printf("T3 FAIL used=%lu supp=%lu\n",budget_used,supp);fails++;}
    else printf("T3 PASS: 3000 events, cap 1000 -> 1000 printed, 2000 suppressed\n");

    /* T4: threaded conservation — producer hammers 2,000,000 pushes while the
       consumer drains concurrently; drained + ringdrop must equal pushed. */
    reset();
    uint64_t n=2000000;
    pthread_t pt; pthread_create(&pt,NULL,producer,&n);
    b=1<<30; supp=0;
    for(;;){ drain_count(&b,&supp);
        if(atomic_load(&g_bfe_head)==atomic_load(&g_bfe_tail)){
            /* producer maybe done? try join with a peek */
            if(drained+atomic_load(&g_bfe_ringdrop)>=n) break;
        }
    }
    pthread_join(pt,NULL);
    drain_count(&b,&supp);
    uint64_t total=drained+atomic_load(&g_bfe_ringdrop);
    if(total!=n){printf("T4 FAIL conservation %lu != %lu\n",total,n);fails++;}
    else printf("T4 PASS: threaded race, %lu pushed = %lu drained + %lu dropped (conserved)\n",
                n,drained,atomic_load(&g_bfe_ringdrop));

    printf("\n%s\n",fails?"*** RING FAILURES ***":"ALL RING TESTS PASS");
    return fails;
}
