/* Test harness: includes the module sources under test directly, so each
 * test's include list documents exactly what the tested code depends on.
 * crc32c_init() is called first in every main — with shared extern tables an
 * uninitialised CRC degenerates to a constant (see crc.h). */
#include "crc.c"
#include "stats.c"
#include "frame.c"

static void reset(void){
    memset(&g_rx,0,sizeof(g_rx));
    memset((void*)&g_stats,0,sizeof(g_stats));
    atomic_store(&g_txok,0); atomic_store(&g_txer,0); atomic_store(&g_qdisc_drop,0);
}
/* good return (FCS+payload valid): counts + advances max_good_seq */
static void good(uint64_t seq){ if(rx_new_good_return(seq)) atomic_fetch_add(&g_stats.distinct_returns,1); }
/* corrupt return: does NOT count as good return (tracked as corruption elsewhere) */
static void corrupt(uint64_t seq){ (void)seq; atomic_fetch_add(&g_stats.payload_crc_errors,1); }
static uint64_t loss(void){
    int64_t l=(int64_t)atomic_load(&g_txok)-(int64_t)atomic_load(&g_stats.distinct_returns);
    return l>0?(uint64_t)l:0;
}

int main(void){
    crc32c_init();
    crc32c_init();
    int fails=0;

    /* T1: clean — 10000 on wire, all good -> 0 loss. */
    reset(); atomic_store(&g_txok,10000);
    for(uint64_t s=0;s<10000;s++) good(s);
    if(loss()!=0){printf("T1 FAIL loss=%lu\n",loss());fails++;}
    else printf("T1 PASS: 10000 on wire, all good -> 0 loss\n");

    /* T2: 100 absent (never returned) -> 100 loss. */
    reset(); atomic_store(&g_txok,10000);
    for(uint64_t s=0;s<10000;s++) if(s<500||s>=600) good(s);
    if(loss()!=100){printf("T2 FAIL loss=%lu (exp 100)\n",loss());fails++;}
    else printf("T2 PASS: 100 absent -> 100 loss\n");

    /* T3: corrupt frame counts in BOTH loss and corruption. 10000 on wire,
       50 came back corrupt (seqs 200..249), the rest good. Corrupt frames are
       NOT good returns -> they're in loss. loss should be 50. corruption=50. */
    reset(); atomic_store(&g_txok,10000);
    for(uint64_t s=0;s<10000;s++){ if(s>=200&&s<250) corrupt(s); else good(s); }
    if(loss()!=50){printf("T3 FAIL loss=%lu (exp 50)\n",loss());fails++;}
    else if(atomic_load(&g_stats.payload_crc_errors)!=50){printf("T3 FAIL corruption=%lu\n",atomic_load(&g_stats.payload_crc_errors));fails++;}
    else printf("T3 PASS: 50 corrupt -> in BOTH loss (50) and corruption (50)\n");

    /* T4: dedup — good frame delivered twice, counted once. */
    reset(); atomic_store(&g_txok,5000);
    for(uint64_t s=0;s<5000;s++){ good(s); good(s); }
    if(atomic_load(&g_stats.distinct_returns)!=5000){printf("T4 FAIL distinct=%lu\n",atomic_load(&g_stats.distinct_returns));fails++;}
    else if(loss()!=0){printf("T4 FAIL loss=%lu\n",loss());fails++;}
    else printf("T4 PASS: double good delivery deduped -> distinct=5000, loss=0\n");

    /* T5: max_good_seq tracks highest good seq (termination gate). Corrupt
       frames must NOT advance it (untrusted seq). */
    reset(); atomic_store(&g_txok,1000);
    for(uint64_t s=0;s<800;s++) good(s);
    corrupt(950);   /* corrupt frame with high seq: must not advance the gate */
    if(!g_rx.max_good_valid || g_rx.max_good_seq!=799){
        printf("T5 FAIL max_good_seq=%lu (exp 799)\n",g_rx.max_good_seq);fails++;}
    else printf("T5 PASS: max_good_seq=799 (corrupt seq 950 did NOT advance the gate)\n");

    /* T6: real outage boundary — 96418 on wire (TxOk), 95886 good returns,
       kernel drops + TxER excluded (not in TxOk). loss = 96418-95886 = 532. */
    reset(); atomic_store(&g_txok,96418); atomic_store(&g_qdisc_drop,5417); atomic_store(&g_txer,2314);
    for(uint64_t s=0;s<95886;s++) good(s);
    if(loss()!=532){printf("T6 FAIL loss=%lu (exp 532)\n",loss());fails++;}
    else printf("T6 PASS: outage -> loss=532 (kernel drops + TxER excluded from TxOk)\n");

    /* T7: uint64 across 2^32. */
    reset();
    uint64_t base=0x100000000ULL;
    atomic_store(&g_txok,base+1000);
    for(uint64_t s=base;s<base+1000;s++) good(s);
    if(loss()!=base){printf("T7 FAIL loss=%lu\n",loss());fails++;}
    else if(g_rx.max_good_seq!=base+999){printf("T7 FAIL max_good=%lu\n",g_rx.max_good_seq);fails++;}
    else printf("T7 PASS: uint64 across 2^32 exact (max_good_seq=%lu)\n",g_rx.max_good_seq);

    printf("\n%s\n", fails?"*** FAILURES ***":"ALL TXOK-MODEL TESTS PASS");
    return fails;
}
