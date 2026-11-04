/* SPI DMA beside the CPU: how far each kind of CPU work holds the card
 * transfers back, and how far the transfers hold the CPU back.  The card is
 * deselected.  As the card drivers do, HSDMA2 (on transmit empty) or IDMA
 * (on receive full) feeds the SPI all-ones from an IVRAM word and HSDMA3
 * takes each character it receives, at the clock Grifo set, while the CPU
 * runs one of a set of loops until HSDMA3 is done.  Results go to
 * spibench.log and the serial port.  See README.md. */
#include <grifo.h>
#include <regs.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define UNITS		1024
#define REPEATS		3
#define SOLO		20000UL		/* iterations timed without DMA */
#define SOLO_BLOCKS	400UL		/* ...of the loops a block a pass */
#define LIMIT		200000UL	/* passes: a transfer that never ends */
#define REGION		0x8000UL	/* each CPU stream wraps within 32 KiB */
#define WINDOW		((uint8_t *)0x81a00)	/* IVRAM the display does not use */
#define WINDOW_SIZE	5632
#define DUMMY		((volatile uint32_t *)(WINDOW + 4096))
#define SPI_RXD_ADDRESS	(REG_BASE + 0x1700)
#define SPI_TXD_ADDRESS	(REG_BASE + 0x1704)
#define BPT_MASK	(31UL << 10)
#define HSDMA2		(1 << 2)
#define HSDMA3		(1 << 3)
#define IDLE_CHANNEL	(1 << 1)	/* HSDMA1: never raised here */
#define SPI_IDMA	(1 << 4)	/* IDMA channel 0x24, SPI receive */
/* Channel 0x24's descriptor, in DSTRAM past Grifo's own. */
#define IDMA_SLOT	((volatile uint32_t *)0x84400)
#define IDMA_TABLE	(0x84400UL - 0x24 * 16)
/* The all-ones word while IVRAM is in area 0, past the descriptor. */
#define DSTRAM_ONES	((volatile uint32_t *)0x84410)

/* Where the transmitter's all-ones come from. */
static volatile uint32_t *tx_source = DUMMY;

typedef unsigned long loop_fn(volatile uint8_t *flag, unsigned long mask,
			      unsigned long limit, uint32_t *p, uint32_t *q);

/* Each pass does its work, then reads the flag and counts down.  Work on
 * p (and q) wraps within REGION, which both buffers are aligned to twice. */
#define LOOP(name, where, body)						\
static unsigned long __attribute__((noinline, section(where)))		\
name(volatile uint8_t *flag, unsigned long mask, unsigned long limit,	\
     uint32_t *p, uint32_t *q)						\
{									\
	unsigned long left = limit, t, wrap = ~REGION;			\
	asm volatile (".balign 16\n1:\n\t" body				\
		      "ld.ub %[t],[%[flag]]\n\t"				\
		      "and %[t],%[mask]\n\t"				\
		      "jrne 2f\n\t"					\
		      "sub %[left],1\n\t"					\
		      "jrne 1b\n"					\
		      "2:"						\
		      : [left] "+r" (left), [p] "+r" (p), [q] "+r" (q),	\
			[t] "=&r" (t)					\
		      : [flag] "r" (flag), [mask] "r" (mask),		\
			[wrap] "r" (wrap)				\
		      : "r0", "r1", "r2", "memory", "cc");		\
	return limit - left;						\
}

#define LD	"ld.w %[t],[%[p]]+\n\t"
#define ST	"ld.w [%[p]]+,%[t]\n\t"
#define WRAP_P	"and %[p],%[wrap]\n\t"
#define MOVES	".rept 20\n\tld.w %[t],%[t]\n\t.endr\n\t"

