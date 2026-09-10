/*
 * Interrupt controller.
 *
 * Only the register file plus a priority lookup: the priority registers are
 * read-modify-written by the drivers (grifo's CTP_initialise does
 * REG_INT_PSI01_PAD |= SERIAL_CH1_INT_PRI_7), so they have to read back what
 * was written or the surrounding fields are lost.
 *
 * Priorities feed the PSR's IL field. Per the C33 PE Core manual, "maskable
 * interrupt requests are accepted only when their priority levels are higher
 * than that set in the IL bit field", and IL is then set to the accepted
 * interrupt's level until reti restores the saved PSR.
 */

#include <string.h>

#include "itc.h"

/* Priority register offsets within the block, from samo-lib/include/regs.h. */
#define PP23L      (0x261u - ITC_BASE)   /* port input 2 low, input 3 high */
#define PK01L      (0x262u - ITC_BASE)   /* key input 0 low, input 1 high */
#define PHSD01L    (0x263u - ITC_BASE)   /* HSDMA 0 low, HSDMA 1 high */
#define PHSD23L    (0x264u - ITC_BASE)   /* HSDMA 2 low, HSDMA 3 high */
#define P16T01     (0x266u - ITC_BASE)   /* timer 0 low, timer 1 high */
#define P16T23     (0x267u - ITC_BASE)   /* timer 2 low, timer 3 high */
#define P16T45     (0x268u - ITC_BASE)   /* timer 4 low, timer 5 high */
#define PSI01_PAD  (0x26au - ITC_BASE)   /* serial ch0 bits 6:4, ch1 bits 2:0 */

/*
 * Cause-of-interrupt flag registers, 0x280..0x28f.
 *
 * RSTONLY selects whether these are write-1-to-clear or plain storage.
 * It resets set, and samo-lib describes that mode as "1 => reset flag bit".
 *
 * Getting this wrong is not subtle. grifo's resume path decides why it
 * woke by reading the timer 2 flags, and it clears them before halting
 * with |=. Treated as storage, that write sets them, so on resume the
 * firmware always concludes it timed out and calls System_PowerOff().
 */
#define FLAG_LO    (0x280u - ITC_BASE)
#define FLAG_HI    (0x290u - ITC_BASE)
#define RST_RESET  (0x29fu - ITC_BASE)
#define RSTONLY    (1u << 0)

#define F16T01     (0x282u - ITC_BASE)   /* 16-bit timer 0-1 causes */
#define F16T23     (0x283u - ITC_BASE)   /* 16-bit timer 2-3 causes */
#define F16T45     (0x284u - ITC_BASE)   /* 16-bit timer 4-5 causes */
#define FSIF01     (0x286u - ITC_BASE)   /* serial ch0-1 causes */
#define FDMA       (0x281u - ITC_BASE)   /* HSDMA 0-3 and IDMA causes */

/* Enable registers, one bit per cause. */
#define EK01_EP03  (0x270u - ITC_BASE)   /* key input and port causes */
#define EDMA       (0x271u - ITC_BASE)   /* HSDMA 0-3 and IDMA enables */
#define FK01_FP03  (0x280u - ITC_BASE)
#define E16T01     (0x272u - ITC_BASE)
#define E16T23     (0x273u - ITC_BASE)
#define E16T45     (0x274u - ITC_BASE)
#define ESIF01     (0x276u - ITC_BASE)

/* Vector numbers from samo-lib/grifo/src/vector.h. */
#define VEC_SERIAL0_ERR   56
#define VEC_SERIAL0_RX    57
#define VEC_SERIAL0_TX    58
#define VEC_SERIAL1_ERR   60
#define VEC_SERIAL1_RX    61
#define VEC_SERIAL1_TX    62

static bool itc_mmio(void *ctx, uint32_t off, unsigned size, uint32_t *val,
		     bool is_write)
{
	struct itc *t = ctx;
	uint32_t i = off - ITC_BASE;

	if (i + size > ITC_LEN)
		return false;

	if (is_write) {
		for (unsigned k = 0; k < size; k++) {
			uint32_t a = i + k;
			uint8_t v = (uint8_t)(*val >> (8 * k));
			/* II.1.5: HST[3:0] issue software requests and read as zero. */
			if (a == 0x9au) {
				if (t->hsdma_trigger)
					t->hsdma_trigger(t->hsdma_ctx, v & 15u);
				continue;
			}
			if (a >= FLAG_LO && a < FLAG_HI &&
			    (t->reg[RST_RESET] & RSTONLY))
				t->reg[a] &= (uint8_t)~v;   /* write 1 to clear */
			else
				t->reg[a] = v;
		}
		t->writes++;
		return true;
	}
	*val = 0;
	for (unsigned k = 0; k < size; k++)
		*val |= (uint32_t)t->reg[i + k] << (8 * k);
	return true;
}

