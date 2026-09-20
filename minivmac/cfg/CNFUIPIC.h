/* WikiReader compiler tuning for Mini vMac's platform-independent core. */
#ifndef MINIVMAC_WR_CNFUIPIC_H
#define MINIVMAC_WR_CNFUIPIC_H

#define MINIVMAC_FAST_M68K __attribute__((section(".fastcode"), noinline))
#define MINIVMAC_FAST_M68K_DATA __attribute__((section(".fastcode.data"), aligned(4)))
#define MINIVMAC_FAST_M68K_BSS __attribute__((section(".fastbss"), aligned(4)))
#define MINIVMAC_INDIRECT_FAST_M68K 1
#undef SmallGlobals
#define SmallGlobals 1
#ifndef MINIVMAC_JIT
#define MINIVMAC_JIT 1
#endif

#endif
