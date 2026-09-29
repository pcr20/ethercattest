/* soem_check — an independent master, for contrast rather than fidelity.
 *
 * ecat_op replays TwinCAT's traffic byte for byte, and across 4,549 s of
 * running it has never seen the link drop that TwinCAT sees every 8.5 s. That
 * is a strong negative, but it is a negative from ONE master implementation,
 * written by us, checked against a capture we also analysed ourselves. A
 * shared blind spot would look exactly like this.
 *
 * So this deliberately does NOT imitate TwinCAT. It is SOEM driving the same
 * drives its own way:
 *
 *   - SOEM's own PDO mapping, read from each slave's object dictionary,
 *     rather than the 8 SDO downloads we captured
 *   - SOEM's own cyclic frame (an LRW built from its IOmap), not the 77-byte
 *     LRD+LRW+BRD frame TwinCAT sends
 *   - SOEM's own bring-up state machine
 *   - no mailbox-state LRD, no per-cycle BRD 0x0130, no BWR 0x0300 clear
 *
 * The only things held in common are the ones under test: both drives in OP,
 * exchanging process data on a 1 ms cycle.
 *
 * If this also sees no drops, the master is excluded by two independent
 * codebases and what remains is physical. If it DOES see drops, then
 * something in our master is protective and the difference between the two
 * becomes the most interesting object in the investigation.
 *
 * Lost links are detected the same way ecat_op does it: the ESC lost-link
 * counters at 0x0310 are read by auto-increment addressing a few times a
 * second and differenced. Those are not cleared by anything either master
 * does, so they are a running total.
 *
 * WRITES TO THE SLAVES: SOEM performs a normal master bring-up (station
 * addresses, SyncManagers, FMMUs, AL state). No EEPROM is written. Slaves are
 * returned to INIT on exit. */
#include "soem/soem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <errno.h>

#define MAX_SLAVES 64

static ecx_contextt ctx;
static uint8 iomap[8192];
static volatile sig_atomic_t running = 1;
static void on_sig(int s) { (void)s; running = 0; }

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void sleep_until(uint64_t deadline)
{
    struct timespec ts = { .tv_sec  = deadline / 1000000000ULL,
                           .tv_nsec = deadline % 1000000000ULL };
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) ;
}