unsigned itc_priority(const struct itc *t, unsigned vector)
{
	if (vector >= 22 && vector <= 25) {
		unsigned channel = vector - 22;
		unsigned reg = channel < 2 ? PHSD01L : PHSD23L;
		return (t->reg[reg] >> (channel & 1u ? 4 : 0)) & 0x7;
	}
	if (vector >= 30 && vector <= 51 &&
	    ((vector - 30) % 4u) < 2u) {
		unsigned channel = (vector - 30) / 4u;
		static const uint8_t preg[] = { P16T01, P16T23, P16T45 };
		return (t->reg[preg[channel / 2]] >> (channel & 1u ? 4 : 0)) & 0x7;
	}
	switch (vector) {
	case 19:
		return (t->reg[PP23L] >> 4) & 0x7; /* port input 3 */
	case 20:
		return t->reg[PK01L] & 0x7;        /* key input 0 */
	case VEC_SERIAL0_ERR:
	case VEC_SERIAL0_RX:
	case VEC_SERIAL0_TX:
		return (t->reg[PSI01_PAD] >> 4) & 0x7;
	case VEC_SERIAL1_ERR:
	case VEC_SERIAL1_RX:
	case VEC_SERIAL1_TX:
		return t->reg[PSI01_PAD] & 0x7;
	default:
		/*
		 * Sources whose priority register is not decoded here. Report
		 * the maximum so they are never masked, which matches the
		 * previous behaviour of ignoring IL entirely.
		 */
		return 7;
	}
}

/*
 * Which enable bit gates a vector. Returning true for anything not listed
 * keeps sources this does not decode behaving as they always have.
 */
bool itc_enabled(const struct itc *t, unsigned vector)
{
	if (vector >= 22 && vector <= 25)
		return (t->reg[EDMA] & (1u << (vector - 22))) != 0;
	if (vector >= 30 && vector <= 51 &&
	    ((vector - 30) % 4u) < 2u) {
		unsigned channel = (vector - 30) / 4u;
		unsigned is_a = (vector - 30) & 1u;
		static const uint8_t ereg[] = { E16T01, E16T23, E16T45 };
		return (t->reg[ereg[channel / 2]] &
			(1u << (2u + 4u * (channel & 1u) + is_a))) != 0;
	}
	switch (vector) {
	case 19: return (t->reg[EK01_EP03] & (1u << 3)) != 0; /* port input 3 */
	case 20: return (t->reg[EK01_EP03] & (1u << 4)) != 0; /* key input 0 */
	case 56: return (t->reg[ESIF01] & (1u << 0)) != 0;  /* serial 0 error */
	case 57: return (t->reg[ESIF01] & (1u << 1)) != 0;  /* serial 0 rx    */
	case 58: return (t->reg[ESIF01] & (1u << 2)) != 0;  /* serial 0 tx    */
	case 60: return (t->reg[ESIF01] & (1u << 3)) != 0;  /* serial 1 error */
	case 61: return (t->reg[ESIF01] & (1u << 4)) != 0;  /* serial 1 rx    */
	case 62: return (t->reg[ESIF01] & (1u << 5)) != 0;  /* serial 1 tx    */
	default: return true;
	}
}