LOOP(spin, ".fastcode", "")
LOOP(load1, ".fastcode", LD WRAP_P)
LOOP(load4, ".fastcode", LD LD LD LD WRAP_P)
LOOP(store1, ".fastcode", ST WRAP_P)
LOOP(store4, ".fastcode", ST ST ST ST WRAP_P)
LOOP(copy4, ".fastcode",
     "ld.w %%r0,[%[p]]+\n\tld.w %%r1,[%[p]]+\n\t"
     "ld.w %%r2,[%[p]]+\n\tld.w %[t],[%[p]]+\n\t"
     "ld.w [%[q]]+,%%r0\n\tld.w [%[q]]+,%%r1\n\t"
     "ld.w [%[q]]+,%%r2\n\tld.w [%[q]]+,%[t]\n\t"
     WRAP_P "and %[q],%[wrap]\n\t")
/* q is the stride: a row (1 KiB) and a word, so every load opens a row. */
LOOP(rowmiss, ".fastcode", "ld.w %[t],[%[p]]\n\tadd %[p],%[q]\n\t" WRAP_P)
LOOP(moves_a0, ".fastcode", MOVES)
/* Twenty register moves (wremu stops at a run of zero words, so not
   nops): too long for the fetch queue to hold, so every pass fetches
   from SDRAM. */
LOOP(moves_sdram, ".text.spibench_sdram", MOVES)

/* The Linux card driver's check, sd_unpack_crc(), a word a pass: a load
 * from the stream, the CRC-ITU-T of its four bytes from tables in A0 RAM,
 * and the word swapped into another buffer, in its C, from A0 RAM as the
 * kernel runs it.  lookup4 is the four table loads alone. */
static uint16_t crc_tables[4 * 256] __attribute__((section(".fastbss")));
static volatile uint32_t crc_sink;

static void crc_tables_init(void)
{
	uint16_t *t = crc_tables;
	unsigned i, v;

	for (i = 0; i < 256; i++) {
		uint16_t c = i << 8;
		for (v = 0; v < 8; v++)
			c = c & 0x8000 ? c << 1 ^ 0x1021 : c << 1;
		t[i] = c;
	}
	for (i = 256; i < 4 * 256; i++)
		t[i] = t[i - 256] << 8 ^ t[t[i - 256] >> 8];
}

static unsigned long __attribute__((noinline, section(".fastcode")))
crc(volatile uint8_t *flag, unsigned long mask, unsigned long limit,
    uint32_t *p, uint32_t *q)
{
	const uint16_t *t0 = crc_tables, *t1 = t0 + 256;
	const uint16_t *t2 = t0 + 512, *t3 = t0 + 768;
	unsigned long left = limit, wrap = ~REGION;
	uint16_t c = 0;

	for (; left; left--) {
		uint32_t raw = *p++, x = raw ^ (uint32_t)c << 16;

		c = t3[x >> 24] ^ t2[(x >> 16) & 0xff] ^
		    t1[(x >> 8) & 0xff] ^ t0[x & 0xff];
		*q++ = __builtin_bswap32(raw);
		p = (uint32_t *)((unsigned long)p & wrap);
		q = (uint32_t *)((unsigned long)q & wrap);
		if (*flag & mask)
			break;
	}
	crc_sink = c;
	return limit - left;
}

/* The kernel's own sd_unpack_crc(), its machine code byte for byte, at
 * the 16-byte alignment it has there: a pass is one block, from the stream
 * into another buffer, as the driver's check does it, on consecutive
 * blocks; unpack2 the blocks that start two bytes into a word, as every
 * other block of a stream does. */
static unsigned char unpack_code[] __attribute__((section(".fastcode.unpack"),
						aligned(16), used)) = {
#include "unpack_crc.inc"
};
typedef uint16_t unpack_fn(const uint32_t *words, uint32_t *out, unsigned n,
			   uint32_t carry, unsigned lead, const uint16_t *tables);