int main(int argc, char **argv)
{
    const char *iface = (argc > 1) ? argv[1] : "enp2s0";
    int dur = (argc > 2) ? atoi(argv[2]) : 3600;
    long period_us = (argc > 3) ? atol(argv[3]) : 1000;

    printf("SOEM independent check\n");
    printf("Interface:  %s\n", iface);
    printf("Cycle:      %ld us\n", period_us);
    printf("Duration:   %d s\n", dur);
    printf("\n  This is NOT a TwinCAT replay. SOEM configures the drives its own\n"
           "  way — its own PDO mapping from the object dictionary, its own\n"
           "  cyclic frame, its own bring-up. The point is independence: only\n"
           "  'both drives in OP at 1 ms' is held in common with ecat_op.\n"
           "\n  It writes to the slaves (station addresses, SyncManagers, FMMUs,\n"
           "  AL state). No EEPROM. Slaves return to INIT on exit.\n\n");

    if (!ecx_init(&ctx, iface)) { printf("FAILED: no socket on %s\n", iface); return 1; }
    if (ecx_config_init(&ctx) <= 0) { printf("FAILED: no slaves found\n"); ecx_close(&ctx); return 1; }
    int ns = ctx.slavecount;
    printf("Slaves found: %d\n", ns);
    for (int i = 1; i <= ns && i <= MAX_SLAVES; i++)
        printf("  %d: %s  (man 0x%08X id 0x%08X rev 0x%08X)\n", i,
               ctx.slavelist[i].name, ctx.slavelist[i].eep_man,
               ctx.slavelist[i].eep_id, ctx.slavelist[i].eep_rev);

    ecx_config_map_group(&ctx, iomap, 0);
    ec_groupt *grp = ctx.grouplist + 0;
    printf("Mapped %dO + %dI bytes\n", grp->Obytes, grp->Ibytes);
    ecx_configdc(&ctx);

    ecx_statecheck(&ctx, 0, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE * 4);
    if (ctx.slavelist[0].state != EC_STATE_SAFE_OP) {
        printf("FAILED: not all slaves reached SAFEOP\n");
        ecx_readstate(&ctx);
        for (int i = 1; i <= ns; i++)
            printf("  slave %d state 0x%04X AL 0x%04X %s\n", i,
                   ctx.slavelist[i].state, ctx.slavelist[i].ALstatuscode,
                   ec_ALstatuscode2string(ctx.slavelist[i].ALstatuscode));
        ecx_close(&ctx); return 1;
    }
    printf("All slaves in SAFEOP\n");

    /* Outputs must already be valid before OP is requested — the same rule
     * that stopped ecat_op's third attempt. */
    ecx_send_processdata(&ctx);
    ecx_receive_processdata(&ctx, EC_TIMEOUTRET);

    ctx.slavelist[0].state = EC_STATE_OPERATIONAL;
    ecx_writestate(&ctx, 0);
    int ok = 0;
    for (int i = 0; i < 40; i++) {
        ecx_send_processdata(&ctx);
        ecx_receive_processdata(&ctx, EC_TIMEOUTRET);
        ecx_statecheck(&ctx, 0, EC_STATE_OPERATIONAL, 50000);
        if (ctx.slavelist[0].state == EC_STATE_OPERATIONAL) { ok = 1; break; }
    }
    if (!ok) {
        printf("FAILED: not all slaves reached OP\n");
        ecx_readstate(&ctx);
        for (int i = 1; i <= ns; i++)
            printf("  slave %d state 0x%04X AL 0x%04X %s\n", i,
                   ctx.slavelist[i].state, ctx.slavelist[i].ALstatuscode,
                   ec_ALstatuscode2string(ctx.slavelist[i].ALstatuscode));
        ctx.slavelist[0].state = EC_STATE_INIT; ecx_writestate(&ctx, 0);
        ecx_close(&ctx); return 1;
    }
    int expected = grp->outputsWKC * 2 + grp->inputsWKC;
    printf("All slaves in OP.  expected WKC = %d\n\n", expected);
    printf("Cyclic exchange running. Ctrl-C to stop.\n\n");

    signal(SIGINT, on_sig); signal(SIGTERM, on_sig);

    uint8 lost[MAX_SLAVES][2]; int have[MAX_SLAVES];
    memset(lost, 0, sizeof lost); memset(have, 0, sizeof have);
    unsigned long long cycles = 0, wkc_bad = 0, lost_events = 0;
    unsigned long long tot_lost[MAX_SLAVES][2]; memset(tot_lost, 0, sizeof tot_lost);
    uint64_t t0 = now_ns(), next = t0, end = t0 + (uint64_t)dur * 1000000000ULL;
    uint64_t poll_period = 200000000ULL, next_poll = t0 + poll_period;  /* 5/s */

    while (running && now_ns() < end) {
        next += (uint64_t)period_us * 1000ULL;
        sleep_until(next);

        ecx_send_processdata(&ctx);
        int wkc = ecx_receive_processdata(&ctx, EC_TIMEOUTRET);
        cycles++;
        if (wkc != expected) {
            if (++wkc_bad < 20)
                printf("  [%8.3f] WKC %d (expected %d)\n",
                       (double)(now_ns() - t0) / 1e9, wkc, expected);
        }

        /* Lost-link counters, by auto-increment addressing, a few times a
         * second. 0x0310 is a running total: nothing clears it. */
        if (now_ns() >= next_poll) {
            next_poll += poll_period;
            for (int s = 0; s < ns && s < MAX_SLAVES; s++) {
                uint8 buf[2] = {0, 0};
                int w = ecx_APRD(&ctx.port, (uint16)(0 - s), 0x0310,
                                 sizeof buf, buf, EC_TIMEOUTRET);
                if (w < 1) continue;
                if (have[s]) {
                    for (int p = 0; p < 2; p++)
                        if (buf[p] != lost[s][p]) {
                            unsigned dl = (uint8)(buf[p] - lost[s][p]);
                            tot_lost[s][p] += dl; lost_events += dl;
                            printf("  [%8.3f] slave %d port %d  *** LOST LINK +%u "
                                   "(now %u) ***\n", (double)(now_ns() - t0) / 1e9,
                                   s, p, dl, buf[p]);
                        }
                }
                lost[s][0] = buf[0]; lost[s][1] = buf[1]; have[s] = 1;
            }
        }
    }

    double secs = (double)(now_ns() - t0) / 1e9;
    printf("\nReturning slaves to INIT...\n");
    ctx.slavelist[0].state = EC_STATE_INIT;
    ecx_writestate(&ctx, 0);
    ecx_close(&ctx);

    printf("\n── Summary ────────────────────────────────────────────────\n");
    printf("  Elapsed:        %.1f s\n", secs);
    printf("  Cycles:         %llu  (%.0f Hz achieved)\n", cycles, cycles / secs);
    printf("  WKC mismatches: %llu\n", wkc_bad);
    printf("  *** LOST LINK events: %llu ***\n", lost_events);
    for (int s = 0; s < ns && s < MAX_SLAVES; s++)
        for (int p = 0; p < 2; p++)
            if (tot_lost[s][p])
                printf("    slave %d port %d: %llu\n", s, p, tot_lost[s][p]);
    if (!lost_events) printf("    no lost-link counter moved on any slave\n");
    return 0;
}