static bool itc_flagged(const struct itc *t, unsigned vector)
{
	if (vector >= 22 && vector <= 25)
		return (t->reg[FDMA] & (1u << (vector - 22))) != 0;
	if (vector >= 30 && vector <= 51 &&
	    ((vector - 30) % 4u) < 2u) {
		unsigned channel = (vector - 30) / 4u;
		unsigned is_a = (vector - 30) & 1u;
		static const uint8_t freg[] = { F16T01, F16T23, F16T45 };
		return (t->reg[freg[channel / 2]] &
			(1u << (2u + 4u * (channel & 1u) + is_a))) != 0;
	}
	switch (vector) {
	case 19: return (t->reg[FK01_FP03] & (1u << 3)) != 0;
	case 20: return (t->reg[FK01_FP03] & (1u << 4)) != 0;
	case 56: return (t->reg[FSIF01] & (1u << 0)) != 0;
	case 57: return (t->reg[FSIF01] & (1u << 1)) != 0;
	case 58: return (t->reg[FSIF01] & (1u << 2)) != 0;
	case 60: return (t->reg[FSIF01] & (1u << 3)) != 0;
	case 61: return (t->reg[FSIF01] & (1u << 4)) != 0;
	case 62: return (t->reg[FSIF01] & (1u << 5)) != 0;
	default: return false;
	}
}

bool itc_next_irq(const struct itc *t, unsigned *vector, unsigned *priority)
{
	/* Most instructions have no enabled cause pending. Reject that case
	 * using the six flag/enable groups before walking individual vectors.
	 * Read the registers directly so MMIO writes, reasserted level sources,
	 * and reset take effect immediately without a cached arbitration result. */
	if (!((t->reg[FK01_FP03] & t->reg[EK01_EP03] & 0x18u) |
	      (t->reg[FDMA] & t->reg[EDMA] & 0x0fu) |
	      (t->reg[F16T01] & t->reg[E16T01] & 0xccu) |
	      (t->reg[F16T23] & t->reg[E16T23] & 0xccu) |
	      (t->reg[F16T45] & t->reg[E16T45] & 0xccu) |
	      (t->reg[FSIF01] & t->reg[ESIF01] & 0x3fu)))
		return false;

	/* Table III.2.1.1.1 is ordered from highest to lowest fixed priority.
	 * Keeping the first vector on a tie implements that documented order. */
	static const uint8_t vectors[] = {
		19, 20, 22, 23, 24, 25,
		30, 31, 34, 35, 38, 39, 42, 43, 46, 47, 50, 51,
		56, 57, 58, 60, 61, 62
	};
	bool found = false;
	unsigned best = 0;

	for (unsigned i = 0; i < sizeof vectors / sizeof vectors[0]; i++) {
		unsigned v = vectors[i];
		unsigned p;
		if (!itc_flagged(t, v) || !itc_enabled(t, v))
			continue;
		p = itc_priority(t, v);
		if (p == 0 || (found && p <= best))
			continue;
		found = true;
		best = p;
		*vector = v;
		*priority = p;
	}
	return found;
}

void itc_set_flag(struct itc *t, unsigned vector)
{
	if (vector >= 22 && vector <= 25) {
		t->reg[FDMA] |= 1u << (vector - 22);
		return;
	}
	if (vector >= 30 && vector <= 51 &&
	    ((vector - 30) % 4u) < 2u) {
		unsigned channel = (vector - 30) / 4u;
		unsigned is_a = (vector - 30) & 1u;
		static const uint8_t freg[] = { F16T01, F16T23, F16T45 };
		t->reg[freg[channel / 2]] |=
			1u << (2u + 4u * (channel & 1u) + is_a);
		return;
	}
	switch (vector) {
	case 19: t->reg[FK01_FP03] |= 1u << 3; break;  /* port input 3 */
	case 20: t->reg[FK01_FP03] |= 1u << 4; break;  /* key input 0 */
	case 56: case 57: case 58:
	case 60: case 61: case 62:
		/* error/rx/tx are bits 0/1/2 for ch0 and 3/4/5 for ch1. */
		t->reg[FSIF01] |= 1u << (vector < 59 ? vector - 56
							 : vector - 57);
		break;
	default: break;
	}
}

/* Reset state without re-registering the device. */
void itc_reset(struct itc *t)
{
	/* Deterministic choice, not a hardware guarantee: interrupt causes
	 * including FDMA are indeterminate at reset (III-2-42, II-1-46).
	 * Firmware initialization tests must also exercise set cause bits. */
	memset(t->reg, 0, sizeof t->reg);
	t->writes = 0;
	/* DENONLY, IDMAONLY and RSTONLY all reset set (manual 0x30029f). */
	t->reg[RST_RESET] = 0x07;
}

void itc_attach(struct mem *m, struct itc *t)
{
	memset(t, 0, sizeof *t);
	itc_reset(t);
	mem_add_mmio(m, "itc", ITC_BASE, ITC_LEN, itc_mmio, t);
}
