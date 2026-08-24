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

#define TTBR    0x400u
#define VECTOR  61u
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

	printf("\n%s\n", fails ? "FAILURES" : "all interrupt tests passed");
	return fails != 0;
}
