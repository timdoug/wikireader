/*
 * Interrupt priority (PSR.IL) unit test.
 *
 * "Maskable interrupt requests are accepted only when their priority levels
 * are higher than that set in the IL bit field. When an interrupt request is
 * accepted, the IL bit field is set to the priority level of that interrupt."
 *   -- C33 PE Core manual, 2.3.1
 *
 * Driven through the public API only: raise an interrupt, step once, and see
 * whether it was accepted. Note that one c33_step both takes the interrupt
 * and executes the handler's first instruction, so acceptance is read from
 * irqs_taken rather than from the PC.
 */

#include <stdio.h>
#include <string.h>

#include "../src/c33.h"
#include "../src/itc.h"

#define TTBR    0x400u
#define VECTOR  61u
#define NMI_VECTOR 7u
#define HANDLER 0x9000u
#define ENTRY   0x1000u

static uint8_t ram[0x20000];

static uint32_t tr(void *ctx, uint32_t a, unsigned sz)
{
	uint32_t v = 0;
	for (unsigned i = 0; i < sz; i++)
		v |= (uint32_t)ram[(a + i) & 0x1ffff] << (8 * i);
	return v;
}

static void tw(void *ctx, uint32_t a, unsigned sz, uint32_t v)
{
	for (unsigned i = 0; i < sz; i++)
		ram[(a + i) & 0x1ffff] = (uint8_t)(v >> (8 * i));
}

static int fails;

static void check(const char *what, int got, int want)
{
	printf("%-58s %s\n", what, got == want ? "ok" : "FAIL");
	if (got != want) {
		printf("    got %d, wanted %d\n", got, want);
		fails++;
	}
}

/* Run one step with IL preset to il and an interrupt pending at prio. */
static struct c33 run(unsigned il, unsigned prio)
{
	struct c33 c;
	memset(&c, 0, sizeof c);
	c.bus.read = tr;
	c.bus.write = tw;
	c33_reset(&c, ENTRY);
	c.sr[SR_TTBR] = TTBR;
	c.sr[SR_SP] = 0x10000;
	c.sr[SR_PSR] = PSR_IE | (il << PSR_IL_SHIFT);
	tw(NULL, TTBR + VECTOR * 4, 4, HANDLER);
	tw(NULL, ENTRY, 2, 0x0000);          /* nop */
	c33_raise_irq(&c, VECTOR, prio);
	c33_step(&c);
	return c;
}

