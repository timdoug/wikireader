/* Run the production receive backend as C33 code, including timeout recovery. */
#include "../../samo-lib/grifo/src/sd_dma.c"

enum fault_kind {
    NO_FAULT,
    STALL_TRANSMIT,
    STALL_BOTH,
    RECEIVE_OVERFLOW
};
static enum fault_kind fault;
static unsigned timer_calls, case_number, measure;
volatile unsigned test_result[4];
static BYTE buffer[520] __attribute__((aligned(4)));

void test_done(void) __attribute__((noinline, noreturn));
void test_done(void) { for (;;) asm volatile ("nop"); }
static void require(int condition, unsigned line)
{
    if (!condition) {
        test_result[0] = 0xbad;
        test_result[1] = case_number;
        test_result[2] = line;
        test_done();
    }
}
#define CHECK(c) require((c), __LINE__)

/* Inject a stalled transmit engine, then a stalled receive engine too.
 * A timer jump forces the driver's timeout without millions of empty polls. */
unsigned long __attribute__((noinline)) Timer_get(void)
{
    unsigned call = timer_calls++;
    if (fault != NO_FAULT && call == 0) {
        REG_IDMA_EN = 0;
        if (fault == STALL_BOTH || fault == RECEIVE_OVERFLOW)
            REG_HS3_EN = DMA_DISABLED;
    }
    if (fault == RECEIVE_OVERFLOW && call == 1) {
        REG_SPI_TXD = 0xffffffffUL;
        while (REG_SPI_STAT & BSYF) ; /* overwrite the unread first word */
    }
    return call * (fault ? DMA_TIMEOUT_TICKS + 1 : 60);
}
int Serial_printf(const char *fmt, ...) { (void)fmt; return 0; }
int snprintf(char *s, size_t n, const char *fmt, ...)
{
    (void)fmt;
    if (n) *s = 0;
    return 0;
}
void mmc_set_spi_receive_dma(mmc_spi_receive_dma_fn fn) { (void)fn; }
void *memset(void *dst, int value, size_t size)
{
    BYTE *p = dst;
    while (size--) *p++ = (BYTE)value;
    return dst;
}

