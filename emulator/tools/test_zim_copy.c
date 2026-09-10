/* Exercise the production copy dispatcher as C33 code under Grifo. */
#include <grifo.h>
#include <regs.h>
#include <string.h>
#include <stdint.h>
#include "zim_copy.h"
size_t zim_memory_bank_size(void) { return 4u << 20; }
volatile unsigned long copy_test_result[4]; /* cases, failure, DMA calls, bytes */
static unsigned char expected[32832];
static unsigned char initial[32832];
static unsigned char arena[32832];
void __attribute__((noinline)) copy_test_done(void) { asm volatile("nop"); }
static void check(int condition)
{
    if (!condition) { copy_test_result[1] = copy_test_result[0] + 1; copy_test_done(); for (;;) { } }
}
static void one(unsigned d, unsigned s, unsigned n, int move)
{
    for (unsigned i=0;i<sizeof(arena);++i) initial[i]=expected[i]=arena[i]=(i*73u+(i>>5))&255;
    for (unsigned i=0;i<n;++i) expected[d+i]=initial[s+i];
    void *ret=move ? zim_copy(arena+d,arena+s,n) : zim_copy(arena+d,arena+s,n);
    check(ret==arena+d);
    for (unsigned i=0;i<sizeof(arena);++i) check(arena[i]==expected[i]);
    ++copy_test_result[0];
}
static void bulk(unsigned char *dst, unsigned char *src, unsigned n)
{
    for (unsigned i=0;i<n+32;++i) dst[(int)i-16]=0xa5;
    for (unsigned i=0;i<n;++i) src[i]=(i*37u+(i>>7))&255;
    check(zim_copy(dst,src,n)==dst);
    for (unsigned i=0;i<n;++i) check(dst[i]==((i*37u+(i>>7))&255));
    for (unsigned i=0;i<16;++i) check(dst[(int)i-16]==0xa5 && dst[n+i]==0xa5);
    ++copy_test_result[0];
}
int grifo_main(int argc, char **argv)
{
    static const unsigned lengths[]={0,1,3,4,7,31,32,33,255,256,257,511,1023,4095,4096,4097,16385};
    (void)argc; (void)argv;
#ifndef COPY_FAULT_TEST
    for (unsigned i=0;i<sizeof(lengths)/sizeof(*lengths);++i) {
        unsigned n=lengths[i];
        /* Every pair of source/destination alignments, both directions,
         * plus real overlap at byte, word and batch distances. */
        for(unsigned a=0;a<4;++a) for(unsigned b=0;b<4;++b) {
            one(16416+a,16+b,n>16384?16384:n,0);
            one(16+a,16416+b,n>16384?16384:n,1);
        }
        for(unsigned gap=1;gap<=64;gap*=4) {
            one(128+gap,128,n,1); one(128,128+gap,n,1);
        }
        one(128,128,n,0);
        watchdog(WATCHDOG_KEY);
    }
#endif
    unsigned char *allocation=memory_allocate(12u<<20,"copy test");
    check(allocation!=0);
    unsigned char *src=(unsigned char *)(((uintptr_t)allocation+0x3fffff)&~0x3fffff)+1024;
    unsigned char *dst=src+0x400000;
#ifdef COPY_FAULT_TEST
    bulk(dst,src,8192);
    check(zim_copy_stats.dma_errors==1 && !zim_copy_stats.dma_calls);
    check(zim_copy_diag.attempts==1 && zim_copy_diag.failed==1 && !zim_copy_diag.latched);
    check(zim_copy_diag.first_fault.attempt==1 && zim_copy_diag.first_fault.enabled==1 &&
          zim_copy_diag.first_fault.count0==2048 && !zim_copy_diag.first_fault.tf0 &&
          !(zim_copy_diag.first_fault.fdma&1) && zim_copy_diag.first_fault.src==(uintptr_t)src);
    check(!(REG_HS0_EN&1) && !(REG_HS0_TF&1) && !(REG_INT_FDMA&1));
    bulk(dst,src,8192);
    check(zim_copy_stats.dma_errors==1 && !zim_copy_stats.dma_calls);
    check(zim_copy_diag.attempts==2 && zim_copy_diag.latched==1 &&
          zim_copy_diag.first_fault.attempt==1);
    copy_test_done();
    return 0;
#endif
    uint16_t saved[12];
    volatile uint16_t *hs=(volatile uint16_t *)(REG_BASE+0x1120);
    volatile uint16_t *adv=(volatile uint16_t *)(REG_BASE+0x1160);
    uint16_t mode=REG_HS_CNTLMODE, acctime=REG_HS_ACCTIME;
    uint8_t irq=REG_INT_EDMA, select=REG_HSDMA_HTGR1;
    for(unsigned i=0;i<6;++i) { saved[i]=hs[i]; saved[i+6]=adv[i]; }
    unsigned long before=zim_copy_stats.dma_calls;
    bulk(dst,src,4095); bulk(dst,src,4096); bulk(dst,src,16385); bulk(dst,src,32771);
    check(zim_copy_stats.dma_calls>before);
    for(unsigned i=0;i<6;++i) check(saved[i]==hs[i] && saved[i+6]==adv[i]);
    check(REG_HS_CNTLMODE==mode && REG_HS_ACCTIME==acctime && REG_INT_EDMA==irq && REG_HSDMA_HTGR1==select);
    check(!(REG_HS0_EN&1) && !(REG_HS0_TF&1) && !(REG_INT_FDMA&1));
    before=zim_copy_stats.dma_calls;
    uint16_t en=REG_HS1_EN; REG_HS1_EN=1;
    bulk(dst,src,8192);
    check(zim_copy_stats.dma_calls==before && REG_HS1_EN==1);
    check(zim_copy_diag.busy==1 && zim_copy_diag.first_guard.enabled==2 &&
          zim_copy_diag.first_guard.src==(uintptr_t)src &&
          zim_copy_diag.first_guard.bytes==8192);
    ZIM_COPY_STATE first_guard=zim_copy_diag.first_guard;
    REG_HS1_EN=en;
    /* A pending software request on a disabled channel is not ours to
     * consume. The copy falls back without changing that request. */
    REG_HSDMA_HTGR1 &= 0xf0;
    REG_HSDMA_HSOFTTGR=1;
    check(REG_HS0_TF&1);
    bulk(dst,src,8192);
    check(zim_copy_stats.dma_calls==before && (REG_HS0_TF&1) &&
          !REG_HS0_EN && zim_copy_diag.trigger==1);
    REG_HS0_TF=1;
    /* Complete an owned, single-word transfer but leave its IRQ cause
     * pending, reproducing a possible handoff from earlier boot code. */
    REG_HS_CNTLMODE=1; REG_HS_ACCTIME=0; REG_INT_EDMA &= ~1u;
    REG_HS0_ADVMODE=1; REG_HS0_CNT=1; REG_HS0_CTRL=0x8000;
    REG_HS0_SADR_H=0x3000; REG_HS0_DADR_H=0x7000;
    REG_HS0_ADV_SADR_L=(uintptr_t)src; REG_HS0_ADV_SADR_H=(uintptr_t)src>>16;
    REG_HS0_ADV_DADR_L=(uintptr_t)dst; REG_HS0_ADV_DADR_H=(uintptr_t)dst>>16;
    REG_HS0_EN=1; REG_HSDMA_HSOFTTGR=1;
    unsigned long start=timer_get();
    while ((REG_HS0_EN&1) && timer_get()-start<600000) { }
    check(!(REG_HS0_EN&1) && (REG_INT_FDMA&1));
    bulk(dst,src,8192);
    check(zim_copy_stats.dma_calls==before && (REG_INT_FDMA&1) &&
          zim_copy_diag.irq==1 && !zim_copy_diag.failed);
    check(!memcmp(&first_guard,&zim_copy_diag.first_guard,sizeof(first_guard)));
    /* Reproduce the measured reset state without changing other causes.
     * An enabled IRQ still has an owner, even with zero configuration. */
    for(unsigned i=0;i<6;++i) { hs[i]=0; adv[i]=0; }
    critical_t outer=critcal_enter();
    REG_INT_EDMA=irq|1;
    bulk(dst,src,8192);
    check(zim_copy_stats.dma_calls==before && (REG_INT_FDMA&1) &&
          !zim_copy_diag.reset_irq && zim_copy_diag.irq==2);
    REG_INT_EDMA=irq;
    critical_exit(outer);
    unsigned other_causes=REG_INT_FDMA&~1u;
    bulk(dst,src,8192);
    check(zim_copy_stats.dma_calls==before+1 && zim_copy_diag.reset_irq==1 &&
          !(REG_INT_FDMA&1) && (REG_INT_FDMA&~1u)==other_causes);
    check(zim_copy_diag.first_reset.attempt && !zim_copy_diag.first_reset.enabled &&
          !zim_copy_diag.first_reset.tf0 && (zim_copy_diag.first_reset.fdma&1) &&
          !zim_copy_diag.first_reset.control0 && !zim_copy_diag.first_reset.count0);
    for(unsigned i=0;i<6;++i) check(!hs[i] && !adv[i]);
    before=zim_copy_stats.dma_calls;
    REG_HS0_TF=1; REG_INT_FDMA=1;
    for(unsigned i=0;i<6;++i) { hs[i]=saved[i]; adv[i]=saved[i+6]; }
    REG_HS_CNTLMODE=mode; REG_HS_ACCTIME=acctime;
    REG_INT_EDMA=irq; REG_HSDMA_HTGR1=select;
    bulk(dst,src,8192);
    check(zim_copy_stats.dma_calls==before+1); /* resumes when the owner clears it */
    before=zim_copy_stats.dma_calls;
    bulk(src+0x100000,src,8192);
    check(zim_copy_stats.dma_calls==before); /* same-bank CPU dispatch */
    check(zim_copy_diag.layout>0);
    /* A0 is outside DMA's accessible areas; IVRAM is a valid destination. */
    bulk((unsigned char *)0x81a10,src,5120);
    check(zim_copy_stats.dma_calls==before+1);
    check(!zim_copy_stats.dma_errors);
    memory_free(allocation,"copy test");
    copy_test_result[2]=zim_copy_stats.dma_calls;
    copy_test_result[3]=zim_copy_stats.dma_bytes;
    copy_test_done();
    return 0;
}