int main(void)
{
	char buf[128];
	struct itc itc = {0};
	struct mem mem;

	/* Each source must use its own documented priority field.  Distinct
	 * values keep coincidentally equal firmware settings from hiding swaps. */
	itc.reg[0x261 - ITC_BASE] = 5u << 4; /* port input 3 */
	itc.reg[0x262 - ITC_BASE] = 3u;      /* key input 0 */
	itc.reg[0x263 - ITC_BASE] = (2u << 4) | 1u; /* HSDMA 0/1 */
	itc.reg[0x264 - ITC_BASE] = (4u << 4) | 3u; /* HSDMA 2/3 */
	itc.reg[0x266 - ITC_BASE] = (2u << 4) | 1u; /* timers 0/1 */
	itc.reg[0x267 - ITC_BASE] = (4u << 4) | 3u; /* timers 2/3 */
	itc.reg[0x268 - ITC_BASE] = (6u << 4) | 5u; /* timers 4/5 */
	itc.reg[0x26a - ITC_BASE] = (4u << 4) | 6u;
	check("port input 3 reads PP23L[6:4]", itc_priority(&itc, 19), 5);
	check("key input 0 reads PK01L[2:0]", itc_priority(&itc, 20), 3);
	for (unsigned channel = 0; channel < 4; channel++) {
		snprintf(buf, sizeof buf, "HSDMA %u uses its priority field", channel);
		check(buf, itc_priority(&itc, 22 + channel), channel + 1);
	}
	check("timer 2 compare B reads P16T23[2:0]",
	      itc_priority(&itc, 38), 3);
	check("timer 2 compare A shares P16T23[2:0]",
	      itc_priority(&itc, 39), 3);
	for (unsigned channel = 0; channel < 6; channel++) {
		unsigned b = 30 + 4 * channel;
		snprintf(buf, sizeof buf, "timer %u uses its documented priority field",
			 channel);
		check(buf, itc_priority(&itc, b), channel + 1);
		check("comparison A shares its channel's priority",
		      itc_priority(&itc, b + 1), channel + 1);
	}
	check("serial 0 reads PSI01_PAD[6:4]", itc_priority(&itc, 57), 4);
	check("serial 1 reads PSI01_PAD[2:0]", itc_priority(&itc, 61), 6);

	/* HSDMA channels use adjacent cause and enable bits. */
	for (unsigned channel = 0; channel < 4; channel++) {
		itc_reset(&itc);
		itc_set_flag(&itc, 22 + channel);
		snprintf(buf, sizeof buf, "HSDMA %u sets its completion cause", channel);
		check(buf, itc.reg[0x281 - ITC_BASE], 1u << channel);
		itc.reg[0x271 - ITC_BASE] = 1u << channel;
		check("HSDMA observes its interrupt enable bit",
		      itc_enabled(&itc, 22 + channel), 1);
	}

	/* Serial causes have distinct error/rx/tx bits on each channel. */
	static const unsigned serial_vectors[] = { 56, 57, 58, 60, 61, 62 };
	static const unsigned serial_bits[] = { 0, 1, 2, 3, 4, 5 };
	for (unsigned i = 0; i < sizeof serial_vectors / sizeof serial_vectors[0];
	     i++) {
		itc_reset(&itc);
		itc_set_flag(&itc, serial_vectors[i]);
		snprintf(buf, sizeof buf, "serial vector %u sets cause bit %u",
			 serial_vectors[i], serial_bits[i]);
		check(buf, itc.reg[0x286 - ITC_BASE], 1u << serial_bits[i]);
	}

	/* Every timer channel has adjacent B/A cause and enable bits. */
	for (unsigned channel = 0; channel < 6; channel++) {
		unsigned b = 30 + 4 * channel;
		unsigned reg = 0x282 + channel / 2;
		unsigned bit = 2 + 4 * (channel & 1);
		itc_reset(&itc);
		itc_set_flag(&itc, b);
		snprintf(buf, sizeof buf, "timer %u compare B sets its cause bit",
			 channel);
		check(buf, itc.reg[reg - ITC_BASE], 1u << bit);
		itc_set_flag(&itc, b + 1);
		snprintf(buf, sizeof buf, "timer %u compare A sets the adjacent bit",
			 channel);
		check(buf, itc.reg[reg - ITC_BASE], 3u << bit);
		itc.reg[reg - 0x10 - ITC_BASE] = 1u << bit;
		check("comparison B observes its enable bit", itc_enabled(&itc, b), 1);
		check("comparison A has a distinct enable bit",
		      itc_enabled(&itc, b + 1), 0);
		itc.reg[reg - 0x10 - ITC_BASE] |= 1u << (bit + 1);
		check("comparison A observes its enable bit",
		      itc_enabled(&itc, b + 1), 1);
	}

	/* Cause-register writes follow the reset mode selected at 0x30029f. */
	if (!mem_init(&mem)) {
		fprintf(stderr, "cannot allocate memory for ITC MMIO tests\n");
		return 1;
	}
	itc_attach(&mem, &itc);
	check("interrupt mode register resets to DEN/IDMA/RST-only",
	      mem_read(&mem, REG_BASE + 0x29f, 1), 0x07);
	itc_set_flag(&itc, 61);
	mem_write(&mem, REG_BASE + 0x286, 1, 0);
	check("zero leaves a cause set in reset-only mode",
	      mem_read(&mem, REG_BASE + 0x286, 1), 1u << 4);
	mem_write(&mem, REG_BASE + 0x286, 1, 1u << 4);
	check("one clears a cause in reset-only mode",
	      mem_read(&mem, REG_BASE + 0x286, 1), 0);
	mem_write(&mem, REG_BASE + 0x29f, 1, 0); /* ordinary read/write mode */
	mem_write(&mem, REG_BASE + 0x286, 1, 0x12);
	check("cause flags are writable when RSTONLY is clear",
	      mem_read(&mem, REG_BASE + 0x286, 1), 0x12);

	/* Simultaneous causes are retained and arbitrated, not overwritten. */
	itc_reset(&itc);
	mem_write(&mem, REG_BASE + 0x273, 1, 1u << 2); /* timer 2 cmp B */
	mem_write(&mem, REG_BASE + 0x276, 1, 1u << 4); /* serial 1 rx */
	mem_write(&mem, REG_BASE + 0x267, 1, 5);       /* timer priority 5 */
	mem_write(&mem, REG_BASE + 0x26a, 1, 3);       /* serial priority 3 */
	itc_set_flag(&itc, 61);
	itc_set_flag(&itc, 38);
	unsigned vector = 0, priority = 0;
	check("higher-priority simultaneous cause wins arbitration",
	      itc_next_irq(&itc, &vector, &priority) && vector == 38 &&
	      priority == 5, 1);
	mem_write(&mem, REG_BASE + 0x283, 1, 1u << 2);
	check("lower-priority cause remains pending afterward",
	      itc_next_irq(&itc, &vector, &priority) && vector == 61 &&
	      priority == 3, 1);
	mem_write(&mem, REG_BASE + 0x267, 1, 3);       /* equal priorities */
	itc_set_flag(&itc, 38);
	check("documented fixed order breaks equal-priority ties",
	      itc_next_irq(&itc, &vector, &priority) && vector == 38, 1);
	mem_free(&mem);

	/* Acceptance is strictly greater-than, at every boundary. */
	for (unsigned il = 0; il <= 7; il++) {
		for (unsigned prio = 0; prio <= 7; prio++) {
			struct c33 c = run(il, prio);
			snprintf(buf, sizeof buf,
				 "IL=%u prio=%u %s", il, prio,
				 prio > il ? "accepted" : "masked");
			check(buf, c.irqs_taken == 1, prio > il);
		}
	}

	/* An accepted interrupt raises IL to its own level and clears IE. */
	struct c33 c = run(2, 5);
	check("accepted interrupt sets IL to its own priority",
	      (c.sr[SR_PSR] & PSR_IL_MASK) >> PSR_IL_SHIFT, 5);
	check("accepted interrupt clears IE", !(c.sr[SR_PSR] & PSR_IE), 1);

	/* The saved PSR carries the old IL and IE for reti to restore. */
	check("saved PSR on the stack keeps the old IL",
	      (tr(NULL, c.sr[SR_SP], 4) & PSR_IL_MASK) >> PSR_IL_SHIFT, 2);
	check("saved PSR on the stack keeps IE set",
	      (tr(NULL, c.sr[SR_SP], 4) & PSR_IE) != 0, 1);

	/* A masked request is not lost: it stays pending. */
	c = run(5, 3);
	check("masked request stays pending", c.irq_pending, 1);
	check("masked request does not run the handler", c.irqs_taken == 0, 1);

	/*
	 * The interrupted instruction has not run yet: the pushed return
	 * address is its own PC, so reti re-executes it.
	 */
	c = run(0, 7);
	check("return address is the un-executed instruction",
	      tr(NULL, c.sr[SR_SP] + 4, 4) == ENTRY, 1);
	check("handler runs from the vector",
	      c.pc == HANDLER + 2, 1);

	/* IE=0 blocks regardless of priority. */
	struct c33 d;
	memset(&d, 0, sizeof d);
	d.bus.read = tr; d.bus.write = tw;
	c33_reset(&d, ENTRY);
	d.sr[SR_TTBR] = TTBR;
	d.sr[SR_SP] = 0x10000;
	d.sr[SR_PSR] = 0;                    /* IE clear, IL 0 */
	c33_raise_irq(&d, VECTOR, 7);
	c33_step(&d);
	check("IE=0 blocks even the highest priority", d.irqs_taken == 0, 1);

	/* An enabled HSDMA cause releases HALT even while PSR.IE is clear. */
	memset(&d, 0, sizeof d);
	d.bus.read = tr; d.bus.write = tw;
	c33_reset(&d, ENTRY);
	d.sr[SR_TTBR] = TTBR;
	d.sr[SR_SP] = 0x10000;
	d.sr[SR_PSR] = 0;
	itc_reset(&itc);
	itc.reg[0x264 - ITC_BASE] = 7u << 4;
	itc.reg[0x271 - ITC_BASE] = 1u << 3;
	d.irq_poll = (bool (*)(void *, unsigned *, unsigned *))itc_next_irq;
	d.irq_ctx = &itc;
	tw(NULL, ENTRY, 2, 0x0080);          /* halt */
	tw(NULL, ENTRY + 2, 2, 0x0000);      /* nop */
	c33_step(&d);
	check("HALT sleeps before DMA completion", d.sleeping, 1);
	itc_set_flag(&itc, 25);
	c33_step(&d);
	check("HSDMA completion releases HALT with IE clear", d.sleeping, 0);
	check("IE-clear wake does not enter the DMA vector", d.irqs_taken, 0);
	check("wake resumes at the instruction after HALT", d.pc, ENTRY + 2);

	/*
	 * An interrupt must not be taken part-way through an ext sequence.
	 * "exception handling ... is not started for other exceptions until
	 * after the target instruction to be extended is executed"
	 * (C33 PE Core manual, 5.6.3).
	 *
	 * The regression: grifo's syscall return does
	 *     ext 0x200 ; ext 0x353 ; ld.w %r0,0x2c
	 * which composes 0x1000d4ec. A touch interrupt landing between the
	 * two prefixes dropped the first one, the load came from 0xd4ec,
	 * read as zero, and an indirect ret went to address 0.
	 */
	{
		struct c33 e;
		memset(&e, 0, sizeof e);
		e.bus.read = tr; e.bus.write = tw;
		c33_reset(&e, ENTRY);
		e.sr[SR_TTBR] = TTBR;
		e.sr[SR_SP] = 0x10000;
		e.sr[SR_PSR] = PSR_IE;
		tw(NULL, TTBR + VECTOR * 4, 4, HANDLER);

		/* ext 0x200 ; ext 0x353 ; ld.w %r0,0x2c */
		tw(NULL, ENTRY + 0, 2, 0xc200);
		tw(NULL, ENTRY + 2, 2, 0xc353);
		tw(NULL, ENTRY + 4, 2, 0x6ec0);

		c33_step(&e);                     /* first ext */
		check("one ext pending after the first prefix", e.n_ext, 1);

		/* Interrupt arrives mid-sequence, at the worst moment. */
		c33_raise_irq(&e, VECTOR, 7);
		c33_step(&e);                     /* second ext */
		check("interrupt deferred during an ext sequence",
		      e.irqs_taken, 0);
		check("it stays pending", e.irq_pending, 1);
		check("both prefixes survive", e.n_ext, 2);

		c33_step(&e);                     /* the target instruction */
		check("target composed the full 32-bit immediate",
		      e.r[0] == 0x1000d4ec, 1);
		check("prefix state cleared after the target", e.n_ext, 0);

		c33_step(&e);                     /* now the interrupt lands */
		check("interrupt taken once the sequence completed",
		      e.irqs_taken, 1);
	}

	/*
	 * No interrupt is accepted between a delayed branch and its slot,
	 * whether or not a conditional branch is taken (Core Manual 5.14.2).
	 */
	for (unsigned taken = 0; taken <= 1; taken++) {
		struct c33 e;
		memset(&e, 0, sizeof e);
		e.bus.read = tr; e.bus.write = tw;
		c33_reset(&e, ENTRY);
		e.sr[SR_TTBR] = TTBR;
		e.sr[SR_SP] = 0x10000;
		e.sr[SR_PSR] = PSR_IE | (taken ? PSR_Z : 0);
		tw(NULL, TTBR + VECTOR * 4, 4, HANDLER);

		/* jreq.d +8 ; add %r0,1 ; nop ; nop */
		tw(NULL, ENTRY + 0, 2, 0x1904);
		tw(NULL, ENTRY + 2, 2, 0x6010);
		tw(NULL, ENTRY + 4, 2, 0x0000);
		tw(NULL, ENTRY + 8, 2, 0x0000);

		c33_step(&e);                     /* delayed branch */
		snprintf(buf, sizeof buf, "%s delayed branch opens a slot",
			 taken ? "taken" : "untaken");
		check(buf, e.delay_pending, 1);

		c33_raise_irq(&e, VECTOR, 7);
		c33_step(&e);                     /* protected slot */
		snprintf(buf, sizeof buf, "%s delayed slot executes before IRQ",
			 taken ? "taken" : "untaken");
		check(buf, e.r[0], 1);
		snprintf(buf, sizeof buf, "%s delayed slot defers IRQ",
			 taken ? "taken" : "untaken");
		check(buf, e.irqs_taken, 0);
		snprintf(buf, sizeof buf, "%s delayed branch selects next PC",
			 taken ? "taken" : "untaken");
		check(buf, e.pc, taken ? ENTRY + 8 : ENTRY + 4);

		c33_step(&e);                     /* interrupt may now land */
		snprintf(buf, sizeof buf, "%s branch accepts IRQ after slot",
			 taken ? "taken" : "untaken");
		check(buf, e.irqs_taken, 1);
	}

	/* NMI has its own acceptance rules (Core Manual 6.3.6). */
	{
		struct c33 e;
		memset(&e, 0, sizeof e);
		e.bus.read = tr; e.bus.write = tw;
		c33_reset(&e, ENTRY);
		e.sr[SR_TTBR] = TTBR;
		tw(NULL, TTBR + NMI_VECTOR * 4, 4, HANDLER);
		tw(NULL, ENTRY, 2, 0x0000);
		c33_raise_nmi(&e);
		c33_step(&e);
		check("NMI is masked until SP has been initialized", e.nmis_taken, 0);

		/* ld.w %sp,%r0 is the architectural event that unmasks NMI. */
		tw(NULL, e.pc, 2, 0xa001);
		e.r[0] = 0x10000;
		c33_step(&e);
		check("loading SP enables NMI acceptance", e.sp_initialized, 1);
		e.sr[SR_PSR] = 15u << PSR_IL_SHIFT; /* IE clear, all IRQ levels masked */
		uint32_t resume = e.pc;
		c33_raise_nmi(&e);
		c33_step(&e);
		check("NMI ignores IE and IL", e.nmis_taken, 1);
		check("NMI does not rewrite IL",
		      (e.sr[SR_PSR] & PSR_IL_MASK) >> PSR_IL_SHIFT, 15);
		check("NMI saves the unexecuted instruction",
		      tr(NULL, e.sr[SR_SP] + 4, 4) == resume, 1);
		check("NMI branches through vector 7", e.pc == HANDLER + 2, 1);

		c33_raise_nmi(&e);
		check("a second NMI is suppressed during NMI handling",
		      e.nmi_pending, 0);
		tw(NULL, e.pc, 2, 0x04c0);          /* reti */
		c33_step(&e);
		check("reti unmasks NMI", e.nmi_active, 0);
		c33_raise_nmi(&e);
		check("NMI can be requested again after reti", e.nmi_pending, 1);
	}

	printf("\n%s\n", fails ? "FAILURES" : "all interrupt tests passed");
	return fails != 0;
}