static BYTE exchange(BYTE out)
{
    REG_SPI_TXD = out;
    while (!(REG_SPI_STAT & RDFF)) ;
    return (BYTE)REG_SPI_RXD;
}
static void begin_read(void)
{
    unsigned i;
    REG_P5_P5D |= 1;
    exchange(0xff);
    REG_P5_P5D &= ~1;
    exchange(0xff);
    exchange(0x51); /* CMD17, sector zero */
    for (i = 0; i < 4; i++) exchange(0);
    exchange(0x95);
    for (i = 0; i < 100000; i++)
        if (exchange(0xff) == 0) break;
    CHECK(i < 100000);
    for (i = 0; i < 100000; i++)
        if (exchange(0xff) == 0xfe) break;
    CHECK(i < 100000);
}
static void run_case(unsigned offset, unsigned bytes, enum fault_kind injection)
{
    unsigned i;
    int got;
    BYTE *dst = buffer + 4 + offset;
    File_IOStats stats;
    ++case_number;
    fault = injection;
    timer_calls = 0;
    dma_given_up = 0;
    SD_DMA_profile(&stats, false);
    SD_DMA_profile(&stats, measure != 0);
    for (i = 0; i < sizeof buffer; i++) buffer[i] = 0xa5;
    begin_read();
    got = receive_dma(dst, bytes);
    CHECK(REG_SPI_INT == 0x14); /* observed stock-loader configuration */
    CHECK(REG_P6_47_CFP == 0x54 && REG_P6_IOC6 == 0 && REG_P6_P6D == 0x30);
    if (measure) {
        SD_DMA_profile(&stats, true);
        CHECK(stats.dma_bits == SD_DMA_BITS);
        CHECK(stats.dma_wait_ticks > 0);
        CHECK(stats.dma_timeouts == (injection != 0));
        CHECK(stats.dma_errors == (injection == RECEIVE_OVERFLOW));
        CHECK(stats.dma_disabled == (injection != 0));
        CHECK(stats.dma32_bytes + stats.dma8_bytes == (injection ? 0 : bytes));
        if (!injection)
            CHECK(stats.dma32_bytes ==
                  (SD_DMA_BITS == 32 && !offset && !(bytes & 3) ? bytes : 0));
    }
    CHECK(REG_SPI_CTL1 == (BPT_8_BITS | MODE_MASTER | ENA | RXDE | TXDE));
    CHECK(!(REG_HS3_EN & 1) && REG_IDMA_EN == 0);
    if (injection == RECEIVE_OVERFLOW) {
        CHECK(got == -1 && dma_given_up);
        CHECK(receive_dma(dst, bytes) == 0);
        if (measure) {
            SD_DMA_profile(&stats, false);
            CHECK(stats.dma_bypass_bytes == bytes);
        }
        return;
    }
    CHECK(got >= 0 && (unsigned)got <= bytes);
    if (injection)
        CHECK(got == (SD_DMA_BITS == 8 || offset || (bytes & 3) ? 1 : 4)
              && dma_given_up);
    else
        CHECK(got == (int)bytes && !dma_given_up);
    for (i = (unsigned)got; i < 512; i++) dst[i] = exchange(0xff);
    CHECK(exchange(0xff) == 0xff && exchange(0xff) == 0xff);
    for (i = 0; i < 512; i++)
        CHECK(dst[i] == (BYTE)((i * 73) ^ (i >> 3) ^ 0x9d));
    for (i = 0; i < 4 + offset; i++) CHECK(buffer[i] == 0xa5);
    for (i = 516 + offset; i < sizeof buffer; i++) CHECK(buffer[i] == 0xa5);
    if (injection) CHECK(receive_dma(dst, bytes) == 0);
    if (measure) {
        SD_DMA_profile(&stats, false);
        CHECK(stats.dma_bypass_bytes == (injection ? bytes : 0));
    }
}
static void run_tests(void)
{
    SD_DMA_initialise();
    REG_P5_P5D = 5; /* card and EEPROM deselected */
    REG_P6_47_CFP = 0x54; /* physical SPI mux; exercise the clock hold */
    REG_P6_IOC6 = 0;
    REG_P6_P6D = 0x30;
    REG_SPI_CTL1 = BPT_8_BITS | MODE_MASTER | ENA | RXDE | TXDE;
    REG_SPI_WAIT = 0;
    REG_SPI_INT = 0x14;
    /* Initial filesystem reads remain byte-wide until the boot checkpoint. */
    run_case(0, 512, NO_FAULT);
    CHECK(!dma_word_enabled);
    SD_DMA_enable_wide();
    for (measure = 0; measure < 2; measure++) {
        run_case(0, 512, NO_FAULT);
        run_case(1, 512, NO_FAULT);
        run_case(2, 512, NO_FAULT);
        run_case(3, 512, NO_FAULT);
        run_case(0, 16, NO_FAULT);
        run_case(0, 64, NO_FAULT);
        run_case(0, 510, NO_FAULT);
        run_case(0, 512, STALL_TRANSMIT);
        run_case(0, 512, STALL_BOTH);
        run_case(1, 512, STALL_TRANSMIT);
        run_case(1, 512, STALL_BOTH);
        run_case(0, 512, RECEIVE_OVERFLOW);
    }
#if SD_DMA_BITS == 32
    {
        /* A long hardware inter-character wait holds BSYF past the driver's
         * bounded idle wait. It must fail without reading/writing CTL1. */
        File_IOStats stats;
        ++case_number;
        fault = timer_calls = dma_given_up = 0;
        set_spi_control(BPT_8_BITS | MODE_MASTER | ENA | RXDE | TXDE |
                        MCBR_MCLK_DIV_512);
        REG_SPI_WAIT = 0xffff;
        exchange(0xff);
        REG_SPI_TXD = 0xff;
        SD_DMA_profile(&stats, true);
        CHECK(receive_dma(buffer, 512) == -1);
        CHECK(REG_SPI_STAT & BSYF);
        CHECK(REG_SPI_INT == 0x14);
        SD_DMA_profile(&stats, false);
        CHECK(stats.dma_timeouts == 1 && stats.dma_errors == 1 && stats.dma_disabled);
    }
#endif
    test_result[0] = 0x600d;
    test_result[1] = case_number;
    test_done();
}
void __attribute__((noreturn)) _start(void)
{
    asm volatile ("xld.w %r15, 0x101ff000\n\tld.w %sp, %r15\n\t"
                  "xld.w %r15, __dp\n\tld.w %r4, 0\n\tld.w %psr, %r4");
    run_tests();
    test_done();
}