static inline unsigned long __attribute__((always_inline))
unpack(volatile uint8_t *flag, unsigned long mask, unsigned long limit,
       uint32_t *p, uint32_t *q, unsigned lead)
{
	unpack_fn *fn = (unpack_fn *)(unsigned long)unpack_code;
	unsigned long left = limit, wrap = ~REGION;
	uint16_t c = 0;

	for (; left; left--) {
		c ^= fn(p, q, 128, 0x1234, lead, crc_tables);
		p = (uint32_t *)((unsigned long)(p + 130) & wrap);
		q = (uint32_t *)((unsigned long)(q + 128) & wrap);
		if (*flag & mask)
			break;
	}
	crc_sink = c;
	return limit - left;
}

static unsigned long __attribute__((noinline, section(".fastcode")))
unpack0(volatile uint8_t *flag, unsigned long mask, unsigned long limit,
	uint32_t *p, uint32_t *q)
{
	return unpack(flag, mask, limit, p, q, 0);
}

static unsigned long __attribute__((noinline, section(".fastcode")))
unpack2(volatile uint8_t *flag, unsigned long mask, unsigned long limit,
	uint32_t *p, uint32_t *q)
{
	return unpack(flag, mask, limit, p, q, 2);
}

static unsigned long __attribute__((noinline, section(".fastcode")))
lookup4(volatile uint8_t *flag, unsigned long mask, unsigned long limit,
	uint32_t *p, uint32_t *q)
{
	const uint16_t *t0 = crc_tables, *t1 = t0 + 256;
	const uint16_t *t2 = t0 + 512, *t3 = t0 + 768;
	unsigned long left = limit;
	uint32_t x = 0x12345678;
	uint16_t c = 0;

	(void)p;
	(void)q;
	for (; left; left--) {
		x = (x * 5 + 1) ^ c;
		c = t3[x >> 24] ^ t2[(x >> 16) & 0xff] ^
		    t1[(x >> 8) & 0xff] ^ t0[x & 0xff];
		if (*flag & mask)
			break;
	}
	crc_sink = c;
	return limit - left;
}

static const struct {
	const char *name;
	loop_fn *fn;
} loops[] = {
	{ "spin", spin }, { "load1", load1 }, { "load4", load4 },
	{ "store1", store1 }, { "store4", store4 }, { "copy4", copy4 },
	{ "rowmiss", rowmiss }, { "moves-a0", moves_a0 },
	{ "moves-sdram", moves_sdram }, { "lookup4", lookup4 },
	{ "crc", crc },
};
static const unsigned widths[] = { 32, 16, 8 };

static uint8_t saved_window[WINDOW_SIZE];
static uint32_t *stream_p, *stream_q, *rx_sdram;
static unsigned long saved_ctl1, timer_pair;
static uint16_t saved_mode;
static uint8_t saved_select, saved_cs;
static uint32_t saved_slot[4];
static uint16_t saved_base0, saved_base1;
static char log_text[24576];
static size_t log_used, log_saved;
static unsigned cases, failures;

static void out(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void out(const char *format, ...)
{
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(log_text + log_used, sizeof(log_text) - log_used,
		      format, ap);
	va_end(ap);
	if (n > 0) {
		debug_print(log_text + log_used);
		log_used += (size_t)n < sizeof(log_text) - log_used ?
			    (size_t)n : sizeof(log_text) - log_used - 1;
	}
}

/* V.2.8 forbids even reading CTL1 while BSYF is set. */
static int wait_spi_idle(void)
{
	for (unsigned long polls = 0; polls < 100000; polls++)
		if (!(REG_SPI_STAT & BSYF))
			return 1;
	return 0;
}

/* Grifo's sd_dma.c set_spi_control: P67 held at CPOL across the ENA cycle,
 * and SPI_INT clear while ENA changes. */
static void set_spi_control(unsigned long control)
{
	critical_t state = critcal_enter();
	uint8_t mux = REG_P6_47_CFP;
	uint8_t direction = REG_P6_IOC6;
	uint8_t data = REG_P6_P6D;
	int hold_clock = (mux & 0xc0) == 0x40;
	unsigned long interrupts = REG_SPI_INT;
	unsigned settle;

	if (hold_clock) {
		REG_P6_P6D = (data & ~0x80) | ((control & CPOL) ? 0x80 : 0);
		REG_P6_IOC6 = direction | 0x80;
		REG_P6_47_CFP = mux & ~0xc0;
	}
	REG_SPI_INT = 0;
	REG_SPI_CTL1 &= ~ENA;
	REG_SPI_CTL1 = control & ~ENA;
	REG_SPI_CTL1 = control;
	if (hold_clock) {
		settle = 4u << ((control >> 4) & 7);
		while (settle--)
			asm volatile ("nop");
		REG_P6_47_CFP = mux;
		REG_P6_IOC6 = direction;
		REG_P6_P6D = data;
	}
	REG_SPI_INT = interrupts;
	critical_exit(state);
}

static void stop_engines(void)
{
	REG_IDMAEN_DELCDC_DESIF2_DESPI &= ~SPI_IDMA;
	REG_IDMAREQ_RLCDC_RSIF2_RSPI &= ~SPI_IDMA;
	REG_IDMA_EN = 0;
	REG_HS2_EN = 0;
	REG_HS3_EN = 0;
	REG_INT_FDMA = HSDMA2 | HSDMA3;
}

/* One stream of UNITS characters of @width bits into @dst, sent by IDMA
 * if @idma, the CPU running @fn meanwhile.  Returns 0 if it did not
 * complete cleanly. */
static int transfer(unsigned width, int idma, uint32_t dst, loop_fn *fn,
		    unsigned long *ticks, unsigned long *passes,
		    unsigned long *status)
{
	unsigned long size = width == 16 ? 0x4000 : 0;
	unsigned long start, end, stat;
	unsigned count;
	critical_t irq;
	int arrived;

	if (!wait_spi_idle())
		return 0;
	set_spi_control((saved_ctl1 & ~BPT_MASK) | ((width - 1UL) << 10));

	REG_HS_CNTLMODE = HSDMAADV;
	stop_engines();
	REG_HS3_ADVMODE = width == 32;
	REG_HS3_CNT = UNITS;
	REG_HS3_CTRL = 0x8000;
	REG_HS3_SADR_L = 0;
	REG_HS3_SADR_H = size;
	REG_HS3_DADR_L = 0;
	REG_HS3_DADR_H = 0x2000;
	REG_HS3_ADV_SADR_L = SPI_RXD_ADDRESS & 0xffff;
	REG_HS3_ADV_SADR_H = SPI_RXD_ADDRESS >> 16;
	REG_HS3_ADV_DADR_L = dst & 0xffff;
	REG_HS3_ADV_DADR_H = dst >> 16;
	if (idma) {
		IDMA_SLOT[0] = (width == 32 ? 2UL : width == 16 ? 1UL : 0) << 16;
		IDMA_SLOT[1] = UNITS - 1;
		IDMA_SLOT[2] = (uint32_t)tx_source;
		IDMA_SLOT[3] = SPI_TXD_ADDRESS;
		REG_IDMABASE0 = IDMA_TABLE & 0xffff;
		REG_IDMABASE1 = IDMA_TABLE >> 16;
	}
	REG_HS2_ADVMODE = width == 32;
	REG_HS2_CNT = UNITS - 1;
	REG_HS2_CTRL = 0x8000;
	REG_HS2_SADR_L = 0;
	REG_HS2_SADR_H = size;
	REG_HS2_DADR_L = 0;
	REG_HS2_DADR_H = 0;
	REG_HS2_ADV_SADR_L = (uint32_t)tx_source & 0xffff;
	REG_HS2_ADV_SADR_H = (uint32_t)tx_source >> 16;
	REG_HS2_ADV_DADR_L = SPI_TXD_ADDRESS & 0xffff;
	REG_HS2_ADV_DADR_H = SPI_TXD_ADDRESS >> 16;
	REG_HSDMA_HTGR2 = idma ? 0x90 : 0x99;
	REG_INT_FSIF2_FSPI = 0x30;
	REG_HS2_TF = 1;
	REG_HS3_TF = 1;
	REG_INT_FDMA = HSDMA2 | HSDMA3 | IDLE_CHANNEL;
	REG_HS3_EN = DMA_ENABLED;
	if (idma) {
		REG_IDMAREQ_RLCDC_RSIF2_RSPI |= SPI_IDMA;
		REG_IDMAEN_DELCDC_DESIF2_DESPI |= SPI_IDMA;
		REG_IDMA_EN = 1;
	} else {
		REG_HS2_EN = DMA_ENABLED;
	}

	irq = critcal_enter();
	start = timer_get();
	REG_SPI_TXD = 0xffffffffUL;
	*passes = fn(&REG_INT_FDMA, HSDMA3, LIMIT, stream_p,
		     fn == rowmiss ? (uint32_t *)1028 : stream_q);
	end = timer_get();
	critical_exit(irq);

	arrived = (REG_INT_FDMA & HSDMA3) != 0;
	count = REG_HS3_CNT;
	stop_engines();
	if (!wait_spi_idle())
		return 0;
	stat = REG_SPI_STAT;
	if (stat & RDFF)
		(void)REG_SPI_RXD;
	set_spi_control(saved_ctl1);
	*ticks = end - start - timer_pair;
	*status = stat;
	return arrived && count == 0 && !(stat & RDOF);
}

static unsigned long solo_passes = SOLO;

/* @fn's own pace: solo_passes passes with nothing else on the bus. */
static unsigned long solo(loop_fn *fn)
{
	unsigned long start, end, passes;
	critical_t irq = critcal_enter();

	REG_INT_FDMA = IDLE_CHANNEL;
	start = timer_get();
	passes = fn(&REG_INT_FDMA, IDLE_CHANNEL, solo_passes, stream_p,
		    fn == rowmiss ? (uint32_t *)1028 : stream_q);
	end = timer_get();
	critical_exit(irq);
	return passes == solo_passes ? end - start - timer_pair : 0;
}

/* After every case, so that a hang or a reset keeps what came before. */
static void save_log(void)
{
	int h = file_open("spibench.log", FILE_OPEN_WRITE);

	if (h < 0)
		return;
	if (file_lseek(h, log_saved) == FILE_ERROR_OK &&
	    file_write(h, log_text + log_saved, log_used - log_saved) ==
	    (ssize_t)(log_used - log_saved))
		log_saved = log_used;
	file_close(h);
}

static unsigned long median(const unsigned long v[3])
{
	unsigned long lo = v[0] < v[1] ? v[0] : v[1];
	unsigned long hi = v[0] < v[1] ? v[1] : v[0];

	return v[2] < lo ? lo : v[2] > hi ? hi : v[2];
}

static void run_case(unsigned width, int idma, int ivram, const char *name,
		     loop_fn *fn);

/* The Linux kernel runs its hot code from IVRAM switched into area 0 (the
 * LCD controller's IRAM bit, I.6.2), with data such as the CRC tables in
 * A0 RAM.  The same loops, copied from A0 RAM into the IVRAM window and
 * run from there with the switch on, with HSDMA3 writing to SDRAM: the
 * window is not at its usual address meanwhile, and the panel shows
 * nothing it can scan. */
extern char __START_fastcode[], __END_fastcode[];
#define IVRAM_IN_A0	0x2000UL
#define IVRAM_PHYS	0x80000UL

static void ivram_cases(void)
{
	static const struct {
		const char *name;
		loop_fn *fn;
	} iv[] = {
		{ "spin-iv", spin }, { "load1-iv", load1 },
		{ "store1-iv", store1 }, { "moves-iv", moves_a0 },
		{ "lookup4-iv", lookup4 }, { "crc-iv", crc },
	};
	static const unsigned iv_widths[] = { 32, 8 };
	unsigned long size = __END_fastcode - __START_fastcode;
	uintptr_t copy = (uintptr_t)WINDOW - IVRAM_PHYS + IVRAM_IN_A0;

	if (size > WINDOW_SIZE) {
		out("ABORT: .fastcode does not fit the IVRAM window\n");
		failures++;
		return;
	}
	uint32_t saved_ones = *DSTRAM_ONES;

	memcpy(WINDOW, __START_fastcode, size);
	*DSTRAM_ONES = 0xffffffffUL;
	tx_source = DSTRAM_ONES;
	REG_LCDC_IRAM |= 1;
	for (unsigned w = 0; w < sizeof(iv_widths) / sizeof(*iv_widths); w++)
		for (unsigned l = 0; l < sizeof(iv) / sizeof(*iv); l++)
			run_case(iv_widths[w], 0, 0, iv[l].name, (loop_fn *)
				 (copy + ((char *)iv[l].fn - __START_fastcode)));
	REG_LCDC_IRAM &= ~1UL;
	tx_source = DUMMY;
	*DSTRAM_ONES = saved_ones;
}

static void run_case(unsigned width, int idma, int ivram, const char *name,
		     loop_fn *fn)
{
	uint32_t dst = ivram ? (uint32_t)WINDOW : (uint32_t)rx_sdram;
	unsigned long ticks[REPEATS], passes[REPEATS], status[REPEATS];
	unsigned long alone = solo(fn);
	unsigned long t, p;
	int ok = 1;

	for (unsigned r = 0; r < REPEATS; r++) {
		watchdog(WATCHDOG_KEY);
		if (!transfer(width, idma, dst, fn, &ticks[r],
			      &passes[r], &status[r]))
			ok = 0;
	}
	t = median(ticks);
	p = median(passes);
	cases++;
	if (!ok)
		failures++;
	/* Cycles a unit, and the CPU's pace beside the DMA against alone:
	   cycles a pass, x100. */
	out("RESULT tx=%s width=%u dst=%s cpu=%s units=%u ticks=%lu,%lu,%lu "
	    "passes=%lu,%lu,%lu stat=%02lx,%02lx,%02lx solo=%lu/%lu "
	    "unit_x100=%lu pass_x100=%lu solo_pass_x100=%lu ok=%d\n",
	    idma ? "idma" : "hsdma", width, ivram ? "ivram" : "sdram",
	    name, UNITS,
	    ticks[0], ticks[1], ticks[2], passes[0], passes[1], passes[2],
	    status[0], status[1], status[2], alone, solo_passes,
	    t * 100 / UNITS, p ? t * 100 / p : 0, alone * 100 / solo_passes, ok);
	save_log();
	lcd_at_xy(0, 3);
	lcd_printf("case %u, %u failed  ", cases, failures);
}

int grifo_main(int argc, char **argv)
{
	int power = argc > 1 && strcmp(argv[1], "off") == 0;
	uint8_t *allocation;
	uintptr_t base;
	event_t event;

	lcd_clear(LCD_WHITE);
	lcd_at_xy(0, 0);
	lcd_print("SPI DMA benchmark\nabout ten seconds...\n");
	{
		int h = file_create("spibench.log", FILE_OPEN_WRITE);

		if (h >= 0)
			file_close(h);
	}
	out("SPIBENCH v1 build=%s %s units=%u repeats=%u solo=%lu\n",
	    __DATE__, __TIME__, UNITS, REPEATS, SOLO);
	if ((REG_HS0_EN | REG_HS1_EN | REG_HS2_EN | REG_HS3_EN) & 1) {
		out("ABORT: DMA channel already active\n");
		goto finish;
	}
	allocation = memory_allocate(8 * REGION, "spibench");
	if (!allocation) {
		out("ABORT: no memory\n");
		goto finish;
	}
	base = ((uintptr_t)allocation + 2 * REGION - 1) & ~(2 * REGION - 1);
	stream_p = (uint32_t *)base;
	stream_q = (uint32_t *)(base + 2 * REGION);
	rx_sdram = (uint32_t *)(base + 4 * REGION);

	saved_ctl1 = REG_SPI_CTL1;
	saved_mode = REG_HS_CNTLMODE;
	saved_select = REG_HSDMA_HTGR2;
	saved_cs = REG_P5_P5D;
	saved_base0 = REG_IDMABASE0;
	saved_base1 = REG_IDMABASE1;
	for (unsigned i = 0; i < 4; i++)
		saved_slot[i] = IDMA_SLOT[i];
	memcpy(saved_window, WINDOW, WINDOW_SIZE);
	lcd_window_disable();
	REG_P5_P5D = saved_cs | 1;	/* the card ignores what follows */
	*DUMMY = 0xffffffffUL;
	crc_tables_init();
	{
		critical_t irq = critcal_enter();
		unsigned long before = timer_get();
		timer_pair = timer_get() - before;
		critical_exit(irq);
	}
	out("CONFIG spi_ctl1=%08lx spi_wait=%08lx sdram_ctl=%08lx "
	    "sdram_ref=%08lx sdram_app=%08lx clkcntl=%08lx gate=%08lx "
	    "acctime=%04x timer_pair=%lu p=%08lx q=%08lx rx=%08lx ivram=%08lx\n",
	    saved_ctl1, (unsigned long)REG_SPI_WAIT,
	    (unsigned long)REG_SDRAMC_CTL, (unsigned long)REG_SDRAMC_REF,
	    (unsigned long)REG_SDRAMC_APP, (unsigned long)REG_CMU_CLKCNTL,
	    (unsigned long)REG_CMU_GATEDCLK1, (unsigned)REG_HS_ACCTIME,
	    timer_pair, (unsigned long)stream_p, (unsigned long)stream_q,
	    (unsigned long)rx_sdram, (unsigned long)WINDOW);

	for (unsigned w = 0; w < sizeof(widths) / sizeof(*widths); w++)
		for (int ivram = 0; ivram < 2; ivram++)
			for (unsigned l = 0; l < sizeof(loops) / sizeof(*loops); l++)
				run_case(widths[w], 0, ivram, loops[l].name,
					 loops[l].fn);
	ivram_cases();
	solo_passes = SOLO_BLOCKS;
	for (unsigned w = 0; w < 3; w += 2) {
		run_case(widths[w], 0, 0, "unpack0", unpack0);
		run_case(widths[w], 0, 0, "unpack2", unpack2);
	}
	solo_passes = SOLO;
	/* Receive-paced: each character waits for both engines. */
	for (unsigned w = 0; w < sizeof(widths) / sizeof(*widths); w++)
		for (unsigned l = 0; widths[w] != 16 &&
				  l < sizeof(loops) / sizeof(*loops); l++)
			run_case(widths[w], 1, 0, loops[l].name,
				 loops[l].fn);

	for (unsigned i = 0; i < 4; i++)
		IDMA_SLOT[i] = saved_slot[i];
	REG_IDMABASE0 = saved_base0;
	REG_IDMABASE1 = saved_base1;
	REG_P5_P5D = saved_cs;
	REG_HSDMA_HTGR2 = saved_select;
	REG_HS_CNTLMODE = saved_mode;
	memcpy(WINDOW, saved_window, WINDOW_SIZE);
	memory_free(allocation, "spibench");
	out("END SPIBENCH cases=%u failures=%u\n", cases, failures);
finish:
	save_log();
	lcd_clear(LCD_WHITE);
	lcd_at_xy(0, 0);
	lcd_printf("SPI DMA benchmark done.\n%u cases, %u failed.\n"
		   "Results in spibench.log.\nTap to return.\n", cases, failures);
	if (power)
		power_off();
	event_flush();
	for (;;) {
		event_wait(&event, NULL, NULL);
		if (event.item_type == EVENT_TOUCH_DOWN ||
		    event.item_type == EVENT_BUTTON_DOWN)
			break;
	}
	return 0;
}
